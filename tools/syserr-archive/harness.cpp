// Drives the real libthecore logger (log_init / sys_err / log_destroy) in the current directory, for
// tools/syserr-archive/run.sh. Nothing here reimplements the archiving: every case runs the code the game and db use.
//
//   harness run <tag>                  one run: log_init, one syserr line "RUN <tag>", log_destroy
//   harness segv <tag>                 syserr line, 50 ms, SIGSEGV (lines already written must be on disk)
//   harness abort-now <tag>            syserr line, then abort() at once (the CHECKPOINT path, a known limit)
//   harness load <lines> <flush 0|1> <bare|sampled|per-line>
//                                      <lines> syserr lines with flush_on(err) off/on, then drain (spdlog::shutdown):
//     bare      no clock or queue reads in the loop: the producer at full speed, total time only
//     sampled   queue size every 256 lines, one clock read per 1024 lines: close to bare. The queue only grows by
//               enqueues, so its true peak <= sampled peak + 256 (+4 for flush_every messages); below capacity => it
//               never filled => the block overflow policy never made the producer wait
//     per-line  two clock reads per line (steady_clock costs ~11.6 us on the test VM: slows the producer) + percentiles
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

static void produce(long i)
{
	// ~200 B, the size of an INVALID PACKET / UNKNOWN HEADER line (game/input.cpp:92, 108)
	sys_err("UNKNOWN HEADER: 0x%04X (recv_seq #%u), LAST: 0x%04X[%u], REMAIN: %d, CHAR: %s, PHASE: %d, fd: %d host: %s line %ld",
		0x1234, 7u, 0x0102, 12u, 0, "<none>", 1, 42, "192.0.2.10", i);
}

static int load(long lines, bool flush, const std::string& mode)
{
	constexpr long QUEUE_EVERY = 256, BATCH = 1024;
	const size_t cap = 1 << 14; // LOGGER_QUEUE_SIZE in libthecore/log.cpp

	log_init();
	if (!flush)
		spdlog::get("syserr")->flush_on(spdlog::level::off);
	auto pool = spdlog::thread_pool();

	size_t queue_max = 0;
	auto sample_queue = [&] { queue_max = std::max(queue_max, pool->queue_size()); };

	std::vector<double> us; // per-line: one value per line; sampled: per-line average of each batch
	long over_1ms = 0, over_10ms = 0;

	const auto start = clk::now();
	if (mode == "bare")
	{
		for (long i = 0; i < lines; ++i)
			produce(i);
	}
	else if (mode == "sampled")
	{
		us.reserve(lines / BATCH + 1);
		auto t0 = clk::now();
		for (long i = 0; i < lines; ++i)
		{
			produce(i);
			if (i % QUEUE_EVERY == QUEUE_EVERY - 1)
				sample_queue();
			if (i % BATCH == BATCH - 1)
			{
				const auto t1 = clk::now();
				us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count() / BATCH);
				t0 = t1;
			}
		}
	}
	else if (mode == "per-line")
	{
		us.reserve(lines);
		for (long i = 0; i < lines; ++i)
		{
			const auto t0 = clk::now();
			produce(i);
			const double d = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
			us.push_back(d);
			over_1ms += d >= 1000.0;
			over_10ms += d >= 10000.0;
			if (i % QUEUE_EVERY == QUEUE_EVERY - 1)
				sample_queue();
		}
	}
	else
		return 2;
	const double produce_ms = std::chrono::duration<double, std::milli>(clk::now() - start).count();
	const size_t queue_end = pool->queue_size();

	const auto drain_start = clk::now();
	log_destroy();
	const double drain_ms = std::chrono::duration<double, std::milli>(clk::now() - drain_start).count();

	std::printf("load mode=%s flush=%d lines=%ld produce_ms=%.1f per_line_us=%.3f drain_ms=%.1f", mode.c_str(), flush ? 1 : 0,
		lines, produce_ms, produce_ms * 1000.0 / lines, drain_ms);
	if (mode != "bare")
	{
		// Besides this loop only spdlog's flush_every(1s) thread enqueues (one flush message per registered logger and
		// second, 2 loggers): a few more between two samples at most
		const size_t bound = queue_max + QUEUE_EVERY + 4;
		std::sort(us.begin(), us.end());
		auto pct = [&](double p) { return us[std::min(us.size() - 1, (size_t)(p * (us.size() - 1)))]; };
		std::printf(" queue_max_sampled=%zu queue_end=%zu queue_peak_bound=%zu queue_cap=%zu queue_ever_full=%s", queue_max,
			queue_end, bound, cap, bound < cap ? "no" : "POSSIBLE");
		if (mode == "sampled")
			std::printf(" batch_us_per_line p50=%.3f p99=%.3f max=%.3f (batches of %ld)", pct(0.50), pct(0.99), us.back(), BATCH);
		else
			std::printf(" line_us p50=%.3f p99=%.3f p999=%.3f max=%.1f over_1ms=%ld over_10ms=%ld", pct(0.50), pct(0.99), pct(0.999),
				us.back(), over_1ms, over_10ms);
	}
	std::printf("\n");
	return 0;
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: harness run|segv|abort-now <tag> | load <lines> <flush 0|1> <bare|sampled|per-line>\n");
		return 2;
	}
	const std::string mode = argv[1];

	if (mode == "load")
		return argc < 5 ? 2 : load(std::atol(argv[2]), std::atoi(argv[3]) == 1, argv[4]);

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
