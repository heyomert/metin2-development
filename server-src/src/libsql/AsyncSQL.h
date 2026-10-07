#ifndef __INC_METIN_II_ASYNCSQL_H__
#define __INC_METIN_II_ASYNCSQL_H__

#include "libthecore/stdafx.h"
#include "libthecore/log.h"

#include <string>
#include <queue>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

#include <mysql.h>
#include <errmsg.h>
#include <mysqld_error.h>

#define QUERY_MAX_LEN 8192

// Modern RAII wrapper for MySQL results
struct SQLResult
{
	SQLResult() noexcept
		: pSQLResult(nullptr), uiNumRows(0), uiAffectedRows(0), uiInsertID(0)
	{
	}

	~SQLResult()
	{
		if (pSQLResult)
		{
			mysql_free_result(pSQLResult);
			pSQLResult = nullptr;
		}
	}

	// Delete copy constructor and assignment operator (non-copyable)
	SQLResult(const SQLResult&) = delete;
	SQLResult& operator=(const SQLResult&) = delete;

	// Allow move semantics
	SQLResult(SQLResult&& other) noexcept
		: pSQLResult(other.pSQLResult),
		uiNumRows(other.uiNumRows),
		uiAffectedRows(other.uiAffectedRows),
		uiInsertID(other.uiInsertID)
	{
		other.pSQLResult = nullptr;
		other.uiNumRows = 0;
		other.uiAffectedRows = 0;
		other.uiInsertID = 0;
	}

	SQLResult& operator=(SQLResult&& other) noexcept
	{
		if (this != &other)
		{
			if (pSQLResult)
			{
				mysql_free_result(pSQLResult);
			}

			pSQLResult = other.pSQLResult;
			uiNumRows = other.uiNumRows;
			uiAffectedRows = other.uiAffectedRows;
			uiInsertID = other.uiInsertID;

			other.pSQLResult = nullptr;
			other.uiNumRows = 0;
			other.uiAffectedRows = 0;
			other.uiInsertID = 0;
		}
		return *this;
	}

	MYSQL_RES*	pSQLResult;
	uint32_t	uiNumRows;
	uint32_t	uiAffectedRows;
	uint32_t	uiInsertID;
};

// What happened to a statement, and what was done about it: two independent axes
// (docs/engineering/db-step2-asyncsql-fix.md, section 1b).
enum class ESQLResult : uint8_t
{
	APPLIED,			// executed without error
	NOT_DELIVERED,		// failed while sending: the whole request never reached the server (not executed)
	ROLLED_BACK,		// server error whose rollback is proven for this statement family (no family is marked yet)
	AMBIGUOUS,			// failed after the whole request was sent: it may or may not have been executed
	PERMANENT,			// server rejected the statement (syntax, schema, constraint, privilege, ...)
	UNEXECUTED_AT_QUIT,	// never attempted: the worker stopped (database unreachable at quit, or queued after quit)
};

enum class ESQLPolicy : uint8_t
{
	NONE,
	TRANSIENT_CONNECTION,				// database unreachable: retried in place
	RETRYABLE_AFTER_PROVEN_ROLLBACK,	// reserved for marked statement families (none in step 2a)
	QUERY_PERMANENT,					// this statement will not succeed by repeating it
	CONNECTION_CONFIG_FATAL,			// credentials, database, host or unknown connection error: not retried
	RESOURCE_LIMIT,						// connection/resource limits; may be temporary, not retried in step 2a
	INTERNAL_PROTOCOL_STATE,			// client protocol state (commands out of sync, malformed packet, out of memory)
	AMBIGUOUS_NO_RETRY,					// outcome unknown: never repeated blindly
};

enum class ESQLPhase : uint8_t
{
	NONE,
	SEND,	// mysql_send_query failed: the request was not completely written
	READ,	// mysql_read_query_result failed: the request was completely written
};

const char* SQLResultName(ESQLResult r);
const char* SQLPolicyName(ESQLPolicy p);
const char* SQLPhaseName(ESQLPhase p);

// SQL Message with improved memory management
typedef struct _SQLMsg
{
	_SQLMsg() noexcept
		: m_pkSQL(nullptr), iID(0), uiResultPos(0), pvUserData(nullptr),
		bReturn(false), uiSQLErrno(0), uiFinalErrno(0), llEnqueueMs(0),
		eResult(ESQLResult::APPLIED), ePolicy(ESQLPolicy::NONE), ePhase(ESQLPhase::NONE), uiAttempts(0)
	{
	}

	~_SQLMsg()
	{
		vec_pkResult.clear();
	}

	// Delete copy operations
	_SQLMsg(const _SQLMsg&) = delete;
	_SQLMsg& operator=(const _SQLMsg&) = delete;

	// Allow move operations
	_SQLMsg(_SQLMsg&&) noexcept = default;
	_SQLMsg& operator=(_SQLMsg&&) noexcept = default;

	// After an applied attempt; pFirst = the result set the attempt already read (CAsyncSQL::Attempt), or NULL for a
	// statement without one. Rows that did not arrive made the attempt fail, so this is never "applied with no rows".
	// Same order as before: result set first, then insert id and affected rows (for a SELECT, affected rows = row count).
	void StoreApplied(MYSQL_RES* pFirst)
	{
		bool bFirst = true;
		do
		{
			auto pRes = std::make_unique<SQLResult>();

			// Further result sets come only from stored procedures; with CLIENT_MULTI_STATEMENTS off nothing sends one
			pRes->pSQLResult = bFirst ? pFirst : mysql_store_result(m_pkSQL);
			bFirst = false;
			pRes->uiInsertID = static_cast<uint32_t>(mysql_insert_id(m_pkSQL));
			pRes->uiAffectedRows = static_cast<uint32_t>(mysql_affected_rows(m_pkSQL));
			pRes->uiNumRows = pRes->pSQLResult ? static_cast<uint32_t>(mysql_num_rows(pRes->pSQLResult)) : 0;

			vec_pkResult.push_back(std::move(pRes));
		} while (mysql_next_result(m_pkSQL) == 0);
	}

	// After a failed attempt, or none: an empty result, so Get() keeps working (no rows, nothing affected, no insert id).
	// Nothing is read from the connection: after a failure Connector leaves affected rows at ~0 and the insert id of the
	// previous successful INSERT, which callers would take for success (step 2a, A-17).
	void StoreFailed()
	{
		vec_pkResult.push_back(std::make_unique<SQLResult>());
	}

	SQLResult* Get()
	{
		if (uiResultPos >= vec_pkResult.size())
			return nullptr;

		return vec_pkResult[uiResultPos].get();
	}

	bool Next()
	{
		if (uiResultPos + 1 >= vec_pkResult.size())
			return false;

		++uiResultPos;
		return true;
	}

	MYSQL*								m_pkSQL;
	int									iID;			// correlation id in logs and the failure ledger
	std::string							stQuery;
	std::vector<std::unique_ptr<SQLResult>>	vec_pkResult;
	unsigned int						uiResultPos;
	void*								pvUserData;
	bool								bReturn;
	unsigned int						uiSQLErrno;		// error of the attempt that finished the message: 0 = applied; never 0 on failure (2000 if none)
	unsigned int						uiFinalErrno;	// same as uiSQLErrno (kept for the step 1c telemetry readers)
	int64_t								llEnqueueMs;	// SQLStatsClock at queueing (telemetry only)
	ESQLResult							eResult;
	ESQLPolicy							ePolicy;
	ESQLPhase							ePhase;
	uint32_t							uiAttempts;
} SQLMsg;

// Telemetry for one CAsyncSQL (docs/monitoring.md -> "SQL (sql_*.log)"). Counters are cumulative since construction
// and only ever grow; snapshot fields describe the moment of collection. Nothing here changes how queries run.
enum ESQLErrnoBucket
{
	SQL_ERRNO_2006,		// CR_SERVER_GONE_ERROR
	SQL_ERRNO_2013,		// CR_SERVER_LOST
	SQL_ERRNO_2014,		// CR_COMMANDS_OUT_OF_SYNC
	SQL_ERRNO_1205,		// ER_LOCK_WAIT_TIMEOUT
	SQL_ERRNO_1213,		// ER_LOCK_DEADLOCK
	SQL_ERRNO_OTHER,
	SQL_ERRNO_BUCKET_MAX,
};

struct SQLStats
{
	bool		configured = false;		// Setup() was called (a connection this process actually uses)
	bool		threaded = false;		// has a worker thread (AsyncQuery/ReturnQuery); false = DirectQuery only
	bool		workerRunning = false;	// worker thread is inside its loop (false if its first connect failed)

	// cumulative
	uint64_t	pushed = 0;				// queued by AsyncQuery/ReturnQuery
	uint64_t	ok = 0;					// completed, last attempt without error
	uint64_t	err = 0;				// completed, last attempt failed: that statement was not (or maybe not) applied
	uint64_t	retry = 0;				// failed attempts retried in place (TRANSIENT_CONNECTION only)
	uint64_t	reconnectSeen = 0;		// connection thread id changed (reconnect noticed, not when it happened)
	uint64_t	errnoCount[SQL_ERRNO_BUCKET_MAX] = {};	// failed attempts by error code; sum = err + retry
	uint64_t	execCount = 0;			// completed messages with a timed last attempt
	uint64_t	execUsTotal = 0;		// threaded: steady_clock us; direct: SQLStatsClock ms * 1000 (coarse)
	uint64_t	execUsMax = 0;			// largest single last attempt since the previous CollectStats()

	// step 2a: completed failures by result and by policy, attempt failures by phase (all cumulative)
	uint64_t	resNotDelivered = 0, resRolledBack = 0, resAmbiguous = 0, resPermanent = 0, resUnexecutedAtQuit = 0;
	uint64_t	polTransient = 0, polConfigFatal = 0, polResourceLimit = 0, polInternal = 0, polQueryPermanent = 0,
				polAmbiguous = 0;
	uint64_t	phaseSendFail = 0, phaseReadFail = 0;
	uint64_t	sessionCheckFail = 0;	// connect/reconnect found autocommit != 1 or an unexpected character set
	uint64_t	logSuppressed = 0;		// syserr lines not written (same failure state repeated); every case is counted
	uint64_t	failureEvents = 0;		// completed failures handed to the failure hook (ledger)

	// snapshot
	uint64_t	queued = 0;				// main queue
	uint64_t	copied = 0;				// worker's copy queue (includes a stuck head)
	uint64_t	results = 0;			// result queue (ReturnQuery results not popped yet)
	uint64_t	queuedBytes = 0;		// SQL text bytes waiting in the main and copy queues
	int64_t		oldestAgeMs = 0;		// age of the oldest message not completed yet (0 = none)
	int64_t		stuckMs = 0;			// head message failing and waiting for another attempt (0 = not stuck)
	uint64_t	unexecutedAtQuit = 0;	// messages never attempted because the worker stopped (cumulative)
};

// Cheap monotonic milliseconds for telemetry timestamps (FreeBSD CLOCK_MONOTONIC_FAST, Linux CLOCK_MONOTONIC_COARSE;
// a few ms of granularity). Never 0 after start-up, so 0 can mean "none".
int64_t SQLStatsClock();

// One completed failure, for the process' failure ledger (metadata only: no SQL text, no values, no identities).
// Called on the worker thread (or DirectQuery's caller thread); must not block.
struct SQLFailureEvent
{
	const char*	label;		// connection label (owner.target.role), "" if not set
	const char*	family;		// SQLFamily() label, never a value
	int			iID;
	unsigned	uiErrno;
	ESQLPhase	ePhase;
	ESQLResult	eResult;
	ESQLPolicy	ePolicy;
	uint32_t	uiAttempts;
	int64_t		llAgeMs;	// since queueing (0 for DirectQuery)
};
using SQLFailureHook = void (*)(const SQLFailureEvent&);

class CAsyncSQL
{
	public:
		CAsyncSQL();
		virtual ~CAsyncSQL();

		void Quit();

		bool Setup(const char* c_pszHost, const char* c_pszUser, const char* c_pszPassword,
			const char* c_pszDB, const char* c_pszLocale, bool bNoThread = false, int iPort = 0);
		bool Setup(CAsyncSQL* sql, bool bNoThread = false);

		bool Connect();
		bool IsConnected() const { return m_bConnected.load(std::memory_order_acquire); }
		bool QueryLocaleSet();

		void AsyncQuery(const char* c_pszQuery);
		void ReturnQuery(const char* c_pszQuery, void* pvUserData);
		std::unique_ptr<SQLMsg> DirectQuery(const char* c_pszQuery);

		DWORD CountQuery();
		DWORD CountResult();
		// Messages not completed yet: main queue + the worker's copy queue (CountQuery() sees only the main queue)
		DWORD CountPending() const;

		void PushResult(std::unique_ptr<SQLMsg> p);
		bool PopResult(std::unique_ptr<SQLMsg>& p);

		// Legacy API compatibility - deprecated, use PopResult(unique_ptr&) instead
		bool PopResult(SQLMsg** pp);

		void ChildLoop();

		MYSQL* GetSQLHandle();

		int CountQueryFinished() const;
		void ResetQueryFinished();

		size_t EscapeString(char* dst, size_t dstSize, const char* src, size_t srcSize);

		// Telemetry snapshot. Single collector: it also restarts the execUsMax window. Lock-free; may be called
		// from any thread, values are read one by one (not one consistent group).
		void CollectStats(SQLStats& out);

		// Names this connection in logs and the failure ledger ("owner.target.role"); set once at setup.
		void SetLabel(const char* label);
		const char* GetLabel() const { return m_stLabel.c_str(); }

		// Process-wide failure hook (the ledger); nullptr disables it.
		static void SetFailureHook(SQLFailureHook hook);

	protected:
		void NoteAttemptFailed(unsigned int uiErrno);
		void NoteCompleted(SQLMsg* p, bool bFailed, uint64_t ullExecUs);
		void NoteThreadIdSeen();
		void Destroy();
		void PushQuery(std::unique_ptr<SQLMsg> p);
		bool PeekQuery(SQLMsg** pp);
		bool PopQuery(int iID);
		bool PeekQueryFromCopyQueue(SQLMsg** pp);
		int CopyQuery();
		bool PopQueryFromCopyQueue();

		// step 2a
		// rpFirst: on success, the result set read (owned by the caller; NULL for a statement without one)
		bool Attempt(SQLMsg* p, ESQLPhase& ePhase, unsigned& uiErrno, MYSQL_RES*& rpFirst);
		void DrainResults();
		void CheckThreadId();
		void VerifySession();
		void Finish(ESQLResult eResult, ESQLPolicy ePolicy, ESQLPhase ePhase, unsigned uiErrno, uint64_t ullExecUs,
			MYSQL_RES* pFirst = nullptr);
		void CountOutcome(ESQLResult eResult, ESQLPolicy ePolicy);
		void ReportFailure(const SQLMsg* p, ESQLPhase ePhase, unsigned uiErrno, ESQLResult eResult, ESQLPolicy ePolicy,
			bool bCompleted);
		void ReportRecovered();
		void DrainAtQuit();

	public:
		int GetCopiedQueryCount() const;
		void ResetCopiedQueryCount();
		void AddCopiedQueryCount(int iCopiedQuery);

	protected:
		// MySQL connection
		MYSQL m_hDB;

		// Connection info
		std::string m_stHost;
		std::string m_stUser;
		std::string m_stPassword;
		std::string m_stDB;
		std::string m_stLocale;
		std::string m_stLabel;
		int m_iPort;

		// Thread control
		std::unique_ptr<std::thread> m_thread;
		std::atomic<bool> m_bEnd;
		std::atomic<bool> m_bConnected;

		// Query queues with mutex protection
		std::queue<std::unique_ptr<SQLMsg>> m_queue_query;
		std::queue<std::unique_ptr<SQLMsg>> m_queue_query_copy;
		std::queue<std::unique_ptr<SQLMsg>> m_queue_result;

		std::mutex m_mtxQuery;
		std::mutex m_mtxResult;
		std::condition_variable m_cvQuery;

		// Counters
		std::atomic<int> m_iMsgCount;
		std::atomic<int> m_iQueryFinished;
		std::atomic<int> m_iCopiedQuery;
		std::atomic<unsigned long> m_ulThreadID;

		// Session state (owner thread only: the worker, or the DirectQuery caller)
		bool m_bSessionCheckPending;
		bool m_bLastAttemptOk;

		// syserr state: one line per change of failure state, repeats counted (owner thread only)
		bool m_bInFailure;
		ESQLPolicy m_eLoggedPolicy;
		uint64_t m_ullSuppressedSinceLog;

		// Telemetry (relaxed atomics; never read by query handling)
		std::atomic<bool> m_bConfigured;
		bool m_bThreaded;
		std::atomic<bool> m_bWorkerRunning;
		std::atomic<uint64_t> m_ullPushed;
		std::atomic<uint64_t> m_ullTaken;		// moved from the main queue to the copy queue
		std::atomic<uint64_t> m_ullCopyDone;	// completed from the copy queue
		std::atomic<uint64_t> m_ullMainDone;	// completed straight from the main queue (after the worker stopped)
		std::atomic<uint64_t> m_ullOk;
		std::atomic<uint64_t> m_ullErr;
		std::atomic<uint64_t> m_ullRetry;
		std::atomic<uint64_t> m_ullReconnectSeen;
		std::atomic<uint64_t> m_ullResultPushed;
		std::atomic<uint64_t> m_ullResultPopped;
		std::atomic<uint64_t> m_aullErrno[SQL_ERRNO_BUCKET_MAX];
		std::atomic<uint64_t> m_ullExecCount;
		std::atomic<uint64_t> m_ullExecUsTotal;
		std::atomic<uint64_t> m_ullExecUsMax;
		std::atomic<int64_t> m_llMainHeadMs;	// enqueue time of the main queue's head (0 = empty)
		std::atomic<int64_t> m_llCopyHeadMs;	// enqueue time of the copy queue's head (0 = empty)
		std::atomic<int64_t> m_llStuckSinceMs;
		std::atomic<uint64_t> m_ullUnexecutedAtQuit;
		std::atomic<uint64_t> m_ullBytesPushed;
		std::atomic<uint64_t> m_ullBytesDone;
		std::atomic<uint64_t> m_aullResult[6];	// by ESQLResult
		std::atomic<uint64_t> m_aullPolicy[8];	// by ESQLPolicy
		std::atomic<uint64_t> m_ullPhaseSendFail;
		std::atomic<uint64_t> m_ullPhaseReadFail;
		std::atomic<uint64_t> m_ullSessionCheckFail;
		std::atomic<uint64_t> m_ullLogSuppressed;
		std::atomic<uint64_t> m_ullFailureEvents;
};

class CAsyncSQL2 : public CAsyncSQL
{
	public:
		void SetLocale(const std::string& stLocale);
};

#endif
