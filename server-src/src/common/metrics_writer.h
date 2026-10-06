#pragma once

#include "metrics_daily_sink.h"

#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>

#include <memory>
#include <string>

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#endif

// One telemetry stream: <dir>/<base>_YYYY-MM-DD.log written by its OWN spdlog queue and worker thread with
// discard_new (docs/monitoring.md -> "Arıza davranışı"). Each stream (game health, game SQL, db SQL) owns one, so a
// stalled or failing stream never makes another wait, and its losses never show in another stream's counters.
// The global syslog/syserr pool is never used. Write() never blocks and never throws: when the queue is full the
// line is dropped and counted (Dropped()), when the file cannot be written it is counted (WriteErrors()).
class metrics_writer
{
public:
	metrics_writer() = default;
	~metrics_writer() { Stop(); }

	metrics_writer(const metrics_writer&) = delete;
	metrics_writer& operator=(const metrics_writer&) = delete;

	// Returns false (stream stays off) if the queue or its worker cannot be created; never throws.
	// extraSink: tests only (a second sink on the same logger, e.g. a deliberately slow one).
	bool Start(const std::string& dir, const std::string& base, int keepDays, size_t queueSize,
		spdlog::sink_ptr extraSink = nullptr)
	{
		Stop();

		try
		{
			m_sink = std::make_shared<metrics_daily_sink>(dir, base, keepDays);
			m_sink->set_pattern("%Y-%m-%dT%H:%M:%S%z %v");

			// The worker blocks every signal so the process signal handlers (SIGVTALRM checkpoint, SIGTERM, ...)
			// keep running on the threads that expect them.
			m_pool = std::make_shared<spdlog::details::thread_pool>(queueSize, 1, []()
			{
#ifndef _WIN32
				sigset_t all;
				sigfillset(&all);
				pthread_sigmask(SIG_BLOCK, &all, nullptr);
#endif
			});

			if (extraSink)
				m_logger = std::make_shared<spdlog::async_logger>(base, spdlog::sinks_init_list{ extraSink, m_sink }, m_pool,
					spdlog::async_overflow_policy::discard_new);
			else
				m_logger = std::make_shared<spdlog::async_logger>(base, m_sink, m_pool, spdlog::async_overflow_policy::discard_new);
			m_logger->set_level(spdlog::level::info);
			m_logger->set_error_handler([](const std::string&) {}); // write failures are counted by the sink
		}
		catch (...)
		{
			m_logger.reset();
			m_pool.reset();
			m_sink.reset();
			return false;
		}

		return true;
	}

	// Drains what is queued, then joins the worker. Idempotent.
	void Stop()
	{
		if (!m_logger)
			return;

		try
		{
			m_logger->flush();
		}
		catch (...)
		{
		}

		// Release the logger first, then the pool: the pool destructor drains the queue and joins its worker.
		m_logger.reset();
		m_pool.reset();
		m_sink.reset();
	}

	bool IsRunning() const { return m_logger != nullptr; }

	// Caller's thread; queues the line or drops it, never waits.
	void Write(const char* line, size_t length)
	{
		if (!m_logger)
			return;

		try
		{
			m_logger->log(spdlog::level::info, spdlog::string_view_t(line, length));
		}
		catch (...)
		{
		}
	}

	// Lines dropped because this stream's queue was full.
	unsigned long long Dropped() const { return m_pool ? (unsigned long long) m_pool->discard_counter() : 0; }

	// Lines that reached the sink but could not be written.
	unsigned long long WriteErrors() const { return m_sink ? m_sink->GetWriteErrors() : 0; }

private:
	std::shared_ptr<metrics_daily_sink>				m_sink;
	std::shared_ptr<spdlog::details::thread_pool>	m_pool;
	std::shared_ptr<spdlog::async_logger>			m_logger;
};
