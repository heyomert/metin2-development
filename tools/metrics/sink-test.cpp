// Failure-isolation test for the game's metrics writer (server-src/src/game/metrics_daily_sink.h + a private
// spdlog thread pool with discard_new, as in server_metrics.cpp). Runs on the server host, no game process needed.
//
// Build on the VM (paths from the documented build, docs/build-and-run.md):
//   c++ -std=c++20 -O2 -DSPDLOG_COMPILED_LIB -I<server-src>/vendor/spdlog-1.15.3/include -I<server-src>/src/game \
//       -o /tmp/sink-test sink-test.cpp <build>/lib/libspdlog.a -lpthread
// Run: /tmp/sink-test [dir-on-a-small-filesystem]   (scenario 8 fills it up; as root, e.g.
//   mkdir -p /tmp/small && mount -t tmpfs -o size=1m tmpfs /tmp/small && /tmp/sink-test /tmp/small; umount /tmp/small)
#include "metrics_daily_sink.h"

#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static int g_failures = 0;
static void Expect(bool ok, const char* what)
{
	std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		++g_failures;
}

static std::string Today()
{
	std::time_t t = std::time(nullptr);
	std::tm tm = *std::localtime(&t);
	char buf[16];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
	return buf;
}

static std::string DaysAgo(int days)
{
	std::time_t t = std::time(nullptr) - (std::time_t) days * 86400;
	std::tm tm = *std::localtime(&t);
	char buf[16];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
	return buf;
}

static size_t CountLines(const fs::path& p)
{
	std::ifstream in(p);
	size_t n = 0;
	std::string line;
	while (std::getline(in, line))
		++n;
	return n;
}

static void BlockAllSignals()
{
	sigset_t all;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, nullptr);
}

struct Writer
{
	std::shared_ptr<metrics_daily_sink> sink;
	std::shared_ptr<spdlog::details::thread_pool> pool;
	std::shared_ptr<spdlog::async_logger> logger;

	Writer(const std::string& dir, spdlog::sink_ptr extra = nullptr)
	{
		sink = std::make_shared<metrics_daily_sink>(dir, "metrics", 14);
		sink->set_pattern("%Y-%m-%dT%H:%M:%S%z %v");
		pool = std::make_shared<spdlog::details::thread_pool>(64, 1, [] { BlockAllSignals(); });
		if (extra)
			logger = std::make_shared<spdlog::async_logger>("metrics", spdlog::sinks_init_list{ extra, sink }, pool, spdlog::async_overflow_policy::discard_new);
		else
			logger = std::make_shared<spdlog::async_logger>("metrics", sink, pool, spdlog::async_overflow_policy::discard_new);
		logger->set_error_handler([](const std::string&) {});
	}

	~Writer()
	{
		logger->flush();
		logger.reset();
		pool.reset();
	}
};

// A sink that takes 50 ms per line, standing in for a stalled disk
class slow_sink final : public spdlog::sinks::base_sink<std::mutex>
{
protected:
	void sink_it_(const spdlog::details::log_msg&) override { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
	void flush_() override {}
};

static std::atomic<int> g_signalsOnWorker{0};
static std::atomic<int> g_signalsTotal{0};
static std::atomic<pthread_t> g_workerThread{};
static void OnSignal(int)
{
	++g_signalsTotal;
	if (pthread_equal(pthread_self(), g_workerThread.load()))
		++g_signalsOnWorker;
}

int main(int argc, char** argv)
{
	const fs::path root = fs::temp_directory_path() / ("metrics-sink-test-" + std::to_string(getpid()));
	fs::create_directories(root);

	std::printf("1. normal write\n");
	{
		const fs::path dir = root / "normal";
		{
			Writer w(dir.string());
			for (int i = 0; i < 20; ++i)
			{
				w.logger->info("schema=1 line={}", i);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		}
		const fs::path file = dir / ("metrics_" + Today() + ".log");
		Expect(fs::exists(file), "log/metrics_<today>.log created");
		Expect(CountLines(file) == 20, "20 lines written");
	}

	std::printf("2. queue saturation with a stalled sink (game thread must never wait)\n");
	{
		auto slow = std::make_shared<slow_sink>();
		Writer w((root / "slow").string(), slow);
		long long maxUs = 0, totalUs = 0;
		const int N = 10000;
		const auto loopStart = Clock::now();
		for (int i = 0; i < N; ++i)
		{
			const auto t0 = Clock::now();
			w.logger->info("schema=1 line={}", i);
			const long long us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
			totalUs += us;
			if (us > maxUs)
				maxUs = us;
		}
		const long long loopMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - loopStart).count();
		const size_t dropped = w.pool->discard_counter();
		const size_t accepted = (size_t) N - dropped;
		// accepted = queue capacity + lines the 50 ms sink consumed while the loop ran (+1 in flight)
		const size_t acceptedMax = 64 + (size_t) (loopMs / 50) + 2;
		std::printf("     loop %lld ms; enqueue avg %.2f us, max %lld us (each sample includes two clock reads); accepted %zu (max allowed %zu), dropped %zu of %d\n",
			loopMs, (double) totalUs / N, maxUs, accepted, acceptedMax, dropped, N);
		Expect(maxUs < 50000, "no enqueue waited for the 50 ms sink");
		Expect(accepted <= acceptedMax, "lines beyond the queue and the sink's progress were dropped, not queued");
		w.pool->reset_discard_counter();
	}

	std::printf("3. 'log' is a regular file (directory cannot be created)\n");
	{
		const fs::path dir = root / "notadir";
		std::ofstream(dir.string()) << "x";
		Writer w(dir.string());
		for (int i = 0; i < 5; ++i)
			w.logger->info("schema=1 line={}", i);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		Expect(w.sink->GetWriteErrors() == 5, "5 write errors counted, process still running");
	}

	std::printf("4. today's file path is a directory\n");
	{
		const fs::path dir = root / "fileisdir";
		fs::create_directories(dir / ("metrics_" + Today() + ".log"));
		Writer w(dir.string());
		for (int i = 0; i < 3; ++i)
			w.logger->info("schema=1 line={}", i);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		Expect(w.sink->GetWriteErrors() == 3, "3 write errors counted");
	}

	std::printf("5. retention: older than 14 days removed on the first write of a day\n");
	{
		const fs::path dir = root / "purge";
		fs::create_directories(dir);
		const std::string old20 = "metrics_" + DaysAgo(20) + ".log", old15 = "metrics_" + DaysAgo(15) + ".log";
		const std::string keep14 = "metrics_" + DaysAgo(14) + ".log", keep1 = "metrics_" + DaysAgo(1) + ".log";
		for (const std::string& n : { old20, old15, keep14, keep1, std::string("syslog_2000-01-01.log"), std::string("metrics_notes.txt") })
			std::ofstream((dir / n).string()) << "x\n";
		{
			Writer w(dir.string());
			w.logger->info("schema=1 line=0");
		}
		Expect(!fs::exists(dir / old20) && !fs::exists(dir / old15), "20 and 15 days old removed");
		Expect(fs::exists(dir / keep14) && fs::exists(dir / keep1), "14 and 1 days old kept");
		Expect(fs::exists(dir / "syslog_2000-01-01.log") && fs::exists(dir / "metrics_notes.txt"), "unrelated files untouched");
	}

	std::printf("6. local midnight: lines go to the file of their own day\n");
	{
		const fs::path dir = root / "midnight";
		std::time_t t = std::time(nullptr);
		std::tm tm = *std::localtime(&t);
		tm.tm_hour = 23;
		tm.tm_min = 59;
		tm.tm_sec = 59;
		tm.tm_isdst = -1;
		const std::time_t beforeMidnight = std::mktime(&tm);
		const std::time_t afterMidnight = beforeMidnight + 2;

		char dayBefore[16], dayAfter[16];
		std::strftime(dayBefore, sizeof(dayBefore), "%Y-%m-%d", std::localtime(&beforeMidnight));
		std::strftime(dayAfter, sizeof(dayAfter), "%Y-%m-%d", std::localtime(&afterMidnight));

		auto sink = std::make_shared<metrics_daily_sink>(dir.string(), "metrics", 14);
		sink->set_pattern("%Y-%m-%dT%H:%M:%S%z %v");
		for (const std::time_t when : { beforeMidnight, beforeMidnight, afterMidnight })
		{
			spdlog::details::log_msg msg(spdlog::log_clock::from_time_t(when), spdlog::source_loc{}, "metrics",
				spdlog::level::info, "schema=1");
			sink->log(msg);
		}
		sink->flush();
		sink.reset();

		const fs::path before = dir / ("metrics_" + std::string(dayBefore) + ".log");
		const fs::path after = dir / ("metrics_" + std::string(dayAfter) + ".log");
		std::printf("     %s: %zu line(s), %s: %zu line(s)\n", before.filename().c_str(), CountLines(before),
			after.filename().c_str(), CountLines(after));
		Expect(std::string(dayBefore) != std::string(dayAfter), "the two timestamps are on different local days");
		Expect(CountLines(before) == 2 && CountLines(after) == 1, "23:59:59 lines in the old day's file, 00:00:01 in the new one");
	}

	std::printf("7. process signals never run on the metrics worker\n");
	{
		auto pool = std::make_shared<spdlog::details::thread_pool>(64, 1, [] { g_workerThread.store(pthread_self()); BlockAllSignals(); });
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		signal(SIGUSR1, OnSignal);
		for (int i = 0; i < 2000; ++i)
			kill(getpid(), SIGUSR1);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		std::printf("     signals handled: %d, on worker: %d\n", g_signalsTotal.load(), g_signalsOnWorker.load());
		Expect(g_signalsTotal.load() > 0 && g_signalsOnWorker.load() == 0, "no signal handler ran on the worker thread");
	}

	if (argc > 1)
	{
		// The day's file is already open when the disk fills up, as in a running game: a 400 byte line still fits in
		// the stdio buffer, so ENOSPC only shows when the line is flushed
		std::printf("8. disk fills up while today's file is open (small filesystem: %s)\n", argv[1]);
		const fs::path fill = fs::path(argv[1]) / "fill";
		Writer w(argv[1]);
		w.logger->info("schema=1 line=0");
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		{
			std::FILE* fp = std::fopen(fill.c_str(), "w");
			static char chunk[65536];
			while (fp && std::fwrite(chunk, 1, sizeof(chunk), fp) == sizeof(chunk))
				;
			if (fp)
				std::fclose(fp);
		}
		// Enough lines to run past the free space left in the file's last allocated block
		const int N = 300;
		for (int i = 1; i <= N; ++i)
		{
			w.logger->info("schema=1 line={} padding=0123456789012345678901234567890123456789", i);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const unsigned long long errors = w.sink->GetWriteErrors();
		const size_t dropped = w.pool->discard_counter();
		const size_t written = CountLines(fs::path(argv[1]) / ("metrics_" + Today() + ".log"));
		std::printf("     lines in file %zu (incl. the first), write errors %llu, dropped at the queue %zu, of %d\n",
			written, errors, dropped, N + 1);
		Expect(errors > 0, "ENOSPC was hit and counted");
		Expect(written + errors + dropped == (size_t) N + 1, "every line is either in the file, counted as a write error or counted as dropped");
		std::error_code fillEc;
		fs::remove(fill, fillEc);
	}
	else
		std::printf("8. disk fills up: skipped (pass a directory on a small filesystem as the first argument)\n");

	std::error_code ec;
	fs::remove_all(root, ec);
	std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
