// Unit tests for dbstat_core.h: the rules that decide when a delta is written and when it is left out.
//   c++ -std=c++20 -Wall -Wextra -o /tmp/dbstat_test dbstat_test.cpp && /tmp/dbstat_test
#include "dbstat_core.h"

#include <cstdio>
#include <cstring>

using namespace dbstat;

static int g_failures = 0;

static void Expect(bool ok, const char* what)
{
	printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		++g_failures;
}

static size_t Index(const char* out)
{
	for (size_t i = 0; i < kDbFieldCount; ++i)
		if (!strcmp(kDbFields[i].out, out))
			return i;
	return kDbFieldCount;
}

// A sample where every supported field has value base + i, Uptime = uptime
static DbSample Full(uint64_t uptime, uint64_t base)
{
	DbSample s;
	char v[32];
	snprintf(v, sizeof(v), "%llu", static_cast<unsigned long long>(uptime));
	s.Set("Uptime", v);
	for (size_t i = 0; i < kDbFieldCount; ++i)
	{
		snprintf(v, sizeof(v), "%llu", static_cast<unsigned long long>(base + i));
		s.Set(kDbFields[i].var, v);
	}
	return s;
}

int main()
{
	const size_t q = Index("questions"), tr = Index("threads_running"), rlw = Index("row_lock_waits");

	printf("1. first sample: gauges only, no deltas\n");
	{
		const DbSample a = Full(100, 1000);
		const DbWindow w = ComputeDb(nullptr, a);
		Expect(w.first && !w.restart && !w.haveWindow, "first=1, no window");
		Expect(w.state[q] == ValueState::NoWindow, "delta field is '-'");
		Expect(w.state[tr] == ValueState::Value && w.value[tr] == a.v[tr], "gauge carries the current value");
	}

	printf("2. normal window: delta = current - previous, window = Uptime difference\n");
	{
		const DbSample a = Full(100, 1000), b = Full(110, 1050);
		const DbWindow w = ComputeDb(&a, b);
		Expect(!w.first && !w.restart && w.haveWindow && w.window_s == 10, "window_s=10");
		Expect(w.state[q] == ValueState::Value && w.value[q] == 50, "questions delta 50");
		Expect(w.na == 0 && w.reset == 0, "na=0 reset=0");
	}

	printf("3. MariaDB restart (Uptime went back): no deltas, no spike\n");
	{
		const DbSample a = Full(5000, 900000), b = Full(12, 30);
		const DbWindow w = ComputeDb(&a, b);
		Expect(w.restart && !w.haveWindow, "restart=1, no window");
		bool anyDelta = false;
		for (size_t i = 0; i < kDbFieldCount; ++i)
			anyDelta |= kDbFields[i].kind == Kind::Delta && w.state[i] == ValueState::Value;
		Expect(!anyDelta, "every delta field is '-'");
	}

	printf("4. one counter went back without a restart (FLUSH STATUS / wrap): only that field is left out\n");
	{
		DbSample a = Full(100, 1000), b = Full(110, 1050);
		b.v[rlw] = a.v[rlw] - 1;
		const DbWindow w = ComputeDb(&a, b);
		Expect(w.state[rlw] == ValueState::NoWindow && w.reset == 1, "row_lock_waits '-', reset=1");
		Expect(w.state[q] == ValueState::Value && w.value[q] == 50, "other deltas still written");
	}

	printf("5. a status variable this server does not have: NA, never a crash\n");
	{
		DbSample a = Full(100, 1000), b = Full(110, 1050);
		b.have[rlw] = false;
		const DbWindow w = ComputeDb(&a, b);
		Expect(w.state[rlw] == ValueState::Unsupported && w.na == 1, "row_lock_waits NA, na=1");
		char buf[4096];
		const size_t len = FormatDbFields(w, buf, sizeof(buf), 0);
		buf[len] = '\0';
		Expect(strstr(buf, " row_lock_waits=NA") != nullptr, "formatted as NA");
	}

	printf("6. a field that appears only now (previous sample lacked it): '-' this window\n");
	{
		DbSample a = Full(100, 1000), b = Full(110, 1050);
		a.have[q] = false;
		const DbWindow w = ComputeDb(&a, b);
		Expect(w.state[q] == ValueState::NoWindow, "questions '-'");
	}

	printf("7. no Uptime in the answer: no window, deltas '-'\n");
	{
		DbSample a = Full(100, 1000), b = Full(110, 1050);
		b.haveUptime = false;
		const DbWindow w = ComputeDb(&a, b);
		Expect(!w.haveWindow && w.state[q] == ValueState::NoWindow, "no window");
	}

	printf("8. non-numeric value is rejected (field stays unsupported)\n");
	{
		DbSample s;
		Expect(!s.Set("Questions", "12a") && !s.have[q], "rejected");
		Expect(s.Set("Some_future_counter", "5"), "unknown names ignored");
	}

	printf("9. process CPU: same pid -> delta; new pid -> first; pid reused with smaller run time -> restart\n");
	{
		ProcPrev p{ 100, 5'000'000 };
		uint64_t us = 0;
		Expect(ProcDelta(&p, 100, 5'250'000, &us) == ProcState::Value && us == 250'000, "250000 us");
		Expect(ProcDelta(&p, 100, 5'000'880, &us) == ProcState::Value && us == 880, "sub-millisecond delta kept (880 us)");
		Expect(ProcDelta(nullptr, 101, 10, &us) == ProcState::First, "new pid first");
		Expect(ProcDelta(&p, 100, 10, &us) == ProcState::Restart, "smaller run time restart");
	}

	printf("10. disk name from the device holding the datadir\n");
	{
		char d[32];
		Expect(DiskFromDevice("/dev/ada0p2", d, sizeof(d)) && !strcmp(d, "ada0"), "/dev/ada0p2 -> ada0");
		Expect(DiskFromDevice("/dev/nda1p3", d, sizeof(d)) && !strcmp(d, "nda1"), "/dev/nda1p3 -> nda1");
		Expect(DiskFromDevice("/dev/ada0s1a", d, sizeof(d)) && !strcmp(d, "ada0"), "/dev/ada0s1a -> ada0");
		Expect(!DiskFromDevice("zroot/ROOT/default", d, sizeof(d)), "ZFS dataset: not a disk (use --disk)");
	}

	printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
