// Producer-side cost of CAsyncSQL::AsyncQuery (what the game loop pays per queued query), built twice from this one
// file: against the libsql before DB step 1c and against the instrumented one, then compared (DB step 1c, acceptance).
// No database: the worker's connect fails at once (nothing listens on 127.0.0.1:1), so the worker is gone and the
// numbers are the caller's own work only (message allocation, query copy, lock, push, notify + telemetry in 1c).
//
//   c++ -std=c++20 -O3 -DOS_FREEBSD [-DHAVE_SQLSTATS] -I<server-src>/src -I<server-src>/include \
//       -I<build>/vendor/mariadb-connector-c-3.4.5/include -I<server-src>/vendor/mariadb-connector-c-3.4.5/include \
//       -o enqueue-bench enqueue-bench.cpp <build>/lib/liblibsql.a <build>/lib/liblibthecore.a \
//       <build>/lib/libmariadbclient.a <build>/lib/libspdlog.a -lssl -lcrypto -lmd -lpthread -lm
//   ./enqueue-bench [N]        (run on an otherwise idle host; prints the median of 9 rounds of N queries)
#include "libsql/AsyncSQL.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace
{
	// A typical short game query (~110 bytes), e.g. a log INSERT
	const char* QUERY = "INSERT INTO log (type, time, who, x, y, what, how, hint, ip, vnum) "
		"VALUES('CHARACTER', NOW(), 1234, 100, 200, 0, 'LOGIN', '', '0.0.0.0', 0)";

	double RoundNs(int n)
	{
		CAsyncSQL sql;
		sql.Setup("127.0.0.1", "nobody", "x", "none", "", false, 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(300)); // worker's connect fails and the thread ends
		for (int i = 0; i < 1000; ++i)
			sql.AsyncQuery(QUERY);

		const auto t0 = Clock::now();
		for (int i = 0; i < n; ++i)
			sql.AsyncQuery(QUERY);
		const auto t1 = Clock::now();
		return std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
	}

	double Median(std::vector<double> v)
	{
		std::sort(v.begin(), v.end());
		return v[v.size() / 2];
	}
}

int main(int argc, char** argv)
{
	log_init();

	// Optional argv[1]: queries per round (default 100000; the 2a A/B uses 200000)
	const int N = argc > 1 ? std::atoi(argv[1]) : 100000;
	if (N <= 0)
		return 2;
	std::vector<double> rounds;
	for (int r = 0; r < 9; ++r)
		rounds.push_back(RoundNs(N));
	std::printf("AsyncQuery producer cost: median %.1f ns/query (min %.1f, max %.1f) over 9 rounds x %d\n",
		Median(rounds), *std::min_element(rounds.begin(), rounds.end()), *std::max_element(rounds.begin(), rounds.end()), N);

#ifdef HAVE_SQLSTATS
	{
		const int M = 1000000;
		volatile int64_t sink = 0;
		const auto t0 = Clock::now();
		for (int i = 0; i < M; ++i)
			sink = sink + SQLStatsClock();
		const auto t1 = Clock::now();
		std::printf("SQLStatsClock: %.1f ns/read\n", std::chrono::duration<double, std::nano>(t1 - t0).count() / M);
	}
	{
		CAsyncSQL sql;
		SQLStats s;
		const int M = 100000;
		const auto t0 = Clock::now();
		for (int i = 0; i < M; ++i)
			sql.CollectStats(s);
		const auto t1 = Clock::now();
		std::printf("CollectStats: %.1f ns/connection (once per connection per 10 s window)\n",
			std::chrono::duration<double, std::nano>(t1 - t0).count() / M);
	}
#endif
	return 0;
}
