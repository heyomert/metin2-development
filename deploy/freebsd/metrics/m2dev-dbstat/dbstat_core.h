// Pure logic of m2dev-dbstat (no I/O): which MariaDB status variables are read, and how a sample becomes a window
// line. Kept separate so the delta/restart/reset rules are unit-tested (dbstat_test.cpp). Rules: docs/monitoring.md
// -> "MariaDB / OS (dbstat)".
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace dbstat
{
	enum class Kind { Delta, Gauge };

	struct Field
	{
		const char* out;  // name in the output line (stable contract)
		const char* var;  // SHOW GLOBAL STATUS name
		Kind kind;
	};

	// Minimal set; each field's reason is in docs/engineering/db-step1b-collector.md. Order = output order.
	inline constexpr Field kDbFields[] = {
		{ "questions", "Questions", Kind::Delta },
		{ "com_select", "Com_select", Kind::Delta },
		{ "com_insert", "Com_insert", Kind::Delta },
		{ "com_update", "Com_update", Kind::Delta },
		{ "com_replace", "Com_replace", Kind::Delta },
		{ "com_delete", "Com_delete", Kind::Delta },
		{ "row_lock_waits", "Innodb_row_lock_waits", Kind::Delta },
		{ "row_lock_time_ms", "Innodb_row_lock_time", Kind::Delta },
		{ "row_lock_current_waits", "Innodb_row_lock_current_waits", Kind::Gauge },
		{ "deadlocks", "Innodb_deadlocks", Kind::Delta },
		{ "table_locks_waited", "Table_locks_waited", Kind::Delta },
		{ "table_locks_immediate", "Table_locks_immediate", Kind::Delta },
		{ "innodb_fsyncs", "Innodb_data_fsyncs", Kind::Delta },
		{ "innodb_log_bytes", "Innodb_os_log_written", Kind::Delta },
		{ "aria_log_syncs", "Aria_transaction_log_syncs", Kind::Delta },
		{ "bp_read_requests", "Innodb_buffer_pool_read_requests", Kind::Delta },
		{ "bp_reads", "Innodb_buffer_pool_reads", Kind::Delta },
		{ "aria_cache_read_requests", "Aria_pagecache_read_requests", Kind::Delta },
		{ "aria_cache_reads", "Aria_pagecache_reads", Kind::Delta },
		{ "history_list_length", "Innodb_history_list_length", Kind::Gauge },
		{ "bp_dirty_pages", "Innodb_buffer_pool_pages_dirty", Kind::Gauge },
		{ "threads_running", "Threads_running", Kind::Gauge },
		{ "threads_connected", "Threads_connected", Kind::Gauge },
		{ "aborted_clients", "Aborted_clients", Kind::Delta },
		{ "aborted_connects", "Aborted_connects", Kind::Delta },
		{ "conn_errors_max", "Connection_errors_max_connections", Kind::Delta },
		{ "slow_queries", "Slow_queries", Kind::Delta },
	};
	inline constexpr size_t kDbFieldCount = sizeof(kDbFields) / sizeof(kDbFields[0]);

	// One read of SHOW GLOBAL STATUS. A name the server does not return stays !have (unsupported -> "NA").
	struct DbSample
	{
		bool haveUptime = false;
		uint64_t uptime = 0;
		bool have[kDbFieldCount] = {};
		uint64_t v[kDbFieldCount] = {};

		// Feed one (name, value) row; unknown names are ignored. Returns false if the value is not an unsigned integer.
		bool Set(const char* name, const char* value)
		{
			uint64_t n = 0;
			if (!value || !*value)
				return false;
			for (const char* p = value; *p; ++p)
			{
				if (*p < '0' || *p > '9')
					return false;
				n = n * 10 + static_cast<uint64_t>(*p - '0');
			}
			if (!strcasecmp(name, "Uptime"))
			{
				haveUptime = true;
				uptime = n;
				return true;
			}
			for (size_t i = 0; i < kDbFieldCount; ++i)
			{
				if (!strcasecmp(name, kDbFields[i].var))
				{
					have[i] = true;
					v[i] = n;
					return true;
				}
			}
			return true;
		}
	};

	enum class ValueState { Value, Unsupported, NoWindow };

	struct DbWindow
	{
		bool first = false;    // no previous sample (collector start or reconnect without one)
		bool restart = false;  // Uptime went backwards: MariaDB restarted, no deltas
		int reset = 0;         // counters that went backwards without a restart (no delta for those)
		int na = 0;            // fields this server does not return
		bool haveWindow = false;
		uint64_t window_s = 0;
		ValueState state[kDbFieldCount] = {};
		uint64_t value[kDbFieldCount] = {};
	};

	// Deltas only between two samples of the same server run; a value that could be wrong is left out, never guessed.
	inline DbWindow ComputeDb(const DbSample* prev, const DbSample& cur)
	{
		DbWindow w;
		w.first = prev == nullptr;
		w.restart = prev && prev->haveUptime && cur.haveUptime && cur.uptime < prev->uptime;
		const bool deltasValid = prev && !w.restart && prev->haveUptime && cur.haveUptime;
		if (deltasValid)
		{
			w.haveWindow = true;
			w.window_s = cur.uptime - prev->uptime;
		}
		for (size_t i = 0; i < kDbFieldCount; ++i)
		{
			if (!cur.have[i])
			{
				w.state[i] = ValueState::Unsupported;
				++w.na;
				continue;
			}
			if (kDbFields[i].kind == Kind::Gauge)
			{
				w.state[i] = ValueState::Value;
				w.value[i] = cur.v[i];
				continue;
			}
			if (!deltasValid || !prev->have[i])
			{
				w.state[i] = ValueState::NoWindow;
				continue;
			}
			if (cur.v[i] < prev->v[i]) // FLUSH STATUS or wrap: the true delta is unknown
			{
				w.state[i] = ValueState::NoWindow;
				++w.reset;
				continue;
			}
			w.state[i] = ValueState::Value;
			w.value[i] = cur.v[i] - prev->v[i];
		}
		return w;
	}

	// Appends " name=value" / " name=NA" / " name=-" for every field; returns the new length (truncates safely).
	inline size_t FormatDbFields(const DbWindow& w, char* buf, size_t size, size_t len)
	{
		for (size_t i = 0; i < kDbFieldCount && len < size; ++i)
		{
			int n;
			if (w.state[i] == ValueState::Value)
				n = snprintf(buf + len, size - len, " %s=%llu", kDbFields[i].out, static_cast<unsigned long long>(w.value[i]));
			else
				n = snprintf(buf + len, size - len, " %s=%s", kDbFields[i].out, w.state[i] == ValueState::Unsupported ? "NA" : "-");
			if (n < 0)
				break;
			len += static_cast<size_t>(n) < size - len ? static_cast<size_t>(n) : size - len - 1;
		}
		return len;
	}

	// Per-process CPU: delta of the kernel's accumulated run time (kinfo_proc.ki_runtime, us), only for the same pid.
	// Kept in us: truncating each window to ms loses up to 1 ms per window and hides small processes.
	// A pid seen for the first time (collector start, process start or restart) is First; the same pid with a smaller
	// run time (pid reused) is Restart. Neither gets a delta.
	struct ProcPrev
	{
		int pid = 0;
		uint64_t runtime_us = 0;
	};

	enum class ProcState { First, Restart, Value };

	inline ProcState ProcDelta(const ProcPrev* prev, int pid, uint64_t runtime_us, uint64_t* cpu_us)
	{
		if (!prev)
			return ProcState::First;
		if (prev->pid != pid || runtime_us < prev->runtime_us)
			return ProcState::Restart;
		*cpu_us = runtime_us - prev->runtime_us;
		return ProcState::Value;
	}

	// "ada0p2" / "ada0s1a" / "nda0p2" / "vtbd0p2" -> "ada0" (leading letters + unit digits); false if not a disk name
	inline bool DiskFromDevice(const char* dev, char* out, size_t size)
	{
		if (!strncmp(dev, "/dev/", 5))
			dev += 5;
		size_t i = 0;
		while (dev[i] >= 'a' && dev[i] <= 'z')
			++i;
		if (i == 0 || dev[i] < '0' || dev[i] > '9')
			return false;
		while (dev[i] >= '0' && dev[i] <= '9')
			++i;
		if (i + 1 > size)
			return false;
		memcpy(out, dev, i);
		out[i] = '\0';
		return true;
	}
}
