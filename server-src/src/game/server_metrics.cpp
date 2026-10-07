#include "stdafx.h"
#include "server_metrics.h"

#include "config.h"
#include "desc_manager.h"
#include "char_manager.h"
#include "db.h"
#include "log.h"
#include "sql_metrics.h"
#include "common/build_identity.h"

#include <cstdio>

#ifdef OS_WINDOWS
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
	const auto METRICS_WINDOW = std::chrono::seconds(10);
	const int METRICS_KEEP_DAYS = 14;
	const size_t METRICS_QUEUE_SIZE = 64;

	long long ToMicroseconds(CServerMetrics::Clock::duration d)
	{
		return (long long) std::chrono::duration_cast<std::chrono::microseconds>(d).count();
	}

	long CurrentProcessId()
	{
#ifdef OS_WINDOWS
		return (long) _getpid();
#else
		return (long) getpid();
#endif
	}

	// Keep the line parseable as key=value: no spaces or '=' inside a value
	void CopySafeValue(char* dst, size_t dstSize, const std::string& src)
	{
		size_t i = 0;
		for (; i + 1 < dstSize && i < src.size(); ++i)
		{
			const char c = src[i];
			dst[i] = (c == ' ' || c == '=' || c == '\t' || c == '\r' || c == '\n') ? '_' : c;
		}
		dst[i] = '\0';
	}
}

CServerMetrics::CServerMetrics() : m_bEnabled(false)
{
	ResetWindow(Clock::now());
	m_startTime = m_iterationStart = m_stamp = m_windowStart;
	m_bytesSeen = 0;
}

CServerMetrics::~CServerMetrics()
{
	Shutdown();
}

void CServerMetrics::Initialize()
{
	if (!g_bMetricsEnable)
	{
		sys_log(0, "METRICS: disabled (METRICS_ENABLE: 0)");
		return;
	}

	// Own queue and worker: never shares the syslog/syserr pool or another telemetry stream
	if (!m_writer.Start("log", "metrics", METRICS_KEEP_DAYS, METRICS_QUEUE_SIZE))
	{
		sys_err("METRICS: initialization failed, metrics disabled");
		return;
	}

	const Clock::time_point now = Clock::now();
	m_startTime = m_iterationStart = m_stamp = now;
	ResetWindow(now);
	m_bytesSeen = 0;
	m_bEnabled = true;

	sys_log(0, "METRICS: enabled, every %d s to log/metrics_YYYY-MM-DD.log, kept %d days",
		(int) METRICS_WINDOW.count(), METRICS_KEEP_DAYS);

	StartSql(now);
}

void CServerMetrics::StartSql(Clock::time_point now)
{
	// Connections that were never set up on this core (e.g. LogManager on the auth core) report configured=0 and
	// get no lines. Targets follow config_init: DBManager uses account_sql on the auth core and player_sql elsewhere;
	// AccountDB uses common_sql (game/config.cpp).
	const char* dbTarget = g_bAuthServer ? "account" : "player";
	m_sql = std::make_unique<sql_metrics_reporter>();
	m_sql->Add("dbmanager", dbTarget, "main", DBManager::instance().GetSQLForStats());
	m_sql->Add("dbmanager", dbTarget, "direct", DBManager::instance().GetDirectSQLForStats());
	m_sql->Add("accountdb", "common", "main", AccountDB::instance().GetSQLForStats());
	m_sql->Add("accountdb", "common", "direct", AccountDB::instance().GetDirectSQLForStats());
	m_sql->Add("logmanager", "log", "main", LogManager::instance().GetSQLForStats());

	if (!m_sqlWriter.Start("log", "sql", METRICS_KEEP_DAYS, METRICS_QUEUE_SIZE))
	{
		sys_err("METRICS: SQL stream initialization failed, SQL lines disabled");
		return;
	}

	m_sql->Start(now);
	sys_log(0, "METRICS: SQL lines every %d s to log/sql_YYYY-MM-DD.log", (int) METRICS_WINDOW.count());

	// Failure ledger (metadata only, 0600): lives until process exit, so the drain of the SQL connections after
	// destroy() (they are locals of main) is still recorded
	char host[64];
	CopySafeValue(host, sizeof(host), g_stHostname);
	if (!sql_failure_ledger::Instance().Start(host, CurrentProcessId(), METRICS_KEEP_DAYS, METRICS_QUEUE_SIZE))
		sys_err("METRICS: SQL failure ledger initialization failed");
}

void CServerMetrics::Shutdown()
{
	if (!m_writer.IsRunning())
		return;

	const Clock::time_point now = Clock::now();
	if (m_bEnabled && now - m_windowStart >= std::chrono::seconds(1))
		Emit(now); // last partial window before exit

	if (m_bEnabled)
		EmitSql(now, true);

	m_bEnabled = false;
	m_writer.Stop();
	m_sqlWriter.Stop();
}

void CServerMetrics::ResetWindow(Clock::time_point now)
{
	m_windowStart = now;

	m_event = m_heartbeat = m_character = m_io = Clock::duration::zero();
	m_work = m_workMax = m_gapMax = Clock::duration::zero();

	m_iterations = 0;
	m_pulses = 0;
	m_latePulses = 0;
	m_lateIterations = 0;
	m_maxLatePulses = 0;
	m_events = 0;
	m_sentBytes = 0;
}

void CServerMetrics::BeginIteration(int iPassedPulses)
{
	const Clock::time_point now = Clock::now();

	// Real time since the previous iteration started (normally one tick, ~16.7 ms). Unlike late_pulses it also
	// catches a stall inside heart_idle's sleep (heart_idle counts missed pulses before sleeping, so that stall
	// is only reported one iteration later), and it is closed into the window the stall ended in.
	const Clock::duration gap = now - m_iterationStart;
	if (gap > m_gapMax)
		m_gapMax = gap;

	// The pulses heart_idle just returned cover the time since its previous return: the previous iteration's work
	// and sleep. They belong to the window that iteration ran in, so they are added before the window is closed;
	// a long iteration and the late pulses it causes therefore land in the same line.
	if (iPassedPulses > 0)
	{
		m_pulses += iPassedPulses;

		// heart_idle returns more than one pulse when the previous iteration overran its tick
		const uint64_t late = (uint64_t) (iPassedPulses - 1);
		if (late > 0)
		{
			m_latePulses += late;
			++m_lateIterations;
			if (late > m_maxLatePulses)
				m_maxLatePulses = late;
		}
	}

	m_iterationStart = m_stamp = now;

	if (now - m_windowStart >= METRICS_WINDOW)
	{
		Emit(now);
		EmitSql(now, false);

		// Building and queueing the line is work of this iteration, but no game section's: restart the section
		// clock so it shows in other_us instead of the next event_us (one extra clock read per window)
		m_stamp = Clock::now();
	}
}

void CServerMetrics::Mark()
{
	m_stamp = Clock::now();
}

void CServerMetrics::MarkEvents(int iEventCount)
{
	const Clock::time_point now = Clock::now();
	m_event += now - m_stamp;
	m_stamp = now;

	if (iEventCount > 0)
		m_events += iEventCount;
}

void CServerMetrics::MarkHeartbeat()
{
	const Clock::time_point now = Clock::now();
	m_heartbeat += now - m_stamp;
	m_stamp = now;
}

void CServerMetrics::MarkCharacterUpdate()
{
	const Clock::time_point now = Clock::now();
	m_character += now - m_stamp;
	m_stamp = now;
}

void CServerMetrics::MarkIO()
{
	const Clock::time_point now = Clock::now();
	m_io += now - m_stamp;
	m_stamp = now;
}

void CServerMetrics::EndIteration(int iCurrentBytesWritten)
{
	// The last section mark (after io_loop) is the end of the iteration's work; no extra clock read
	const Clock::duration work = m_stamp - m_iterationStart;

	++m_iterations;
	m_work += work;
	if (work > m_workMax)
		m_workMax = work;

	AddBytesWritten(iCurrentBytesWritten);
}

void CServerMetrics::AddBytesWritten(int iCurrentBytesWritten)
{
	const int64_t current = iCurrentBytesWritten;
	m_sentBytes += (current >= m_bytesSeen) ? (uint64_t) (current - m_bytesSeen) : (uint64_t) (current > 0 ? current : 0);
	m_bytesSeen = current;
}

void CServerMetrics::OnBytesWrittenReset(int iCurrentBytesWritten)
{
	AddBytesWritten(iCurrentBytesWritten);
	m_bytesSeen = 0;
}

void CServerMetrics::Emit(Clock::time_point now)
{
	const Clock::duration window = now - m_windowStart;
	const long long windowUs = ToMicroseconds(window);

	const Clock::duration sections = m_event + m_heartbeat + m_character + m_io;
	const Clock::duration other = m_work > sections ? m_work - sections : Clock::duration::zero();
	const double busyPct = windowUs > 0 ? 100.0 * (double) ToMicroseconds(m_work) / (double) windowUs : 0.0;

	char host[64];
	CopySafeValue(host, sizeof(host), g_stHostname);

	char line[1024];
	const int len = snprintf(line, sizeof(line),
		"schema=1 host=%s ch=%u port=%u pid=%ld uptime_s=%lld window_ms=%lld"
		" users_local=%u descs_total=%zu chars_total=%zu pcs=%zu fsm_chars=%zu"
		" iters=%llu pulses=%llu late_pulses=%llu late_iters=%llu max_late_pulses=%llu"
		" work_us=%lld work_max_us=%lld iter_gap_max_us=%lld busy_pct=%.2f"
		" event_us=%lld hb_us=%lld chr_us=%lld io_us=%lld other_us=%lld"
		" events=%llu sent_bytes=%llu metrics_dropped=%llu metrics_write_errors=%llu%s",
		host, (unsigned) g_bChannel, (unsigned) mother_port, CurrentProcessId(),
		(long long) std::chrono::duration_cast<std::chrono::seconds>(now - m_startTime).count(), windowUs / 1000,
		(unsigned) DESC_MANAGER::instance().GetLocalUserCount(), DESC_MANAGER::instance().GetClientSet().size(),
		CHARACTER_MANAGER::instance().GetCharacterCount(), CHARACTER_MANAGER::instance().GetPCCount(),
		CHARACTER_MANAGER::instance().GetStateCharacterCount(),
		(unsigned long long) m_iterations, (unsigned long long) m_pulses, (unsigned long long) m_latePulses,
		(unsigned long long) m_lateIterations, (unsigned long long) m_maxLatePulses,
		ToMicroseconds(m_work), ToMicroseconds(m_workMax), ToMicroseconds(m_gapMax), busyPct,
		ToMicroseconds(m_event), ToMicroseconds(m_heartbeat), ToMicroseconds(m_character),
		ToMicroseconds(m_io), ToMicroseconds(other),
		(unsigned long long) m_events, (unsigned long long) m_sentBytes,
		m_writer.Dropped(), m_writer.WriteErrors(), M2BuildFields());

	ResetWindow(now);

	if (len <= 0)
		return;

	const size_t length = (size_t) len < sizeof(line) ? (size_t) len : sizeof(line) - 1;
	m_writer.Write(line, length);
}

void CServerMetrics::EmitSql(Clock::time_point now, bool bFinal)
{
	if (!m_sqlWriter.IsRunning() || !m_sql)
		return;

	char host[64];
	CopySafeValue(host, sizeof(host), g_stHostname);

	// The game's SQL connections are closed after the log is shut down (game/main.cpp), so what they leave
	// unexecuted cannot be counted here: game lines never carry unexecuted_at_quit (the failure ledger records it)
	m_sql->Window(now, host, CurrentProcessId(), m_sqlWriter, "", bFinal, false);
}
