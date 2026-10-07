#pragma once

#include "metrics_writer.h"
#include "build_identity.h"
#include "libsql/AsyncSQL.h"

#include <atomic>
#include <cstdio>
#include <mutex>

// SQL failure ledger (docs/engineering/db-step2-asyncsql-fix.md, section 7): one line per statement that finished
// without being applied, in log/sql_failures_YYYY-MM-DD.log, file mode 0600. METADATA ONLY: connection label, family
// (verb.table, never a value), correlation id, errno, phase, result, policy, attempts, age, build. No SQL text, no
// hash of it, no parameters, no account/player/item id, no IP, no name.
//
// A function-local static that lives until process exit: in game the SQL objects are locals of main() and drain
// after destroy() has stopped the metrics streams, so the ledger must outlive them. Its own queue and worker
// (metrics_writer) do not depend on the global spdlog pool that log_destroy() shuts down. Write() never blocks.
class sql_failure_ledger
{
public:
	static sql_failure_ledger& Instance()
	{
		static sql_failure_ledger s_ledger;
		return s_ledger;
	}

	// Starts the stream and installs the hook. host: the process label in every line (e.g. "db", "channel1_1").
	bool Start(const char* host, long pid, int keepDays, size_t queueSize)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		if (m_writer.IsRunning())
			return true;
		if (!m_writer.Start("log", "sql_failures", keepDays, queueSize, nullptr, 0600))
			return false;
		SafeValue(m_host, sizeof(m_host), host);
		m_pid = pid;
		CAsyncSQL::SetFailureHook(&sql_failure_ledger::Hook);
		return true;
	}

	unsigned long long Dropped() const { return m_writer.Dropped(); }
	unsigned long long WriteErrors() const { return m_writer.WriteErrors(); }

private:
	sql_failure_ledger() = default;

	~sql_failure_ledger()
	{
		CAsyncSQL::SetFailureHook(nullptr);
		std::lock_guard<std::mutex> lock(m_mtx);
		m_writer.Stop();
	}

	static void Hook(const SQLFailureEvent& e)
	{
		Instance().Write(e);
	}

	void Write(const SQLFailureEvent& e)
	{
		char label[96];
		SafeValue(label, sizeof(label), e.label && *e.label ? e.label : "-");

		char line[512];
		const int n = std::snprintf(line, sizeof(line),
			"schema=1 src=sql_failure host=%s pid=%ld role=%s family=%s id=%d phase=%s errno=%u result=%s policy=%s"
			" attempts=%u age_ms=%lld%s",
			m_host, m_pid, label, e.family, e.iID, SQLPhaseName(e.ePhase), e.uiErrno, SQLResultName(e.eResult),
			SQLPolicyName(e.ePolicy), e.uiAttempts, (long long) e.llAgeMs, M2BuildFields());
		if (n <= 0)
			return;

		// Serialized with Stop() so a hook call on a worker thread never races the stream going away
		std::lock_guard<std::mutex> lock(m_mtx);
		m_writer.Write(line, (size_t) n < sizeof(line) ? (size_t) n : sizeof(line) - 1);
	}

	// Keep the line parseable as key=value
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

	std::mutex m_mtx;
	metrics_writer m_writer;
	char m_host[64] = "-";
	long m_pid = 0;
};
