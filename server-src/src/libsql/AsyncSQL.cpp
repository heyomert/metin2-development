#ifndef OS_WINDOWS
#include <sys/time.h>
#endif

#include <cstdlib>
#include <cstring>
#include <chrono>

#include "AsyncSQL.h"
#include "SQLFamily.h"

#include <ctime>

int64_t SQLStatsClock()
{
#if defined(CLOCK_MONOTONIC_FAST) || defined(CLOCK_MONOTONIC_COARSE)
	// Tick-granular kernel clock: ~63 ns per read on the test VM against ~12 us for CLOCK_MONOTONIC
	// (docs/engineering/db-step1-measurement.md). Only used for ages in ms.
#if defined(CLOCK_MONOTONIC_FAST)
	const clockid_t clk = CLOCK_MONOTONIC_FAST;
#else
	const clockid_t clk = CLOCK_MONOTONIC_COARSE;
#endif
	struct timespec ts;
	if (clock_gettime(clk, &ts) != 0)
		return 1;
	return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000 + 1;
#else
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count() + 1;
#endif
}

const char* SQLResultName(ESQLResult r)
{
	switch (r)
	{
		case ESQLResult::APPLIED:				return "applied";
		case ESQLResult::NOT_DELIVERED:			return "not_delivered";
		case ESQLResult::ROLLED_BACK:			return "rolled_back";
		case ESQLResult::AMBIGUOUS:				return "ambiguous";
		case ESQLResult::PERMANENT:				return "permanent";
		case ESQLResult::UNEXECUTED_AT_QUIT:	return "unexecuted_at_quit";
	}
	return "unknown";
}

const char* SQLPolicyName(ESQLPolicy p)
{
	switch (p)
	{
		case ESQLPolicy::NONE:								return "none";
		case ESQLPolicy::TRANSIENT_CONNECTION:				return "transient_connection";
		case ESQLPolicy::RETRYABLE_AFTER_PROVEN_ROLLBACK:	return "retryable_after_proven_rollback";
		case ESQLPolicy::QUERY_PERMANENT:					return "query_permanent";
		case ESQLPolicy::CONNECTION_CONFIG_FATAL:			return "connection_config_fatal";
		case ESQLPolicy::RESOURCE_LIMIT:					return "resource_limit";
		case ESQLPolicy::INTERNAL_PROTOCOL_STATE:			return "internal_protocol_state";
		case ESQLPolicy::AMBIGUOUS_NO_RETRY:				return "ambiguous_no_retry";
	}
	return "unknown";
}

const char* SQLPhaseName(ESQLPhase p)
{
	switch (p)
	{
		case ESQLPhase::NONE:	return "none";
		case ESQLPhase::SEND:	return "send";
		case ESQLPhase::READ:	return "read";
	}
	return "unknown";
}

namespace
{
	// Process-wide failure hook (the ledger), set once at start-up and cleared before it goes away
	std::atomic<SQLFailureHook> g_failureHook{ nullptr };

	// Retry interval of the TRANSIENT_CONNECTION path: the value the code used before step 2a (not a tuned number)
	constexpr std::chrono::milliseconds TRANSIENT_RETRY_INTERVAL(100);

	ESQLErrnoBucket ErrnoBucket(unsigned int uiErrno)
	{
		switch (uiErrno)
		{
			case CR_SERVER_GONE_ERROR:		return SQL_ERRNO_2006;
			case CR_SERVER_LOST:			return SQL_ERRNO_2013;
			case CR_COMMANDS_OUT_OF_SYNC:	return SQL_ERRNO_2014;
			case ER_LOCK_WAIT_TIMEOUT:		return SQL_ERRNO_1205;
			case ER_LOCK_DEADLOCK:			return SQL_ERRNO_1213;
			default:						return SQL_ERRNO_OTHER;
		}
	}

	// Release, with acquire loads in CollectStats: a collector that sees a later stage (a completion) also sees the
	// earlier ones (take, push), so derived queue lengths never go negative. Same instruction as relaxed on x86-64.
	inline void Bump(std::atomic<uint64_t>& counter, uint64_t value = 1)
	{
		counter.fetch_add(value, std::memory_order_release);
	}

	// docs/engineering/db-step2-asyncsql-fix.md, section 1b. The phase comes from the API that failed
	// (mysql_send_query or mysql_read_query_result), not from the error number.
	void Classify(ESQLPhase ePhase, unsigned e, ESQLResult& eResult, ESQLPolicy& ePolicy)
	{
		if (ePhase == ESQLPhase::SEND)
		{
			// The request was not completely written (Connector/C 3.4.5 send path + server test P1/P1b/P1c)
			eResult = ESQLResult::NOT_DELIVERED;
			switch (e)
			{
				case CR_CONNECTION_ERROR:		// server unreachable (probe P3, G1)
				case CR_SERVER_GONE_ERROR:		// write failed after reconnecting
				case CR_SERVER_LOST:			// lost during the reconnect handshake
					ePolicy = ESQLPolicy::TRANSIENT_CONNECTION;
					return;
				case ER_CON_COUNT_ERROR:		// 1040 (probe C3)
				case ER_TOO_MANY_USER_CONNECTIONS:	// 1203
				case ER_USER_LIMIT_REACHED:		// 1226 (probe C4)
					ePolicy = ESQLPolicy::RESOURCE_LIMIT;
					return;
				case CR_COMMANDS_OUT_OF_SYNC:
				case CR_MALFORMED_PACKET:
				case CR_OUT_OF_MEMORY:
				case CR_UNKNOWN_ERROR:
					ePolicy = ESQLPolicy::INTERNAL_PROTOCOL_STATE;
					return;
				case CR_NET_PACKET_TOO_LARGE:	// this statement is too large
					ePolicy = ESQLPolicy::QUERY_PERMANENT;
					return;
				default:						// 1045 (C1), 1049 (C2), 1044, 1129, 1130, 2005, 2059 and anything not listed
					ePolicy = ESQLPolicy::CONNECTION_CONFIG_FATAL;
					return;
			}
		}

		// The whole request was written: a lost connection leaves the outcome unknown (probe P2: executed)
		switch (e)
		{
			case CR_COMMANDS_OUT_OF_SYNC:
			case CR_MALFORMED_PACKET:
			case CR_OUT_OF_MEMORY:
				eResult = ESQLResult::AMBIGUOUS;
				ePolicy = ESQLPolicy::INTERNAL_PROTOCOL_STATE;
				return;
			case ER_NET_READ_ERROR:
			case ER_NET_READ_INTERRUPTED:
			case ER_NET_ERROR_ON_WRITE:
			case ER_NET_WRITE_INTERRUPTED:
			case ER_CONNECTION_KILLED:
			case ER_SERVER_SHUTDOWN:
			case ER_QUERY_INTERRUPTED:
			case ER_STATEMENT_TIMEOUT:
			case ER_LOCK_WAIT_TIMEOUT:		// rollback proven only for tested families (R1-R5); none is marked yet
			case ER_LOCK_DEADLOCK:
				eResult = ESQLResult::AMBIGUOUS;
				ePolicy = ESQLPolicy::AMBIGUOUS_NO_RETRY;
				return;
			default:
				break;
		}

		if (e >= CR_MIN_ERROR && e <= CR_MAX_ERROR)	// any other client-side error after a complete send
		{
			eResult = ESQLResult::AMBIGUOUS;
			ePolicy = ESQLPolicy::AMBIGUOUS_NO_RETRY;
			return;
		}

		eResult = ESQLResult::PERMANENT;
		ePolicy = ESQLPolicy::QUERY_PERMANENT;
	}

	bool IsConnectionLevel(ESQLPolicy p)
	{
		return p == ESQLPolicy::TRANSIENT_CONNECTION || p == ESQLPolicy::CONNECTION_CONFIG_FATAL ||
			p == ESQLPolicy::RESOURCE_LIMIT || p == ESQLPolicy::INTERNAL_PROTOCOL_STATE;
	}
}

CAsyncSQL::CAsyncSQL()
	: m_stHost(""), m_stUser(""), m_stPassword(""), m_stDB(""), m_stLocale(""),
	m_iPort(0), m_thread(nullptr), m_bEnd(false), m_bConnected(false),
	m_iMsgCount(0), m_iQueryFinished(0), m_iCopiedQuery(0), m_ulThreadID(0),
	m_bSessionCheckPending(false), m_bLastAttemptOk(true),
	m_bInFailure(false), m_eLoggedPolicy(ESQLPolicy::NONE), m_ullSuppressedSinceLog(0),
	m_bConfigured(false), m_bThreaded(false), m_bWorkerRunning(false), m_ullPushed(0), m_ullTaken(0), m_ullCopyDone(0), m_ullMainDone(0),
	m_ullOk(0), m_ullErr(0), m_ullRetry(0), m_ullReconnectSeen(0), m_ullResultPushed(0), m_ullResultPopped(0),
	m_ullExecCount(0), m_ullExecUsTotal(0), m_ullExecUsMax(0), m_llMainHeadMs(0), m_llCopyHeadMs(0),
	m_llStuckSinceMs(0), m_ullUnexecutedAtQuit(0), m_ullBytesPushed(0), m_ullBytesDone(0),
	m_ullPhaseSendFail(0), m_ullPhaseReadFail(0), m_ullSessionCheckFail(0), m_ullLogSuppressed(0), m_ullFailureEvents(0)
{
	memset(&m_hDB, 0, sizeof(m_hDB));
	for (auto& c : m_aullErrno)
		c.store(0, std::memory_order_relaxed);
	for (auto& c : m_aullResult)
		c.store(0, std::memory_order_relaxed);
	for (auto& c : m_aullPolicy)
		c.store(0, std::memory_order_relaxed);
}

void CAsyncSQL::SetFailureHook(SQLFailureHook hook)
{
	g_failureHook.store(hook, std::memory_order_release);
}

void CAsyncSQL::SetLabel(const char* label)
{
	m_stLabel = label ? label : "";
}

void CAsyncSQL::NoteAttemptFailed(unsigned int uiErrno)
{
	Bump(m_aullErrno[ErrnoBucket(uiErrno)]);
}

void CAsyncSQL::NoteCompleted(SQLMsg* p, bool bFailed, uint64_t ullExecUs)
{
	p->uiFinalErrno = bFailed ? p->uiSQLErrno : 0;
	Bump(bFailed ? m_ullErr : m_ullOk);

	if (ullExecUs == UINT64_MAX)	// not timed
		return;

	Bump(m_ullExecCount);
	Bump(m_ullExecUsTotal, ullExecUs);

	uint64_t cur = m_ullExecUsMax.load(std::memory_order_relaxed);
	while (cur < ullExecUs && !m_ullExecUsMax.compare_exchange_weak(cur, ullExecUs, std::memory_order_relaxed))
	{
	}
}

void CAsyncSQL::NoteThreadIdSeen()
{
	Bump(m_ullReconnectSeen);
}

void CAsyncSQL::CountOutcome(ESQLResult eResult, ESQLPolicy ePolicy)
{
	Bump(m_aullResult[static_cast<int>(eResult)]);
	if (eResult != ESQLResult::APPLIED)
		Bump(m_aullPolicy[static_cast<int>(ePolicy)]);
}

void CAsyncSQL::CollectStats(SQLStats& out)
{
	const auto get = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_acquire); };

	out.configured = m_bConfigured.load(std::memory_order_relaxed);
	out.threaded = m_bThreaded;
	out.workerRunning = m_bWorkerRunning.load(std::memory_order_relaxed);

	// Completions first, then what feeds them: a message finishing between two loads can then only make a
	// queue look one message longer, never negative.
	out.ok = get(m_ullOk);
	out.err = get(m_ullErr);
	const uint64_t copyDone = get(m_ullCopyDone);
	const uint64_t mainDone = get(m_ullMainDone);
	const uint64_t bytesDone = get(m_ullBytesDone);
	const uint64_t taken = get(m_ullTaken);
	out.pushed = get(m_ullPushed);
	const uint64_t bytesPushed = get(m_ullBytesPushed);
	const uint64_t resultPopped = get(m_ullResultPopped);
	const uint64_t resultPushed = get(m_ullResultPushed);

	out.retry = get(m_ullRetry);
	out.reconnectSeen = get(m_ullReconnectSeen);
	for (int i = 0; i < SQL_ERRNO_BUCKET_MAX; ++i)
		out.errnoCount[i] = get(m_aullErrno[i]);
	out.execCount = get(m_ullExecCount);
	out.execUsTotal = get(m_ullExecUsTotal);
	out.execUsMax = m_ullExecUsMax.exchange(0, std::memory_order_relaxed);

	out.resNotDelivered = get(m_aullResult[static_cast<int>(ESQLResult::NOT_DELIVERED)]);
	out.resRolledBack = get(m_aullResult[static_cast<int>(ESQLResult::ROLLED_BACK)]);
	out.resAmbiguous = get(m_aullResult[static_cast<int>(ESQLResult::AMBIGUOUS)]);
	out.resPermanent = get(m_aullResult[static_cast<int>(ESQLResult::PERMANENT)]);
	out.resUnexecutedAtQuit = get(m_aullResult[static_cast<int>(ESQLResult::UNEXECUTED_AT_QUIT)]);
	out.polTransient = get(m_aullPolicy[static_cast<int>(ESQLPolicy::TRANSIENT_CONNECTION)]);
	out.polConfigFatal = get(m_aullPolicy[static_cast<int>(ESQLPolicy::CONNECTION_CONFIG_FATAL)]);
	out.polResourceLimit = get(m_aullPolicy[static_cast<int>(ESQLPolicy::RESOURCE_LIMIT)]);
	out.polInternal = get(m_aullPolicy[static_cast<int>(ESQLPolicy::INTERNAL_PROTOCOL_STATE)]);
	out.polQueryPermanent = get(m_aullPolicy[static_cast<int>(ESQLPolicy::QUERY_PERMANENT)]);
	out.polAmbiguous = get(m_aullPolicy[static_cast<int>(ESQLPolicy::AMBIGUOUS_NO_RETRY)]);
	out.phaseSendFail = get(m_ullPhaseSendFail);
	out.phaseReadFail = get(m_ullPhaseReadFail);
	out.sessionCheckFail = get(m_ullSessionCheckFail);
	out.logSuppressed = get(m_ullLogSuppressed);
	out.failureEvents = get(m_ullFailureEvents);

	out.queued = out.pushed >= taken + mainDone ? out.pushed - taken - mainDone : 0;
	out.copied = taken >= copyDone ? taken - copyDone : 0;
	out.results = resultPushed >= resultPopped ? resultPushed - resultPopped : 0;
	out.queuedBytes = bytesPushed >= bytesDone ? bytesPushed - bytesDone : 0;

	const int64_t now = SQLStatsClock();
	const int64_t copyHead = m_llCopyHeadMs.load(std::memory_order_relaxed);
	const int64_t mainHead = m_llMainHeadMs.load(std::memory_order_relaxed);
	const int64_t oldest = copyHead ? copyHead : mainHead;	// FIFO: the copy queue's head is older than the main queue's
	out.oldestAgeMs = oldest && now > oldest ? now - oldest : 0;
	const int64_t stuck = m_llStuckSinceMs.load(std::memory_order_relaxed);
	out.stuckMs = stuck && now > stuck ? now - stuck : 0;
	out.unexecutedAtQuit = get(m_ullUnexecutedAtQuit);
}

CAsyncSQL::~CAsyncSQL()
{
	Quit();
	Destroy();
}

void CAsyncSQL::Destroy()
{
	if (m_hDB.host)
	{
		sys_log(0, "AsyncSQL: closing mysql connection.");
		mysql_close(&m_hDB);
		m_hDB.host = nullptr;
	}
}

bool CAsyncSQL::QueryLocaleSet()
{
	if (m_stLocale.empty())
	{
		sys_err("m_stLocale == 0");
		return true;
	}

	if (m_stLocale == "ascii")
	{
		sys_err("m_stLocale == ascii");
		return true;
	}

	if (mysql_set_character_set(&m_hDB, m_stLocale.c_str()))
	{
		sys_err("cannot set locale %s by 'mysql_set_character_set', errno %u %s",
			m_stLocale.c_str(), mysql_errno(&m_hDB), mysql_error(&m_hDB));
		return false;
	}

	sys_log(0, "\t--mysql_set_character_set(%s)", m_stLocale.c_str());
	return true;
}

bool CAsyncSQL::Connect()
{
	if (mysql_init(&m_hDB) == nullptr)
	{
		fprintf(stderr, "mysql_init failed\n");
		return false;
	}

	if (!m_stLocale.empty())
	{
		if (mysql_options(&m_hDB, MYSQL_SET_CHARSET_NAME, m_stLocale.c_str()) != 0)
		{
			fprintf(stderr, "mysql_option failed : MYSQL_SET_CHARSET_NAME %s ", mysql_error(&m_hDB));
		}
	}

	// One statement per call: with CLIENT_MULTI_STATEMENTS a failing second statement is only reported by
	// mysql_next_result, so a failed statement could be counted as applied (tools/sql-reliability/multistmt.sh).
	// No caller sends more than one statement (step 2a inventory).
	if (!mysql_real_connect(&m_hDB, m_stHost.c_str(), m_stUser.c_str(),
		m_stPassword.c_str(), m_stDB.c_str(), m_iPort, nullptr, 0))
	{
		fprintf(stderr, "mysql_real_connect: %s\n", mysql_error(&m_hDB));
		return false;
	}

	my_bool reconnect = true;
	if (mysql_options(&m_hDB, MYSQL_OPT_RECONNECT, &reconnect) != 0)
	{
		fprintf(stderr, "mysql_option: %s\n", mysql_error(&m_hDB));
	}

	fprintf(stdout, "AsyncSQL: connected to %s (reconnect %d)\n", m_stHost.c_str(), reconnect);

	m_ulThreadID.store(mysql_thread_id(&m_hDB), std::memory_order_release);
	VerifySession();
	m_bConnected.store(true, std::memory_order_release);
	return true;
}

// Autocommit and the character set are what the retry classification and every caller rely on
// (docs/engineering/db-step2-asyncsql-fix.md, sections 2 and 9). Checked once after connecting and once after each
// reconnect; never in a loop. Connector/C restores the character set itself on reconnect (probe K7d).
void CAsyncSQL::VerifySession()
{
	m_bSessionCheckPending = false;

	static const char kCheck[] = "SELECT @@autocommit, @@character_set_client";
	if (mysql_send_query(&m_hDB, kCheck, sizeof(kCheck) - 1) || mysql_read_query_result(&m_hDB))
	{
		m_bSessionCheckPending = true; // not reachable now: checked after the next successful statement
		return;
	}

	std::string autocommit, charset;
	if (MYSQL_RES* res = mysql_store_result(&m_hDB))
	{
		if (MYSQL_ROW row = mysql_fetch_row(res))
		{
			autocommit = row[0] ? row[0] : "";
			charset = row[1] ? row[1] : "";
		}
		mysql_free_result(res);
	}
	DrainResults();

	const bool checkCharset = !m_stLocale.empty() && m_stLocale != "ascii";
	if (autocommit != "1" || (checkCharset && charset != m_stLocale))
	{
		Bump(m_ullSessionCheckFail);
		sys_err("AsyncSQL: session check failed role=%s autocommit=%s charset=%s expected_charset=%s",
			m_stLabel.c_str(), autocommit.c_str(), charset.c_str(), checkCharset ? m_stLocale.c_str() : "-");
	}
}

void CAsyncSQL::CheckThreadId()
{
	const unsigned long cur = mysql_thread_id(&m_hDB);
	if (m_ulThreadID.load(std::memory_order_acquire) == cur)
		return;

	m_ulThreadID.store(cur, std::memory_order_release);
	NoteThreadIdSeen();
	m_bSessionCheckPending = true;
	sys_log(0, "AsyncSQL: reconnected role=%s", m_stLabel.c_str());
}

// Frees any further result sets after the first one was taken (stored procedures; S11)
void CAsyncSQL::DrainResults()
{
	while (mysql_next_result(&m_hDB) == 0)
	{
		if (MYSQL_RES* r = mysql_store_result(&m_hDB))
			mysql_free_result(r);
	}
}

// One attempt, with the failure phase known exactly: mysql_send_query writes the request, mysql_read_query_result
// reads the answer (mysql_real_query would return -1 for some read-phase failures too).
bool CAsyncSQL::Attempt(SQLMsg* p, ESQLPhase& ePhase, unsigned& uiErrno, MYSQL_RES*& rpFirst)
{
	rpFirst = nullptr;
	CheckThreadId();
	if (m_bSessionCheckPending && m_bLastAttemptOk)
		VerifySession();

	++p->uiAttempts;

	if (mysql_send_query(&m_hDB, p->stQuery.c_str(), static_cast<unsigned long>(p->stQuery.length())))
	{
		ePhase = ESQLPhase::SEND;
		uiErrno = mysql_errno(&m_hDB);
		Bump(m_ullPhaseSendFail);
		NoteAttemptFailed(uiErrno);
		m_bLastAttemptOk = false;
		return false;
	}

	// A statement with a result set: its rows are part of the answer. mysql_read_query_result reads only the column
	// definitions; if the rows do not arrive (connection lost, query killed while sending them), mysql_store_result
	// returns NULL and the attempt failed. Without this the caller saw an applied statement with no rows.
	bool bReadFailed = mysql_read_query_result(&m_hDB) != 0;
	if (!bReadFailed && mysql_field_count(&m_hDB) > 0)
	{
		rpFirst = mysql_store_result(&m_hDB);
		bReadFailed = !rpFirst;
	}

	if (bReadFailed)
	{
		ePhase = ESQLPhase::READ;
		uiErrno = mysql_errno(&m_hDB);
		if (!uiErrno)
			uiErrno = CR_UNKNOWN_ERROR;
		Bump(m_ullPhaseReadFail);
		NoteAttemptFailed(uiErrno);
		m_bLastAttemptOk = false;
		CheckThreadId();
		return false;
	}

	ePhase = ESQLPhase::NONE;
	uiErrno = 0;
	m_bLastAttemptOk = true;
	CheckThreadId(); // a silent reconnect inside this statement is checked before the next one
	return true;
}

void CAsyncSQL::ReportFailure(const SQLMsg* p, ESQLPhase ePhase, unsigned uiErrno, ESQLResult eResult, ESQLPolicy ePolicy,
	bool bCompleted)
{
	char family[64];
	SQLFamily(p->stQuery.c_str(), family, sizeof(family));
	const int64_t llAgeMs = p->llEnqueueMs ? SQLStatsClock() - p->llEnqueueMs : 0;

	if (bCompleted)
	{
		if (const SQLFailureHook hook = g_failureHook.load(std::memory_order_acquire))
		{
			const SQLFailureEvent ev{ m_stLabel.c_str(), family, p->iID, uiErrno, ePhase, eResult, ePolicy, p->uiAttempts,
				llAgeMs > 0 ? llAgeMs : 0 };
			hook(ev);
			Bump(m_ullFailureEvents);
		}
	}

	// syserr: one line when the failure state changes; repeats of the same state are only counted
	if (m_bInFailure && m_eLoggedPolicy == ePolicy)
	{
		++m_ullSuppressedSinceLog;
		Bump(m_ullLogSuppressed);
		return;
	}

	sys_err("AsyncSQL: %s role=%s family=%s id=%d phase=%s errno=%u result=%s policy=%s attempts=%u age_ms=%lld",
		bCompleted ? "failed" : "attempt failed, retrying", m_stLabel.c_str(), family, p->iID, SQLPhaseName(ePhase), uiErrno,
		SQLResultName(eResult), SQLPolicyName(ePolicy), p->uiAttempts, static_cast<long long>(llAgeMs > 0 ? llAgeMs : 0));
	m_bInFailure = true;
	m_eLoggedPolicy = ePolicy;
	m_ullSuppressedSinceLog = 0;
}

void CAsyncSQL::ReportRecovered()
{
	if (!m_bInFailure)
		return;

	if (m_ullSuppressedSinceLog > 0 || IsConnectionLevel(m_eLoggedPolicy))
		sys_err("AsyncSQL: recovered role=%s after policy=%s, %llu repeated failure(s) counted but not logged",
			m_stLabel.c_str(), SQLPolicyName(m_eLoggedPolicy), static_cast<unsigned long long>(m_ullSuppressedSinceLog));

	m_bInFailure = false;
	m_eLoggedPolicy = ESQLPolicy::NONE;
	m_ullSuppressedSinceLog = 0;
}

bool CAsyncSQL::Setup(CAsyncSQL* sql, bool bNoThread)
{
	return Setup(sql->m_stHost.c_str(),
		sql->m_stUser.c_str(),
		sql->m_stPassword.c_str(),
		sql->m_stDB.c_str(),
		sql->m_stLocale.c_str(),
		bNoThread,
		sql->m_iPort);
}

bool CAsyncSQL::Setup(const char* c_pszHost, const char* c_pszUser, const char* c_pszPassword,
	const char* c_pszDB, const char* c_pszLocale, bool bNoThread, int iPort)
{
	m_stHost = c_pszHost;
	m_stUser = c_pszUser;
	m_stPassword = c_pszPassword;
	m_stDB = c_pszDB;
	m_iPort = iPort;
	m_bConfigured.store(true, std::memory_order_relaxed);

	if (c_pszLocale)
	{
		m_stLocale = c_pszLocale;
		sys_log(0, "AsyncSQL: locale %s", m_stLocale.c_str());
	}

	if (!bNoThread)
	{
		m_bThreaded = true;

		// Create worker thread using modern C++ thread
		m_thread = std::make_unique<std::thread>([this]() {
			if (!Connect())
				return;
			ChildLoop();
		});

		return true;
	}
	else
	{
		return Connect();
	}
}

void CAsyncSQL::Quit()
{
	m_bEnd.store(true, std::memory_order_release);
	m_cvQuery.notify_all();

	if (m_thread && m_thread->joinable())
	{
		m_thread->join();
		m_thread.reset();

		// Queued after the worker stopped (or the worker never ran: its first connect failed): never attempted
		std::queue<std::unique_ptr<SQLMsg>> left;
		{
			std::lock_guard<std::mutex> lock(m_mtxQuery);
			std::swap(left, m_queue_query);
			m_llMainHeadMs.store(0, std::memory_order_relaxed);
		}
		std::queue<std::unique_ptr<SQLMsg>> copyLeft;
		std::swap(copyLeft, m_queue_query_copy);

		uint64_t n = 0;
		for (auto* q : { &copyLeft, &left })
		{
			const bool fromCopy = q == &copyLeft;
			while (!q->empty())
			{
				SQLMsg* p = q->front().get();
				p->eResult = ESQLResult::UNEXECUTED_AT_QUIT;
				p->uiSQLErrno = p->uiFinalErrno = CR_UNKNOWN_ERROR;
				CountOutcome(ESQLResult::UNEXECUTED_AT_QUIT, ESQLPolicy::NONE);
				Bump(m_ullErr);
				Bump(fromCopy ? m_ullCopyDone : m_ullMainDone);
				Bump(m_ullBytesDone, p->stQuery.size());
				ReportFailure(p, ESQLPhase::NONE, 0, ESQLResult::UNEXECUTED_AT_QUIT, ESQLPolicy::NONE, true);
				q->pop();
				++n;
			}
		}
		m_llCopyHeadMs.store(0, std::memory_order_relaxed);
		if (n)
		{
			Bump(m_ullUnexecutedAtQuit, n);
			sys_err("AsyncSQL: quit role=%s %llu message(s) queued after the worker stopped, not executed", m_stLabel.c_str(),
				static_cast<unsigned long long>(n));
		}
	}
}

std::unique_ptr<SQLMsg> CAsyncSQL::DirectQuery(const char* c_pszQuery)
{
	auto p = std::make_unique<SQLMsg>();
	p->m_pkSQL = &m_hDB;
	p->iID = m_iMsgCount.fetch_add(1, std::memory_order_acq_rel) + 1;
	p->stQuery = c_pszQuery;

	const int64_t llStartMs = SQLStatsClock();

	// No retry and no waiting here: DirectQuery runs on the caller's thread (in game, the main loop)
	ESQLPhase ePhase;
	unsigned uiErrno;
	MYSQL_RES* pFirst;
	const bool bOk = Attempt(p.get(), ePhase, uiErrno, pFirst);

	ESQLResult eResult = ESQLResult::APPLIED;
	ESQLPolicy ePolicy = ESQLPolicy::NONE;
	if (!bOk)
		Classify(ePhase, uiErrno, eResult, ePolicy);

	p->uiSQLErrno = bOk ? 0 : (uiErrno ? uiErrno : CR_UNKNOWN_ERROR); // same rule as Finish()
	p->eResult = eResult;
	p->ePolicy = ePolicy;
	p->ePhase = ePhase;

	// Coarse (clock granularity, a few ms): enough to see DirectQuery calls that hold the calling loop
	const int64_t llElapsedMs = SQLStatsClock() - llStartMs;
	NoteCompleted(p.get(), !bOk, llElapsedMs > 0 ? static_cast<uint64_t>(llElapsedMs) * 1000 : 0);
	CountOutcome(eResult, ePolicy);

	if (bOk)
		ReportRecovered();
	else
		ReportFailure(p.get(), ePhase, uiErrno, eResult, ePolicy, true);

	if (bOk)
		p->StoreApplied(pFirst);
	else
		p->StoreFailed();
	return p;
}

void CAsyncSQL::AsyncQuery(const char* c_pszQuery)
{
	auto p = std::make_unique<SQLMsg>();
	p->m_pkSQL = &m_hDB;
	p->iID = m_iMsgCount.fetch_add(1, std::memory_order_acq_rel) + 1;
	p->stQuery = c_pszQuery;
	p->llEnqueueMs = SQLStatsClock();

	PushQuery(std::move(p));
}

void CAsyncSQL::ReturnQuery(const char* c_pszQuery, void* pvUserData)
{
	auto p = std::make_unique<SQLMsg>();
	p->m_pkSQL = &m_hDB;
	p->iID = m_iMsgCount.fetch_add(1, std::memory_order_acq_rel) + 1;
	p->stQuery = c_pszQuery;
	p->bReturn = true;
	p->pvUserData = pvUserData;
	p->llEnqueueMs = SQLStatsClock();

	PushQuery(std::move(p));
}

void CAsyncSQL::PushResult(std::unique_ptr<SQLMsg> p)
{
	std::lock_guard<std::mutex> lock(m_mtxResult);
	m_queue_result.push(std::move(p));
	Bump(m_ullResultPushed);
}

bool CAsyncSQL::PopResult(std::unique_ptr<SQLMsg>& p)
{
	std::lock_guard<std::mutex> lock(m_mtxResult);

	if (m_queue_result.empty())
		return false;

	p = std::move(m_queue_result.front());
	m_queue_result.pop();
	Bump(m_ullResultPopped);
	return true;
}

// Legacy API for backward compatibility
bool CAsyncSQL::PopResult(SQLMsg** pp)
{
	std::lock_guard<std::mutex> lock(m_mtxResult);

	if (m_queue_result.empty())
		return false;

	*pp = m_queue_result.front().release();
	m_queue_result.pop();
	Bump(m_ullResultPopped);
	return true;
}

void CAsyncSQL::PushQuery(std::unique_ptr<SQLMsg> p)
{
	const uint64_t ullBytes = p->stQuery.size();
	{
		std::lock_guard<std::mutex> lock(m_mtxQuery);
		if (m_queue_query.empty())
			m_llMainHeadMs.store(p->llEnqueueMs, std::memory_order_relaxed);
		m_queue_query.push(std::move(p));
		Bump(m_ullPushed);
		Bump(m_ullBytesPushed, ullBytes);
	}
	m_cvQuery.notify_one();
}

bool CAsyncSQL::PeekQuery(SQLMsg** pp)
{
	std::lock_guard<std::mutex> lock(m_mtxQuery);

	if (m_queue_query.empty())
		return false;

	*pp = m_queue_query.front().get();
	return true;
}

bool CAsyncSQL::PopQuery(int iID)
{
	std::lock_guard<std::mutex> lock(m_mtxQuery);

	if (m_queue_query.empty())
		return false;

	m_queue_query.pop();
	return true;
}

bool CAsyncSQL::PeekQueryFromCopyQueue(SQLMsg** pp)
{
	if (m_queue_query_copy.empty())
		return false;

	*pp = m_queue_query_copy.front().get();
	return true;
}

// Moves everything from the main queue to the worker's copy queue; returns how many messages moved (0 = none)
int CAsyncSQL::CopyQuery()
{
	std::lock_guard<std::mutex> lock(m_mtxQuery);

	if (m_queue_query.empty())
		return 0;

	// Telemetry: what moves now is newer than anything already in the copy queue; publish the copy head before
	// clearing the main head so a collector never sees both empty while messages wait
	if (m_queue_query_copy.empty())
		m_llCopyHeadMs.store(m_queue_query.front()->llEnqueueMs, std::memory_order_relaxed);
	const uint64_t ullMoved = m_queue_query.size();

	while (!m_queue_query.empty())
	{
		m_queue_query_copy.push(std::move(m_queue_query.front()));
		m_queue_query.pop();
	}

	Bump(m_ullTaken, ullMoved);
	m_llMainHeadMs.store(0, std::memory_order_relaxed);

	return static_cast<int>(ullMoved);
}

bool CAsyncSQL::PopQueryFromCopyQueue()
{
	if (m_queue_query_copy.empty())
		return false;

	m_queue_query_copy.pop();
	return true;
}

int CAsyncSQL::GetCopiedQueryCount() const
{
	return m_iCopiedQuery.load(std::memory_order_acquire);
}

void CAsyncSQL::ResetCopiedQueryCount()
{
	m_iCopiedQuery.store(0, std::memory_order_release);
}

void CAsyncSQL::AddCopiedQueryCount(int iCopiedQuery)
{
	m_iCopiedQuery.fetch_add(iCopiedQuery, std::memory_order_acq_rel);
}

DWORD CAsyncSQL::CountQuery()
{
	std::lock_guard<std::mutex> lock(m_mtxQuery);
	return static_cast<DWORD>(m_queue_query.size());
}

DWORD CAsyncSQL::CountPending() const
{
	const uint64_t done = m_ullCopyDone.load(std::memory_order_acquire) + m_ullMainDone.load(std::memory_order_acquire);
	const uint64_t pushed = m_ullPushed.load(std::memory_order_acquire);
	return static_cast<DWORD>(pushed >= done ? pushed - done : 0);
}

DWORD CAsyncSQL::CountResult()
{
	std::lock_guard<std::mutex> lock(m_mtxResult);
	return static_cast<DWORD>(m_queue_result.size());
}

// Modern profiler using chrono
class cProfiler
{
	public:
		cProfiler(int nInterval = 500000)
			: m_nInterval(nInterval)
		{
			Start();
		}

		void Start()
		{
			m_start = std::chrono::steady_clock::now();
		}

		void Stop()
		{
			m_end = std::chrono::steady_clock::now();
		}

		bool IsOk() const
		{
			auto duration = std::chrono::duration_cast<std::chrono::microseconds>(m_end - m_start);
			return duration.count() <= m_nInterval;
		}

		uint64_t GetElapsedUs() const
		{
			auto duration = std::chrono::duration_cast<std::chrono::microseconds>(m_end - m_start);
			return duration.count() > 0 ? static_cast<uint64_t>(duration.count()) : 0;
		}

	private:
		int m_nInterval;
		std::chrono::steady_clock::time_point m_start;
		std::chrono::steady_clock::time_point m_end;
};

// Completes the copy queue's head: result, counters, report, and (ReturnQuery) its result in FIFO order, failures
// included, so a ReturnQuery result is never delivered before every earlier statement has finished (H-1 barrier).
void CAsyncSQL::Finish(ESQLResult eResult, ESQLPolicy ePolicy, ESQLPhase ePhase, unsigned uiErrno, uint64_t ullExecUs,
	MYSQL_RES* pFirst)
{
	auto pMsg = std::move(m_queue_query_copy.front());
	m_queue_query_copy.pop();
	SQLMsg* p = pMsg.get();

	const bool bFailed = eResult != ESQLResult::APPLIED;
	// Callers read uiSQLErrno == 0 as "applied" (db/ClientManagerLogin.cpp:137): a failure without an error code
	// (never attempted) still gets a non-zero one
	p->uiSQLErrno = bFailed ? (uiErrno ? uiErrno : CR_UNKNOWN_ERROR) : 0;
	p->eResult = eResult;
	p->ePolicy = ePolicy;
	p->ePhase = ePhase;

	// Telemetry: before the result is published (PushResult) so its uiFinalErrno is set when the caller reads it
	NoteCompleted(p, bFailed, ullExecUs);
	CountOutcome(eResult, ePolicy);
	if (eResult == ESQLResult::UNEXECUTED_AT_QUIT)
		Bump(m_ullUnexecutedAtQuit);
	Bump(m_ullCopyDone);
	Bump(m_ullBytesDone, p->stQuery.size());
	m_llStuckSinceMs.store(0, std::memory_order_relaxed);
	m_llCopyHeadMs.store(m_queue_query_copy.empty() ? 0 : m_queue_query_copy.front()->llEnqueueMs,
		std::memory_order_relaxed);

	if (bFailed)
		ReportFailure(p, ePhase, uiErrno, eResult, ePolicy, true);
	else
		ReportRecovered();

	if (p->bReturn)
	{
		if (bFailed)
			p->StoreFailed();
		else
			p->StoreApplied(pFirst);
		PushResult(std::move(pMsg));
	}
	else if (!bFailed)
	{
		// A statement returning rows through AsyncQuery must not leave them pending (S11)
		if (pFirst)
			mysql_free_result(pFirst);
		DrainResults();
	}

	m_iQueryFinished.fetch_add(1, std::memory_order_acq_rel);
}

void CAsyncSQL::ChildLoop()
{
	cProfiler profiler(500000); // 0.5 seconds

	m_bWorkerRunning.store(true, std::memory_order_relaxed);

	while (!m_bEnd.load(std::memory_order_acquire))
	{
		// Sleep only when nothing is waiting: a head that still has to be attempted is never left behind (S1)
		if (m_queue_query_copy.empty())
		{
			{
				std::unique_lock<std::mutex> lock(m_mtxQuery);
				m_cvQuery.wait(lock, [this] {
					return !m_queue_query.empty() || m_bEnd.load(std::memory_order_acquire);
				});
				if (m_bEnd.load(std::memory_order_acquire))
					break;
			}

			const int moved = CopyQuery();
			if (moved <= 0)
				continue;
			AddCopiedQueryCount(moved);
		}

		SQLMsg* p = m_queue_query_copy.front().get();

		profiler.Start();
		ESQLPhase ePhase;
		unsigned uiErrno;
		MYSQL_RES* pFirst;
		const bool bOk = Attempt(p, ePhase, uiErrno, pFirst);
		profiler.Stop();

		if (bOk)
		{
			if (!profiler.IsOk())
			{
				char family[64];
				SQLFamily(p->stQuery.c_str(), family, sizeof(family));
				sys_log(0, "AsyncSQL: slow role=%s family=%s id=%d ms=%llu", m_stLabel.c_str(), family, p->iID,
					static_cast<unsigned long long>(profiler.GetElapsedUs() / 1000));
			}
			Finish(ESQLResult::APPLIED, ESQLPolicy::NONE, ESQLPhase::NONE, 0, profiler.GetElapsedUs(), pFirst);
			continue;
		}

		ESQLResult eResult;
		ESQLPolicy ePolicy;
		Classify(ePhase, uiErrno, eResult, ePolicy);

		if (ePolicy == ESQLPolicy::TRANSIENT_CONNECTION)
		{
			// Not delivered and the database is unreachable: the same head again, in order, after the interval.
			// Quit() interrupts the wait; the attempt is counted, the syserr line only on a change of state.
			Bump(m_ullRetry);
			if (m_llStuckSinceMs.load(std::memory_order_relaxed) == 0)
				m_llStuckSinceMs.store(SQLStatsClock(), std::memory_order_relaxed);
			ReportFailure(p, ePhase, uiErrno, eResult, ePolicy, false);

			std::unique_lock<std::mutex> lock(m_mtxQuery);
			m_cvQuery.wait_for(lock, TRANSIENT_RETRY_INTERVAL, [this] { return m_bEnd.load(std::memory_order_acquire); });
			continue;
		}

		Finish(eResult, ePolicy, ePhase, uiErrno, profiler.GetElapsedUs());
	}

	DrainAtQuit();

	m_bWorkerRunning.store(false, std::memory_order_relaxed);
}

// Shutdown: the copy queue first, then the main queue, in order, one attempt each, classified like any attempt.
// Nothing is dropped silently: once the database proves unreachable the rest is not attempted and every message
// is counted and reported as UNEXECUTED_AT_QUIT. No time limit is set here (shutdown policy is a separate decision).
void CAsyncSQL::DrainAtQuit()
{
	bool unreachable = false;
	uint64_t applied = 0, failed = 0, unexecuted = 0;

	for (;;)
	{
		if (m_queue_query_copy.empty())
		{
			const int moved = CopyQuery();
			if (moved <= 0)
				break;
			AddCopiedQueryCount(moved);
		}

		if (unreachable)
		{
			Finish(ESQLResult::UNEXECUTED_AT_QUIT, ESQLPolicy::NONE, ESQLPhase::NONE, 0, UINT64_MAX);
			++unexecuted;
			continue;
		}

		SQLMsg* p = m_queue_query_copy.front().get();
		ESQLPhase ePhase;
		unsigned uiErrno;
		MYSQL_RES* pFirst;
		if (Attempt(p, ePhase, uiErrno, pFirst))
		{
			Finish(ESQLResult::APPLIED, ESQLPolicy::NONE, ESQLPhase::NONE, 0, UINT64_MAX, pFirst);
			++applied;
			continue;
		}

		ESQLResult eResult;
		ESQLPolicy ePolicy;
		Classify(ePhase, uiErrno, eResult, ePolicy);
		if (ePolicy == ESQLPolicy::TRANSIENT_CONNECTION)
			unreachable = true;
		Finish(eResult, ePolicy, ePhase, uiErrno, UINT64_MAX);
		++failed;
	}

	if (applied || failed || unexecuted)
	{
		const char* fmt = "AsyncSQL: quit role=%s drained applied=%llu failed=%llu unexecuted=%llu";
		if (failed || unexecuted)
			sys_err(fmt, m_stLabel.c_str(), static_cast<unsigned long long>(applied), static_cast<unsigned long long>(failed),
				static_cast<unsigned long long>(unexecuted));
		else
			sys_log(0, fmt, m_stLabel.c_str(), static_cast<unsigned long long>(applied), 0ULL, 0ULL);
	}
}

int CAsyncSQL::CountQueryFinished() const
{
	return m_iQueryFinished.load(std::memory_order_acquire);
}

void CAsyncSQL::ResetQueryFinished()
{
	m_iQueryFinished.store(0, std::memory_order_release);
}

MYSQL* CAsyncSQL::GetSQLHandle()
{
	return &m_hDB;
}

size_t CAsyncSQL::EscapeString(char* dst, size_t dstSize, const char* src, size_t srcSize)
{
	if (srcSize == 0)
	{
		memset(dst, 0, dstSize);
		return 0;
	}

	if (dstSize == 0)
		return 0;

	if (dstSize < srcSize * 2 + 1)
	{
		// The source may be a password, a name or a message: its size only, never its content
		sys_err("AsyncSQL: escape buffer too small (dstSize %u srcSize %u)",
			static_cast<unsigned int>(dstSize), static_cast<unsigned int>(srcSize));

		dst[0] = '\0';
		return 0;
	}

	return mysql_real_escape_string(GetSQLHandle(), dst, src, srcSize);
}

void CAsyncSQL2::SetLocale(const std::string& stLocale)
{
	m_stLocale = stLocale;
	QueryLocaleSet();
}
