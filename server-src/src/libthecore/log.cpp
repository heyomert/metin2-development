#include "log.h"

#include <cstdarg>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "syslog_rotate_sink.h"

constexpr size_t LOGGER_QUEUE_SIZE = (1 << 14);
constexpr size_t LOGGER_NUM_THREADS = 1;

// Previous runs' syserr.log kept in log/ (roadmap T-1). Bounds the number of runs, not the size of one run.
constexpr size_t SYSERR_KEEP_RUNS = 30;

static std::shared_ptr<spdlog::logger> g_syslog;
static std::shared_ptr<spdlog::logger> g_syserr;

static bool g_bLogInitialized = false;

namespace
{
	struct SyserrArchiveResult
	{
		bool append = false;				// the previous content is still in syserr.log: open it without truncating
		std::string archived;				// where the previous run went, empty if nothing was moved
		std::vector<std::string> warnings;	// written to syserr once its logger exists
	};

	// "syserr_YYYY-MM-DD_HH-MM-SS.log" or "syserr_YYYY-MM-DD_HH-MM-SS_N.log" -> sort key, nothing for any other name
	std::optional<std::pair<std::string, int>> syserr_archive_key(const std::string& name)
	{
		constexpr std::string_view prefix = "syserr_", suffix = ".log";
		constexpr size_t stamp_len = std::char_traits<char>::length("YYYY-MM-DD_HH-MM-SS");
		if (name.size() < prefix.size() + stamp_len + suffix.size() || name.compare(0, prefix.size(), prefix) != 0
			|| name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
			return std::nullopt;

		const std::string stamp = name.substr(prefix.size(), stamp_len);
		for (size_t i = 0; i < stamp.size(); ++i)
		{
			const bool sep = i == 4 || i == 7 || i == 13 || i == 16 ? stamp[i] == '-' : i == 10 ? stamp[i] == '_' : std::isdigit((unsigned char)stamp[i]) != 0;
			if (!sep)
				return std::nullopt;
		}

		const std::string rest = name.substr(prefix.size() + stamp_len, name.size() - prefix.size() - stamp_len - suffix.size());
		if (rest.empty())
			return std::make_pair(stamp, 1);
		if (rest.size() < 2 || rest.size() > 4 || rest[0] != '_' || rest[1] == '0'
			|| !std::all_of(rest.begin() + 1, rest.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; }))
			return std::nullopt;
		return std::make_pair(stamp, std::stoi(rest.substr(1)));
	}

	// Moves a non-empty syserr.log of the previous run to log/ and prunes old archives. Never deletes the current
	// evidence: whenever the move does not happen, the caller appends to the existing file instead of truncating it.
	SyserrArchiveResult archive_previous_syserr()
	{
		namespace fs = std::filesystem;
		SyserrArchiveResult r;
		const fs::path current = "syserr.log";
		const fs::path dir = "log";

		try
		{
			std::error_code ec;
			bool has_content = false;
			const auto st = fs::status(current, ec);
			if (ec && st.type() != fs::file_type::not_found)
			{
				r.append = true;
				r.warnings.push_back(fmt::format("SYSERR_ARCHIVE: cannot stat syserr.log: {}; appending", ec.message()));
			}
			else if (fs::is_regular_file(st))
			{
				const auto size = fs::file_size(current, ec);
				if (ec)
				{
					r.append = true;
					r.warnings.push_back(fmt::format("SYSERR_ARCHIVE: cannot read the size of syserr.log: {}; appending", ec.message()));
				}
				else
					has_content = size > 0;
			}

			if (has_content)
			{
				// Name it after its last write: that is when the previous run stopped writing
				const auto written = fs::last_write_time(current, ec);
				const auto when = ec ? std::chrono::system_clock::now()
					: std::chrono::time_point_cast<std::chrono::system_clock::duration>(
						written - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
				const std::tm tm = spdlog::details::os::localtime(std::chrono::system_clock::to_time_t(when));
				char stamp[32];
				std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm);

				fs::create_directories(dir, ec);
				if (ec || !fs::is_directory(dir))
				{
					r.append = true;
					r.warnings.push_back(fmt::format("SYSERR_ARCHIVE: cannot use directory {}: {}; previous run kept in syserr.log, appending",
						dir.string(), ec ? ec.message() : "not a directory"));
				}
				else
				{
					// rename() replaces an existing target on POSIX: pick a name that does not exist yet
					fs::path target;
					for (int n = 1; n <= 999 && target.empty(); ++n)
					{
						const fs::path candidate = dir / (n == 1 ? fmt::format("syserr_{}.log", stamp) : fmt::format("syserr_{}_{}.log", stamp, n));
						// libc++ reports a missing path as not_found AND sets ec (ENOENT): check the type first
						const auto candidate_st = fs::symlink_status(candidate, ec);
						if (candidate_st.type() == fs::file_type::not_found)
						{
							ec.clear();
							target = candidate;
						}
						else if (ec)
							break;
					}

					if (!target.empty())
						fs::rename(current, target, ec);

					if (target.empty() || ec)
					{
						r.append = true;
						r.warnings.push_back(fmt::format("SYSERR_ARCHIVE: cannot move syserr.log to {}: {}; previous run kept in syserr.log, appending",
							target.empty() ? dir.string() : target.string(), ec ? ec.message() : "no free name"));
					}
					else
						r.archived = target.string();
				}
			}

			// Keep the newest SYSERR_KEEP_RUNS archives. Only exact syserr_<stamp>[_N].log regular files are touched, and
			// never the one just written: with a clock set back its name could sort oldest.
			const std::string just_archived = r.archived.empty() ? std::string() : fs::path(r.archived).filename().string();
			const size_t keep_others = just_archived.empty() ? SYSERR_KEEP_RUNS : SYSERR_KEEP_RUNS - 1;
			std::vector<std::pair<std::pair<std::string, int>, fs::path>> archives;
			if (fs::is_directory(dir, ec))
			{
				for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
				{
					std::error_code type_ec;
					if (!it->is_regular_file(type_ec) || it->is_symlink(type_ec))
						continue;
					const std::string name = it->path().filename().string();
					if (name == just_archived)
						continue;
					if (auto key = syserr_archive_key(name))
						archives.emplace_back(std::move(*key), it->path());
				}
				if (ec)
					r.warnings.push_back(fmt::format("SYSERR_ARCHIVE: cannot list {}: {}; old archives not pruned", dir.string(), ec.message()));
			}

			if (archives.size() > keep_others)
			{
				std::sort(archives.begin(), archives.end());
				size_t failed = 0;
				for (size_t i = 0; i < archives.size() - keep_others; ++i)
				{
					std::error_code rm_ec;
					if (!fs::remove(archives[i].second, rm_ec) || rm_ec)
						++failed;
				}
				if (failed)
					r.warnings.push_back(fmt::format("SYSERR_ARCHIVE: {} old archive(s) could not be removed", failed));
			}
		}
		catch (const std::exception& e)
		{
			r.append = r.archived.empty();
			r.warnings.push_back(fmt::format("SYSERR_ARCHIVE: unexpected error: {}{}", e.what(), r.append ? "; appending to syserr.log" : ""));
		}
		catch (...)
		{
			r.append = r.archived.empty();
			r.warnings.push_back(fmt::format("SYSERR_ARCHIVE: unexpected error{}", r.append ? "; appending to syserr.log" : ""));
		}

		return r;
	}
}

void log_init()
{
	if (g_bLogInitialized)
		return;

	spdlog::init_thread_pool(LOGGER_QUEUE_SIZE, LOGGER_NUM_THREADS);

	auto syslog_sink = std::make_shared<syslog_rotate_sink>();
	syslog_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

	g_syslog = std::make_shared<spdlog::async_logger>(
		"syslog",
		syslog_sink,
		spdlog::thread_pool(),
		spdlog::async_overflow_policy::block);

	spdlog::register_logger(g_syslog);


	// Before the sink opens the file: syserr.log then holds only this run, the previous one is in log/ (T-1)
	const SyserrArchiveResult archive = archive_previous_syserr();

	auto syserr_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("syserr.log", !archive.append);
	syserr_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%!()] %v");

	g_syserr = std::make_shared<spdlog::async_logger>(
		"syserr",
		syserr_sink,
		spdlog::thread_pool(),
		spdlog::async_overflow_policy::block);

	// Every syserr line is an error: flush it from the log thread right after writing, so a crash (SIGSEGV) does not
	// lose the lines already written but still buffered. The game thread only enqueues, as before.
	g_syserr->flush_on(spdlog::level::err);

	spdlog::register_logger(g_syserr);

	// _sys_err() drops lines until g_bLogInitialized is set: write the archive results to the loggers directly
	if (!archive.archived.empty())
		g_syslog->info("SYSERR_ARCHIVE: previous run moved to {}", archive.archived);
	for (const auto& warning : archive.warnings)
		g_syserr->error(warning);

#ifdef _DEBUG
	g_syslog->set_level(spdlog::level::debug);
#else
	g_syslog->set_level(spdlog::level::info);
#endif

	spdlog::flush_every(std::chrono::seconds(1));

	std::atexit([]() { log_destroy(); });

	g_bLogInitialized = true;
}

void log_destroy()
{
	if (!g_bLogInitialized)
		return;

	spdlog::shutdown();
	g_bLogInitialized = false;
}

void _sys_err(std::string_view str, const std::source_location& src_loc)
{
	if (!g_bLogInitialized)
		return;

	spdlog::source_loc loc;
	loc.funcname = src_loc.function_name();
	loc.line = src_loc.line();
	loc.filename = src_loc.file_name();

	g_syserr->log(loc, spdlog::level::err, str);
}

void _sys_log(int level, std::string_view str)
{
	if (!g_bLogInitialized)
		return;

	spdlog::level::level_enum lvl = spdlog::level::info;
	switch (level)
	{
		case 1:		lvl = spdlog::level::debug; break;
		case 2:		lvl = spdlog::level::trace; break;
		case 3:		lvl = spdlog::level::trace; break;
		default:	lvl = spdlog::level::info; break;
	}

	g_syslog->log(lvl, str);
}

std::string_view _format(std::string_view fmt, ...)
{
	constexpr int BUFFER_SIZE = 4096;
	thread_local char buffer[BUFFER_SIZE + 1];
	va_list args;
	va_start(args, fmt);
	int len = vsnprintf(buffer, BUFFER_SIZE, fmt.data(), args);
	va_end(args);
	return { buffer, (std::string_view::size_type) std::clamp(len, 0, BUFFER_SIZE) };
}
