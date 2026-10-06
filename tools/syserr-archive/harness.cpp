// Drives the real libthecore logger (log_init / sys_err / log_destroy) in the current directory, for
// tools/syserr-archive/run.sh. Nothing here reimplements the archiving: every case runs the code the game and db use.
//
//   harness run <tag>                  one run: log_init, one syserr line "RUN <tag>", log_destroy
//   harness segv <tag>                 syserr line, 50 ms, SIGSEGV (lines already written must be on disk)
//   harness abort-now <tag>            syserr line, then abort() at once (the CHECKPOINT path, a known limit)
//   harness load <lines> <flush 0|1> [total]
//                                      producer timing for <lines> syserr lines (flush_on(err) on/off), then drain;
//                                      "total": no per-line clock reads (steady_clock costs ~11.6 us on the test VM)
#include "libthecore/log.h"

#include <spdlog/spdlog.h>
#include <spdlog/async.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;

static int load(long lines, bool flush, bool per_line)
{
	log_init();
	if (!flush)
		spdlog::get("syserr")->flush_on(spdlog::level::off);

	auto pool = spdlog::thread_pool();
	std::vector<double> us;
	us.reserve(lines);
	size_t queue_max = 0;
	long over_1ms = 0, over_10ms = 0;

	// ~200 B, the size of an INVALID PACKET / UNKNOWN HEADER line (game/input.cpp:92, 108)
	const auto start = clk::now();
	for (long i = 0; i < lines && !per_line; ++i)
		sys_err("UNKNOWN HEADER: 0x%04X (recv_seq #%u), LAST: 0x%04X[%u], REMAIN: %d, CHAR: %s, PHASE: %d, fd: %d host: %s line %ld",
			0x1234, 7u, 0x0102, 12u, 0, "<none>", 1, 42, "192.0.2.10", i);
	for (long i = 0; i < lines && per_line; ++i)
	{
		const auto t0 = clk::now();
		sys_err("UNKNOWN HEADER: 0x%04X (recv_seq #%u), LAST: 0x%04X[%u], REMAIN: %d, CHAR: %s, PHASE: %d, fd: %d host: %s line %ld",
			0x1234, 7u, 0x0102, 12u, 0, "<none>", 1, 42, "192.0.2.10", i);
		const double d = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
		us.push_back(d);
		over_1ms += d >= 1000.0;
		over_10ms += d >= 10000.0;
		if ((i & 255) == 0)
			queue_max = std::max(queue_max, pool->queue_size());
	}
	const double produce_ms = std::chrono::duration<double, std::milli>(clk::now() - start).count();

	const auto drain_start = clk::now();
	log_destroy(); // spdlog::shutdown: waits until the queue is written
	const double drain_ms = std::chrono::duration<double, std::milli>(clk::now() - drain_start).count();

	if (!per_line)
	{
		std::printf("load lines=%ld flush=%d mode=total produce_ms=%.1f per_line_us=%.3f drain_ms=%.1f\n",
			lines, flush ? 1 : 0, produce_ms, produce_ms * 1000.0 / lines, drain_ms);
		return 0;
	}

	std::sort(us.begin(), us.end());
	auto pct = [&](double p) { return us[std::min(us.size() - 1, (size_t)(p * (us.size() - 1)))]; };
	double sum = 0;
	for (double d : us)
		sum += d;
	std::printf("load lines=%ld flush=%d produce_ms=%.1f drain_ms=%.1f per_line_us avg=%.3f p50=%.3f p99=%.3f p999=%.3f max=%.1f "
		"over_1ms=%ld over_10ms=%ld queue_max=%zu queue_cap=%d\n",
		lines, flush ? 1 : 0, produce_ms, drain_ms, sum / us.size(), pct(0.50), pct(0.99), pct(0.999), us.back(),
		over_1ms, over_10ms, queue_max, 1 << 14);
	return 0;
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: harness run|segv|abort-now <tag> | load <lines> <flush 0|1> [total]\n");
		return 2;
	}
	const std::string mode = argv[1];

	if (mode == "load")
		return argc < 4 ? 2 : load(std::atol(argv[2]), std::atoi(argv[3]) == 1, !(argc > 4 && std::strcmp(argv[4], "total") == 0));

	log_init();
	sys_err("RUN %s", argv[2]);

	if (mode == "segv")
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		std::raise(SIGSEGV);
	}
	if (mode == "abort-now")
		std::abort();

	log_destroy();
	return 0;
}
