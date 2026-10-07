#pragma once

#include <spdlog/sinks/base_sink.h>
#include <spdlog/details/os.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Appends each day's lines straight to <dir>/<base>_YYYY-MM-DD.log. There is no active file that gets renamed at
// midnight, so a restart on a later day cannot mix days, and no rename/copy/remove can fail half way.
// Runs only on the metrics worker thread. Nothing may escape sink_it_/flush_: spdlog's worker rethrows anything
// that is not a std::exception (spdlog/logger.h SPDLOG_LOGGER_CATCH), which would terminate the game process.
class metrics_daily_sink final : public spdlog::sinks::base_sink<std::mutex>
{
public:
	// fileMode: -1 keeps the process umask (telemetry); e.g. 0600 for a stream that must not be world-readable
	// (the SQL failure ledger). It is set when the file is created and re-applied to an existing file.
	metrics_daily_sink(std::string dir, std::string base, int keepDays, int fileMode = -1)
		: m_dir(std::move(dir)), m_base(std::move(base)), m_keepDays(keepDays), m_fileMode(fileMode)
	{
	}

	~metrics_daily_sink() override
	{
		close_();
	}

	// Lines that could not be written (directory/file not writable, short write). Read from the game thread.
	unsigned long long GetWriteErrors() const
	{
		return m_writeErrors.load(std::memory_order_relaxed);
	}

protected:
	void sink_it_(const spdlog::details::log_msg& msg) override
	{
		try
		{
			const std::tm tm = spdlog::details::os::localtime(spdlog::log_clock::to_time_t(msg.time));
			char day[16];
			std::strftime(day, sizeof(day), "%Y-%m-%d", &tm);

			if (m_day != day)
			{
				close_();
				m_day = day;
				purge_(msg.time);
			}

			if (!m_fp)
				open_();

			if (!m_fp)
			{
				m_writeErrors.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			spdlog::memory_buf_t formatted;
			formatter_->format(msg, formatted);

			// Flushed per line (one line per 10 s): a full disk or I/O error usually only shows in fflush, because a
			// line fits in the stdio buffer, so it has to be checked here for the line to be counted as lost
			if (std::fwrite(formatted.data(), 1, formatted.size(), m_fp) != formatted.size() || std::fflush(m_fp) != 0)
			{
				m_writeErrors.fetch_add(1, std::memory_order_relaxed);
				close_(); // retried on the next line
			}
		}
		catch (...)
		{
			m_writeErrors.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void flush_() override
	{
		// Nothing buffered: sink_it_ flushes every line
	}

private:
	std::string file_name_(const std::string& day) const
	{
		return m_dir + "/" + m_base + "_" + day + ".log";
	}

	void open_()
	{
		std::error_code ec;
		std::filesystem::create_directories(m_dir, ec);
#ifndef _WIN32
		if (m_fileMode >= 0)
		{
			const int fd = ::open(file_name_(m_day).c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, (mode_t) m_fileMode);
			if (fd < 0)
				return;
			if (::fchmod(fd, (mode_t) m_fileMode) != 0 || !(m_fp = ::fdopen(fd, "a")))
				::close(fd);
			return;
		}
#endif
		m_fp = std::fopen(file_name_(m_day).c_str(), "a");
	}

	void close_()
	{
		if (m_fp)
		{
			std::fclose(m_fp);
			m_fp = nullptr;
		}
	}

	// Removes <base>_YYYY-MM-DD.log files older than m_keepDays; every error is ignored.
	void purge_(spdlog::log_clock::time_point now)
	{
		const std::tm cutoffTm = spdlog::details::os::localtime(
			spdlog::log_clock::to_time_t(now - std::chrono::hours(24 * m_keepDays)));
		char cutoff[16];
		std::strftime(cutoff, sizeof(cutoff), "%Y-%m-%d", &cutoffTm);

		const std::string prefix = m_base + "_";
		const size_t nameLength = prefix.size() + 10 + 4; // YYYY-MM-DD + ".log"

		std::error_code ec;
		std::filesystem::directory_iterator it(m_dir, ec), end;
		for (; !ec && it != end; it.increment(ec))
		{
			const std::string name = it->path().filename().string();
			if (name.size() != nameLength || name.compare(0, prefix.size(), prefix) != 0)
				continue;

			if (name.compare(prefix.size(), 10, cutoff) < 0)
			{
				std::error_code removeEc;
				std::filesystem::remove(it->path(), removeEc);
			}
		}
	}

private:
	const std::string m_dir;
	const std::string m_base;
	const int m_keepDays;
	const int m_fileMode;

	std::string m_day;
	std::FILE* m_fp = nullptr;
	std::atomic<unsigned long long> m_writeErrors{0};
};
