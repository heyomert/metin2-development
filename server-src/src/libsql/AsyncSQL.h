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

// SQL Message with improved memory management
typedef struct _SQLMsg
{
	_SQLMsg() noexcept
		: m_pkSQL(nullptr), iID(0), uiResultPos(0), pvUserData(nullptr),
		bReturn(false), uiSQLErrno(0), uiFinalErrno(0), llEnqueueMs(0)
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

	void Store()
	{
		do
		{
			auto pRes = std::make_unique<SQLResult>();

			pRes->pSQLResult = mysql_store_result(m_pkSQL);
			pRes->uiInsertID = static_cast<uint32_t>(mysql_insert_id(m_pkSQL));
			pRes->uiAffectedRows = static_cast<uint32_t>(mysql_affected_rows(m_pkSQL));

			if (pRes->pSQLResult)
			{
				pRes->uiNumRows = static_cast<uint32_t>(mysql_num_rows(pRes->pSQLResult));
			}
			else
			{
				pRes->uiNumRows = 0;
			}

			vec_pkResult.push_back(std::move(pRes));
		} while (mysql_next_result(m_pkSQL) == 0);
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
	int									iID;
	std::string							stQuery;
	std::vector<std::unique_ptr<SQLResult>>	vec_pkResult;
	unsigned int						uiResultPos;
	void*								pvUserData;
	bool								bReturn;
	unsigned int						uiSQLErrno;		// last failed attempt; not cleared when a retry succeeds
	unsigned int						uiFinalErrno;	// attempt that completed the message: 0 = executed without error
	int64_t								llEnqueueMs;	// SQLStatsClock at queueing (telemetry only)
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
	uint64_t	err = 0;				// completed, last attempt failed: that statement was not applied
	uint64_t	retry = 0;				// failed attempts that left the message queued for another attempt
	uint64_t	reconnectSeen = 0;		// connection thread id differed before a query (reconnect noticed, not when it happened)
	uint64_t	errnoCount[SQL_ERRNO_BUCKET_MAX] = {};	// failed attempts by error code; sum = err + retry
	uint64_t	execCount = 0;			// completed messages with a timed last attempt
	uint64_t	execUsTotal = 0;		// threaded: steady_clock us; direct: SQLStatsClock ms * 1000 (coarse)
	uint64_t	execUsMax = 0;			// largest single last attempt since the previous CollectStats()

	// snapshot
	uint64_t	queued = 0;				// main queue
	uint64_t	copied = 0;				// worker's copy queue (includes a stuck head)
	uint64_t	results = 0;			// result queue (ReturnQuery results not popped yet)
	int64_t		oldestAgeMs = 0;		// age of the oldest message not completed yet (0 = none)
	int64_t		stuckMs = 0;			// head message failing and waiting for another attempt (0 = not stuck)
	uint64_t	unexecutedAtQuit = 0;	// left in the copy queue when the worker stopped (never executed)
};

// Cheap monotonic milliseconds for telemetry timestamps (FreeBSD CLOCK_MONOTONIC_FAST, Linux CLOCK_MONOTONIC_COARSE;
// a few ms of granularity). Never 0 after start-up, so 0 can mean "none".
int64_t SQLStatsClock();

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

		// Telemetry (relaxed atomics; never read by query handling)
		std::atomic<bool> m_bConfigured;
		bool m_bThreaded;
		std::atomic<bool> m_bWorkerRunning;
		std::atomic<uint64_t> m_ullPushed;
		std::atomic<uint64_t> m_ullTaken;		// moved from the main queue to the copy queue
		std::atomic<uint64_t> m_ullCopyDone;	// completed from the copy queue (normal loop)
		std::atomic<uint64_t> m_ullMainDone;	// completed straight from the main queue (shutdown loop)
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
};

class CAsyncSQL2 : public CAsyncSQL
{
	public:
		void SetLocale(const std::string& stLocale);
};

#endif
