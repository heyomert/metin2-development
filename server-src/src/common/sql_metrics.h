#pragma once

#include "metrics_writer.h"
#include "build_identity.h"
#include "libsql/AsyncSQL.h"
#include "sql_failure_ledger.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>

// SQL telemetry lines (docs/monitoring.md -> "SQL (sql_*.log)"). Every connection is measured every window; what is
// written:
//   kind=sum   one line per window for the whole process: window deltas (Δ) and worst current values
//   kind=conn  one line per connection, only when that connection shows an anomaly in the window, at start, every
//              PERIODIC and at the final window; counters there are cumulative totals since process start
//              (*_total), so the difference between any two lines of the same pid is right even if lines in
//              between were skipped or dropped.
// Runs on the owning process' main thread (game loop / db loop). No allocation after Add(), no locks, no I/O:
// lines go to a metrics_writer, which queues them for its own worker.
class sql_metrics_reporter
{
public:
	using Clock = std::chrono::steady_clock;

	static constexpr int MAX_CONNS = 16;
	static constexpr int64_t OLDEST_ANOMALY_MS = 1000;	// a message waiting this long is worth a line (BASELINE_PENDING)
	static constexpr auto PERIODIC = std::chrono::minutes(5);

	// Setup only. owner = the object holding the connection, target = which configured database it uses,
	// role = main (worker thread) or direct (caller's thread) / for db: main (ReturnQuery), async, direct.
	void Add(const char* owner, const char* target, const char* role, CAsyncSQL* sql)
	{
		if (m_count >= MAX_CONNS || !sql)
			return;
		Conn& c = m_conns[m_count++];
		c.owner = owner;
		c.target = target;
		c.role = role;
		c.sql = sql;

		// The same name in syserr lines and the failure ledger
		char label[96];
		std::snprintf(label, sizeof(label), "%s.%s.%s", owner, target, role);
		sql->SetLabel(label);
	}

	void Start(Clock::time_point now)
	{
		m_start = m_windowStart = m_lastPeriodic = now;
		m_first = true;
	}

	// extraSum: process-specific Δ fields appended to the sum line (" key=value ..."), may be "".
	// final: last window before exit; every connection gets a line, with unexecuted_at_quit when quitCounted.
	void Window(Clock::time_point now, const char* host, long pid, metrics_writer& out, const char* extraSum,
		bool final = false, bool quitCounted = false)
	{
		char safeHost[64];
		SafeValue(safeHost, sizeof(safeHost), host);
		const long long uptime = (long long) std::chrono::duration_cast<std::chrono::seconds>(now - m_start).count();
		const long long windowMs = (long long) std::chrono::duration_cast<std::chrono::milliseconds>(now - m_windowStart).count();

		const bool periodic = m_first || final || now - m_lastPeriodic >= PERIODIC;
		if (periodic)
			m_lastPeriodic = now;

		Sum sum;
		for (int i = 0; i < m_count; ++i)
		{
			Conn& c = m_conns[i];
			SQLStats cur;
			c.sql->CollectStats(cur);
			if (!cur.configured)
				continue;

			if (cur.execUsMax > c.execMaxSinceLine)
				c.execMaxSinceLine = cur.execUsMax;

			const SQLStats& p = c.prev;
			const uint64_t dErr = cur.err - p.err, dRetry = cur.retry - p.retry, dReconnect = cur.reconnectSeen - p.reconnectSeen;

			++sum.conns;
			sum.reconnect += dReconnect;
			sum.qBytes += cur.queuedBytes;
			sum.resNotDelivered += cur.resNotDelivered - p.resNotDelivered;
			sum.resRolledBack += cur.resRolledBack - p.resRolledBack;
			sum.resAmbiguous += cur.resAmbiguous - p.resAmbiguous;
			sum.resPermanent += cur.resPermanent - p.resPermanent;
			sum.resUnexecuted += cur.resUnexecutedAtQuit - p.resUnexecutedAtQuit;
			sum.polTransient += cur.polTransient - p.polTransient;
			sum.polConfig += cur.polConfigFatal - p.polConfigFatal;
			sum.polResource += cur.polResourceLimit - p.polResourceLimit;
			sum.polInternal += cur.polInternal - p.polInternal;
			sum.polQuery += cur.polQueryPermanent - p.polQueryPermanent;
			sum.polAmbiguous += cur.polAmbiguous - p.polAmbiguous;
			sum.failSend += cur.phaseSendFail - p.phaseSendFail;
			sum.failRead += cur.phaseReadFail - p.phaseReadFail;
			sum.sessionCheckFail += cur.sessionCheckFail - p.sessionCheckFail;
			sum.logSuppressed += cur.logSuppressed - p.logSuppressed;
			sum.failureEvents += cur.failureEvents - p.failureEvents;
			for (int e = 0; e < SQL_ERRNO_BUCKET_MAX; ++e)
				sum.errnoCount[e] += cur.errnoCount[e] - p.errnoCount[e];

			if (cur.threaded)
			{
				sum.q += cur.queued;
				sum.cq += cur.copied;
				sum.rq += cur.results;
				if (cur.oldestAgeMs > sum.oldestMax)
					sum.oldestMax = cur.oldestAgeMs;
				if (cur.stuckMs > 0)
					++sum.stuck;
				if (!cur.workerRunning)
					++sum.workersDown;
				sum.pushed += cur.pushed - p.pushed;
				sum.ok += cur.ok - p.ok;
				sum.err += dErr;
				sum.retry += dRetry;
				sum.execN += cur.execCount - p.execCount;
				sum.execUs += cur.execUsTotal - p.execUsTotal;
				if (cur.execUsMax > sum.execMaxUs)
					sum.execMaxUs = cur.execUsMax;
			}
			else
			{
				sum.directN += (cur.ok - p.ok) + dErr;
				sum.directErr += dErr;
				if (cur.execUsMax / 1000 > sum.directMaxMs)
					sum.directMaxMs = cur.execUsMax / 1000;
			}

			const bool anomaly = dErr || dRetry || dReconnect || cur.stuckMs > 0 || cur.oldestAgeMs >= OLDEST_ANOMALY_MS ||
				(cur.threaded && !cur.workerRunning) || cur.sessionCheckFail != p.sessionCheckFail;
			if (anomaly || periodic)
			{
				const char* reason = final ? "final" : m_first ? "start" : anomaly ? "anomaly" : "periodic";
				WriteConn(out, safeHost, pid, uptime, c, cur, reason, final && quitCounted);
				c.execMaxSinceLine = 0;
			}

			c.prev = cur;
		}

		char line[2048];
		size_t len = Append(line, sizeof(line), 0,
			"schema=1 src=sql kind=sum host=%s pid=%ld uptime_s=%lld window_ms=%lld first=%d"
			" conns=%d q=%llu cq=%llu rq=%llu oldest_ms_max=%lld stuck_conns=%d workers_down=%d"
			" pushed=%llu ok=%llu err=%llu retry=%llu reconnect_seen=%llu",
			safeHost, pid, uptime, windowMs, m_first ? 1 : 0,
			sum.conns, U(sum.q), U(sum.cq), U(sum.rq), (long long) sum.oldestMax, sum.stuck, sum.workersDown,
			U(sum.pushed), U(sum.ok), U(sum.err), U(sum.retry), U(sum.reconnect));
		len = AppendErrno(line, sizeof(line), len, sum.errnoCount, "");
		len = Append(line, sizeof(line), len,
			" exec_n=%llu exec_us=%llu exec_max_us=%llu direct_n=%llu direct_err=%llu direct_max_ms=%llu%s",
			U(sum.execN), U(sum.execUs), U(sum.execMaxUs), U(sum.directN), U(sum.directErr), U(sum.directMaxMs),
			extraSum ? extraSum : "");
		// Step 2a (docs/monitoring.md): queue bytes, failed messages by result and by policy, failed attempts by phase
		len = Append(line, sizeof(line), len,
			" q_bytes=%llu res_not_delivered=%llu res_rolled_back=%llu res_ambiguous=%llu res_permanent=%llu"
			" res_unexecuted_at_quit=%llu pol_transient=%llu pol_config_fatal=%llu pol_resource_limit=%llu"
			" pol_internal=%llu pol_query_permanent=%llu pol_ambiguous=%llu fail_send=%llu fail_read=%llu"
			" session_check_fail=%llu log_suppressed=%llu ledger_events=%llu ledger_dropped=%llu ledger_write_errors=%llu",
			U(sum.qBytes), U(sum.resNotDelivered), U(sum.resRolledBack), U(sum.resAmbiguous), U(sum.resPermanent),
			U(sum.resUnexecuted), U(sum.polTransient), U(sum.polConfig), U(sum.polResource), U(sum.polInternal),
			U(sum.polQuery), U(sum.polAmbiguous), U(sum.failSend), U(sum.failRead), U(sum.sessionCheckFail),
			U(sum.logSuppressed), U(sum.failureEvents), sql_failure_ledger::Instance().Dropped(),
			sql_failure_ledger::Instance().WriteErrors());
		len = Append(line, sizeof(line), len, " metrics_dropped=%llu metrics_write_errors=%llu%s",
			out.Dropped(), out.WriteErrors(), M2BuildFields());
		out.Write(line, len);

		m_windowStart = now;
		m_first = false;
	}

private:
	struct Conn
	{
		const char* owner = "";
		const char* target = "";
		const char* role = "";
		CAsyncSQL* sql = nullptr;
		SQLStats prev;
		uint64_t execMaxSinceLine = 0;
	};

	struct Sum
	{
		int conns = 0, stuck = 0, workersDown = 0;
		uint64_t q = 0, cq = 0, rq = 0;
		int64_t oldestMax = 0;
		uint64_t pushed = 0, ok = 0, err = 0, retry = 0, reconnect = 0;
		uint64_t errnoCount[SQL_ERRNO_BUCKET_MAX] = {};
		uint64_t execN = 0, execUs = 0, execMaxUs = 0;
		uint64_t directN = 0, directErr = 0, directMaxMs = 0;
		uint64_t qBytes = 0;
		uint64_t resNotDelivered = 0, resRolledBack = 0, resAmbiguous = 0, resPermanent = 0, resUnexecuted = 0;
		uint64_t polTransient = 0, polConfig = 0, polResource = 0, polInternal = 0, polQuery = 0, polAmbiguous = 0;
		uint64_t failSend = 0, failRead = 0, sessionCheckFail = 0, logSuppressed = 0, failureEvents = 0;
	};

	static unsigned long long U(uint64_t v) { return (unsigned long long) v; }

	static size_t Append(char* buf, size_t size, size_t len, const char* fmt, ...)
	{
		if (len >= size)
			return len;
		va_list ap;
		va_start(ap, fmt);
		const int n = std::vsnprintf(buf + len, size - len, fmt, ap);
		va_end(ap);
		if (n < 0)
			return len;
		return len + (size_t) n < size ? len + (size_t) n : size - 1;
	}

	static size_t AppendErrno(char* buf, size_t size, size_t len, const uint64_t* e, const char* suffix)
	{
		return Append(buf, size, len, " e2006%s=%llu e2013%s=%llu e2014%s=%llu e1205%s=%llu e1213%s=%llu e_other%s=%llu",
			suffix, U(e[SQL_ERRNO_2006]), suffix, U(e[SQL_ERRNO_2013]), suffix, U(e[SQL_ERRNO_2014]),
			suffix, U(e[SQL_ERRNO_1205]), suffix, U(e[SQL_ERRNO_1213]), suffix, U(e[SQL_ERRNO_OTHER]));
	}

	// Keep the line parseable as key=value: no spaces or '=' inside a value
	static void SafeValue(char* dst, size_t size, const char* src)
	{
		size_t i = 0;
		for (; src && src[i] && i + 1 < size; ++i)
		{
			const char ch = src[i];
			dst[i] = (ch == ' ' || ch == '=' || ch == '\t' || ch == '\r' || ch == '\n') ? '_' : ch;
		}
		dst[i] = '\0';
	}

	void WriteConn(metrics_writer& out, const char* host, long pid, long long uptime, const Conn& c, const SQLStats& s,
		const char* reason, bool withQuit)
	{
		char line[2048];
		size_t len = Append(line, sizeof(line), 0,
			"schema=1 src=sql kind=conn host=%s pid=%ld uptime_s=%lld owner=%s target=%s role=%s mode=%s reason=%s",
			host, pid, uptime, c.owner, c.target, c.role, s.threaded ? "thread" : "direct", reason);

		if (s.threaded)
		{
			len = Append(line, sizeof(line), len,
				" q=%llu cq=%llu rq=%llu oldest_ms=%lld stuck_ms=%lld worker=%d"
				" pushed_total=%llu ok_total=%llu err_total=%llu retry_total=%llu reconnect_seen_total=%llu",
				U(s.queued), U(s.copied), U(s.results), (long long) s.oldestAgeMs, (long long) s.stuckMs,
				s.workerRunning ? 1 : 0, U(s.pushed), U(s.ok), U(s.err), U(s.retry), U(s.reconnectSeen));
			len = AppendErrno(line, sizeof(line), len, s.errnoCount, "_total");
			len = Append(line, sizeof(line), len, " exec_n_total=%llu exec_us_total=%llu exec_max_us=%llu",
				U(s.execCount), U(s.execUsTotal), U(c.execMaxSinceLine));
			if (withQuit)
				len = Append(line, sizeof(line), len, " unexecuted_at_quit=%llu", U(s.unexecutedAtQuit));
		}
		else
		{
			len = Append(line, sizeof(line), len, " ok_total=%llu err_total=%llu reconnect_seen_total=%llu",
				U(s.ok), U(s.err), U(s.reconnectSeen));
			len = AppendErrno(line, sizeof(line), len, s.errnoCount, "_total");
			len = Append(line, sizeof(line), len, " exec_n_total=%llu exec_ms_total=%llu exec_max_ms=%llu",
				U(s.execCount), U(s.execUsTotal / 1000), U(c.execMaxSinceLine / 1000));
		}

		len = Append(line, sizeof(line), len,
			" q_bytes=%llu res_not_delivered_total=%llu res_rolled_back_total=%llu res_ambiguous_total=%llu"
			" res_permanent_total=%llu res_unexecuted_at_quit_total=%llu pol_transient_total=%llu pol_config_fatal_total=%llu"
			" pol_resource_limit_total=%llu pol_internal_total=%llu pol_query_permanent_total=%llu pol_ambiguous_total=%llu"
			" fail_send_total=%llu fail_read_total=%llu session_check_fail_total=%llu log_suppressed_total=%llu",
			U(s.queuedBytes), U(s.resNotDelivered), U(s.resRolledBack), U(s.resAmbiguous), U(s.resPermanent),
			U(s.resUnexecutedAtQuit), U(s.polTransient), U(s.polConfigFatal), U(s.polResourceLimit), U(s.polInternal),
			U(s.polQueryPermanent), U(s.polAmbiguous), U(s.phaseSendFail), U(s.phaseReadFail), U(s.sessionCheckFail),
			U(s.logSuppressed));

		out.Write(line, len);
	}

private:
	Conn m_conns[MAX_CONNS];
	int m_count = 0;
	bool m_first = true;
	Clock::time_point m_start, m_windowStart, m_lastPeriodic;
};
