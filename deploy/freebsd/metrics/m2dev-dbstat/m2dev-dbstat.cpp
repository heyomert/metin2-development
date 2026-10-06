// m2dev-dbstat: read-only MariaDB + FreeBSD health sampler (docs/monitoring.md -> "MariaDB / OS (dbstat)",
// design: docs/engineering/db-step1b-collector.md).
//
// Every interval it writes key=value lines (kind=db, kind=os, kind=proc) to <out>/dbstat_YYYY-MM-DD.log.
// It depends only on the MariaDB client library of the installed server package and FreeBSD base libraries
// (libkvm, libdevstat); nothing from the game (no libsql, libthecore, spdlog). game, db and MariaDB never depend on it:
// if it stops, only these lines stop.
//
// MariaDB access: one persistent connection over the unix socket, as a USAGE-only user (unix_socket auth). It sends
// exactly SHOW GLOBAL STATUS WHERE Variable_name IN (...) per interval and SELECT VERSION(), @@long_query_time,
// @@datadir on connect and every 60 intervals. No writes, no locks, no processlist (no PROCESS privilege needed).
#include "dbstat_core.h"

#include <mysql.h>

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <devstat.h>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <kvm.h>
#include <libgen.h>
#include <limits.h>
#include <paths.h>
#include <pwd.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <initializer_list>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace
{
	volatile sig_atomic_t g_stop = 0;

	void OnSignal(int)
	{
		g_stop = 1;
	}

	struct Options
	{
		const char* socket = "/var/run/mysql/mysql.sock";
		const char* user = nullptr; // default: the OS user (unix_socket auth)
		const char* out = "/var/log/m2dev-metrics";
		const char* disk = nullptr; // default: the disk holding MariaDB's datadir
		int interval = 10;
		int keepDays = 14;
		long samples = -1; // tests: stop after N intervals
		bool toStdout = false;
		const char* procs[32] = {};
		int procCount = 0;
	};

	void Usage()
	{
		fprintf(stderr, "usage: m2dev-dbstat [--socket PATH] [--user NAME] [--out DIR] [--interval SEC] [--keep-days N]\n"
			"                    [--disk NAME] [--proc PATTERN]... [--stdout] [--samples N]\n");
	}

	bool ParseArgs(int argc, char** argv, Options& o)
	{
		for (int i = 1; i < argc; ++i)
		{
			const char* a = argv[i];
			const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
			auto need = [&](const char* name) { if (!v) { fprintf(stderr, "%s needs a value\n", name); return false; } ++i; return true; };
			if (!strcmp(a, "--socket")) { if (!need(a)) return false; o.socket = v; }
			else if (!strcmp(a, "--user")) { if (!need(a)) return false; o.user = v; }
			else if (!strcmp(a, "--out")) { if (!need(a)) return false; o.out = v; }
			else if (!strcmp(a, "--disk")) { if (!need(a)) return false; o.disk = v; }
			else if (!strcmp(a, "--interval")) { if (!need(a)) return false; o.interval = atoi(v); }
			else if (!strcmp(a, "--keep-days")) { if (!need(a)) return false; o.keepDays = atoi(v); }
			else if (!strcmp(a, "--samples")) { if (!need(a)) return false; o.samples = atol(v); }
			else if (!strcmp(a, "--proc")) { if (!need(a)) return false; if (o.procCount < 32) o.procs[o.procCount++] = v; }
			else if (!strcmp(a, "--stdout")) o.toStdout = true;
			else { Usage(); return false; }
		}
		if (o.interval < 1 || o.keepDays < 1)
		{
			Usage();
			return false;
		}
		if (o.procCount == 0) // what this project runs on the DB host (argv[0] names, see dbstat_core.h)
			for (const char* p : { "mariadbd", "db", "game_auth", "channel*_core*", "m2dev-dbstat" })
				o.procs[o.procCount++] = p;
		return true;
	}

	// Value safe for key=value: no spaces or '=' (MariaDB version strings are plain, but never trust input)
	void SafeCopy(char* dst, size_t size, const char* src)
	{
		size_t i = 0;
		for (; src && src[i] && i + 1 < size; ++i)
			dst[i] = (src[i] == ' ' || src[i] == '=' || src[i] == '\t' || src[i] == '\n') ? '_' : src[i];
		dst[i] = '\0';
	}

	// --- output ----------------------------------------------------------------------------------------------------

	// <dir>/dbstat_YYYY-MM-DD.log, appended and flushed per line, files older than keepDays removed at day change.
	// A failed write drops that line and is counted; nothing is buffered (a full disk cannot grow this process).
	class DailyFile
	{
	public:
		DailyFile(const char* dir, int keepDays, bool toStdout) : m_dir(dir), m_keepDays(keepDays), m_stdout(toStdout) {}
		~DailyFile() { Close(); }

		void Write(const char* line, size_t len, time_t now)
		{
			if (m_stdout)
			{
				if (fwrite(line, 1, len, stdout) != len || fflush(stdout) != 0)
					++m_errors;
				return;
			}
			char day[16];
			struct tm tm;
			localtime_r(&now, &tm);
			strftime(day, sizeof(day), "%Y-%m-%d", &tm);
			if (strcmp(day, m_day) != 0)
			{
				Close();
				memcpy(m_day, day, sizeof(day));
				Purge(now);
			}
			if (!m_fp)
			{
				char path[PATH_MAX];
				snprintf(path, sizeof(path), "%s/dbstat_%s.log", m_dir, m_day);
				m_fp = fopen(path, "a");
			}
			if (!m_fp || fwrite(line, 1, len, m_fp) != len || fflush(m_fp) != 0)
			{
				++m_errors;
				Close(); // retried on the next line
			}
		}

		unsigned long long Errors() const { return m_errors; }

	private:
		void Close()
		{
			if (m_fp)
			{
				fclose(m_fp);
				m_fp = nullptr;
			}
		}

		void Purge(time_t now)
		{
			const time_t cutoffTime = now - static_cast<time_t>(m_keepDays) * 86400;
			struct tm tm;
			localtime_r(&cutoffTime, &tm);
			char cutoff[16];
			strftime(cutoff, sizeof(cutoff), "%Y-%m-%d", &tm);
			DIR* d = opendir(m_dir);
			if (!d)
				return;
			while (struct dirent* e = readdir(d))
			{
				// exactly dbstat_YYYY-MM-DD.log
				if (strlen(e->d_name) != 7 + 10 + 4 || strncmp(e->d_name, "dbstat_", 7) || strcmp(e->d_name + 17, ".log"))
					continue;
				if (strncmp(e->d_name + 7, cutoff, 10) < 0)
				{
					char path[PATH_MAX];
					snprintf(path, sizeof(path), "%s/%s", m_dir, e->d_name);
					unlink(path);
				}
			}
			closedir(d);
		}

		const char* m_dir;
		int m_keepDays;
		bool m_stdout;
		char m_day[16] = {};
		FILE* m_fp = nullptr;
		unsigned long long m_errors = 0;
	};

	size_t Prefix(char* buf, size_t size, time_t now, const char* host, const char* kind)
	{
		struct tm tm;
		localtime_r(&now, &tm);
		size_t len = strftime(buf, size, "%Y-%m-%dT%H:%M:%S%z", &tm);
		int n = snprintf(buf + len, size - len, " schema=1 src=dbstat kind=%s host=%s", kind, host);
		return len + (n > 0 ? static_cast<size_t>(n) : 0);
	}

	size_t Append(char* buf, size_t size, size_t len, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
	size_t Append(char* buf, size_t size, size_t len, const char* fmt, ...)
	{
		if (len >= size)
			return len;
		va_list ap;
		va_start(ap, fmt);
		const int n = vsnprintf(buf + len, size - len, fmt, ap);
		va_end(ap);
		if (n < 0)
			return len;
		return len + (static_cast<size_t>(n) < size - len ? static_cast<size_t>(n) : size - len - 1);
	}

	// --- MariaDB ---------------------------------------------------------------------------------------------------

	class Db
	{
	public:
		Db(const Options& o) : m_o(o)
		{
			size_t len = Append(m_query, sizeof(m_query), 0, "SHOW GLOBAL STATUS WHERE Variable_name IN ('Uptime'");
			for (size_t i = 0; i < dbstat::kDbFieldCount; ++i)
				len = Append(m_query, sizeof(m_query), len, ",'%s'", dbstat::kDbFields[i].var);
			Append(m_query, sizeof(m_query), len, ")");
			m_backoff = o.interval;
		}

		~Db() { Close(); }

		// One interval: connect if due, read, build the db line. Never blocks longer than the client timeouts.
		size_t Sample(char* buf, size_t size, size_t len, time_t now, unsigned long long writeErrors)
		{
			if (!m_h && now >= m_retryAt)
				Connect(now);
			if (m_h && (m_sinceInfo++ >= 60 || !m_info))
				ReadInfo(now);

			dbstat::DbSample cur;
			if (m_h && !ReadStatus(cur, now))
				m_h = nullptr; // ReadStatus closed it and scheduled the retry

			if (!m_h)
				return Append(buf, size, len, " up=0 err=%s retry_in_s=%lld write_errors=%llu\n", m_err,
					static_cast<long long>(m_retryAt > now ? m_retryAt - now : 0), writeErrors);

			const dbstat::DbWindow w = dbstat::ComputeDb(m_havePrev ? &m_prev : nullptr, cur);
			len = Append(buf, size, len, " up=1 version=%s long_query_time_s=%s uptime_s=%llu", m_version, m_lqt,
				static_cast<unsigned long long>(cur.uptime));
			if (w.haveWindow)
				len = Append(buf, size, len, " window_s=%llu", static_cast<unsigned long long>(w.window_s));
			else
				len = Append(buf, size, len, " window_s=-");
			len = Append(buf, size, len, " first=%d restart=%d reset=%d na=%d", w.first ? 1 : 0, w.restart ? 1 : 0, w.reset, w.na);
			len = dbstat::FormatDbFields(w, buf, size, len);
			len = Append(buf, size, len, " write_errors=%llu\n", writeErrors);
			m_prev = cur;
			m_havePrev = true;
			return len;
		}

		const char* Disk() const { return m_disk[0] ? m_disk : nullptr; }

	private:
		void Close()
		{
			if (m_h)
			{
				mysql_close(m_h);
				m_h = nullptr;
			}
		}

		void Fail(const char* err, time_t now)
		{
			Close();
			snprintf(m_err, sizeof(m_err), "%s", err);
			m_retryAt = now + m_backoff;
			m_backoff = m_backoff * 2 > 60 ? 60 : m_backoff * 2; // 10, 20, 40, 60, 60, ... (never a storm)
			m_info = false;
		}

		static const char* Classify(unsigned e)
		{
			switch (e)
			{
				case 1045: case 1698: return "auth";      // access denied
				case 2002: case 2003: return "connect";   // no server on the socket
				case 2006: return "gone";
				case 2013: return "lost";                 // includes read timeout
				default: return "other";
			}
		}

		void Connect(time_t now)
		{
			m_h = mysql_init(nullptr);
			if (!m_h)
			{
				Fail("other", now);
				return;
			}
			unsigned connectTimeout = 3, rwTimeout = 5;
			mysql_options(m_h, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
			mysql_options(m_h, MYSQL_OPT_READ_TIMEOUT, &rwTimeout);
			mysql_options(m_h, MYSQL_OPT_WRITE_TIMEOUT, &rwTimeout);
			const char* user = m_o.user;
			if (!user)
				if (const struct passwd* pw = getpwuid(geteuid()))
					user = pw->pw_name;
			if (!mysql_real_connect(m_h, nullptr, user, nullptr, nullptr, 0, m_o.socket, 0))
			{
				Fail(Classify(mysql_errno(m_h)), now);
				return;
			}
			m_backoff = m_o.interval;
			m_info = false;
		}

		void ReadInfo(time_t now)
		{
			m_sinceInfo = 0;
			if (mysql_query(m_h, "SELECT VERSION(), @@long_query_time, @@datadir"))
			{
				Fail(Classify(mysql_errno(m_h)), now);
				return;
			}
			if (MYSQL_RES* r = mysql_store_result(m_h))
			{
				if (MYSQL_ROW row = mysql_fetch_row(r))
				{
					SafeCopy(m_version, sizeof(m_version), row[0]);
					SafeCopy(m_lqt, sizeof(m_lqt), row[1]);
					m_disk[0] = '\0';
					struct statfs sf;
					if (row[2] && statfs(row[2], &sf) == 0)
						dbstat::DiskFromDevice(sf.f_mntfromname, m_disk, sizeof(m_disk));
				}
				mysql_free_result(r);
			}
			m_info = true;
		}

		bool ReadStatus(dbstat::DbSample& cur, time_t now)
		{
			if (!m_h)
				return false;
			if (mysql_query(m_h, m_query))
			{
				Fail(Classify(mysql_errno(m_h)), now);
				return false;
			}
			MYSQL_RES* r = mysql_store_result(m_h);
			if (!r)
			{
				Fail(Classify(mysql_errno(m_h)), now);
				return false;
			}
			while (MYSQL_ROW row = mysql_fetch_row(r))
				cur.Set(row[0], row[1]);
			mysql_free_result(r);
			return true;
		}

		const Options& m_o;
		MYSQL* m_h = nullptr;
		char m_query[2048] = {};
		char m_err[16] = "none";
		char m_version[64] = "-";
		char m_lqt[32] = "-";
		char m_disk[32] = {};
		time_t m_retryAt = 0;
		int m_backoff = 10;
		bool m_info = false;
		int m_sinceInfo = 0;
		bool m_havePrev = false;
		dbstat::DbSample m_prev;
	};

	// --- FreeBSD ---------------------------------------------------------------------------------------------------

	class Os
	{
	public:
		Os()
		{
			char err[_POSIX2_LINE_MAX];
			m_kd = kvm_openfiles(nullptr, _PATH_DEVNULL, nullptr, O_RDONLY, err);
			m_page = getpagesize();
			if (devstat_checkversion(nullptr) == 0)
			{
				m_cur.dinfo = static_cast<struct devinfo*>(calloc(1, sizeof(struct devinfo)));
				m_last.dinfo = static_cast<struct devinfo*>(calloc(1, sizeof(struct devinfo)));
				m_devstat = m_cur.dinfo && m_last.dinfo;
			}
		}

		~Os()
		{
			if (m_kd)
				kvm_close(m_kd);
			for (struct statinfo* s : { &m_cur, &m_last })
				if (s->dinfo)
				{
					free(s->dinfo->mem_ptr);
					free(s->dinfo);
				}
		}

		size_t Sample(char* buf, size_t size, size_t len, const char* disk)
		{
			double load[1];
			if (getloadavg(load, 1) == 1)
				len = Append(buf, size, len, " load1=%.2f", load[0]);
			else
				len = Append(buf, size, len, " load1=NA");

			u_int freePages = 0;
			size_t sz = sizeof(freePages);
			if (sysctlbyname("vm.stats.vm.v_free_count", &freePages, &sz, nullptr, 0) == 0)
				len = Append(buf, size, len, " mem_free_mb=%llu", static_cast<unsigned long long>(freePages) * m_page / 1048576);
			else
				len = Append(buf, size, len, " mem_free_mb=NA");

			struct kvm_swap sw;
			if (m_kd && kvm_getswapinfo(m_kd, &sw, 1, 0) >= 0)
				len = Append(buf, size, len, " swap_used_mb=%llu", static_cast<unsigned long long>(sw.ksw_used) * m_page / 1048576);
			else
				len = Append(buf, size, len, " swap_used_mb=NA");

			return Disk(buf, size, len, disk);
		}

		// One line per tracked process (argv[0] basename matches a --proc pattern)
		template <typename Emit>
		void Procs(const Options& o, Emit emit)
		{
			if (!m_kd)
				return;
			int n = 0;
			struct kinfo_proc* kp = kvm_getprocs(m_kd, KERN_PROC_PROC, 0, &n);
			if (!kp)
				return;
			dbstat::ProcPrev next[64];
			int nextCount = 0;
			for (int i = 0; i < n; ++i)
			{
				const char* name = kp[i].ki_comm;
				char argv0[PATH_MAX];
				if (char** av = kvm_getargv(m_kd, &kp[i], 0); av && av[0])
				{
					snprintf(argv0, sizeof(argv0), "%s", av[0]);
					name = basename(argv0);
				}
				bool tracked = false;
				for (int p = 0; p < o.procCount && !tracked; ++p)
					tracked = fnmatch(o.procs[p], name, 0) == 0;
				if (!tracked)
					continue;
				const dbstat::ProcPrev* prev = nullptr;
				for (int j = 0; j < m_prevCount; ++j)
					if (m_prev[j].pid == kp[i].ki_pid)
						prev = &m_prev[j];
				uint64_t cpuUs = 0;
				const uint64_t runtime = kp[i].ki_runtime;
				const dbstat::ProcState st = dbstat::ProcDelta(prev, kp[i].ki_pid, runtime, &cpuUs);
				emit(name, kp[i].ki_pid, st, cpuUs, static_cast<unsigned long long>(kp[i].ki_rssize) * m_page / 1024);
				if (nextCount < 64)
					next[nextCount++] = { kp[i].ki_pid, runtime };
			}
			memcpy(m_prev, next, sizeof(next[0]) * nextCount);
			m_prevCount = nextCount;
		}

	private:
		size_t Disk(char* buf, size_t size, size_t len, const char* disk)
		{
			if (!disk || !m_devstat || devstat_getdevs(nullptr, &m_cur) == -1)
				return Append(buf, size, len, " disk=%s r_s=NA w_s=NA mb_r_s=NA mb_w_s=NA ms_r=NA ms_w=NA qlen=NA busy_pct=NA", disk ? disk : "NA");
			len = Append(buf, size, len, " disk=%s", disk);
			struct devstat* c = Find(m_cur, disk);
			struct devstat* l = m_haveLast ? Find(m_last, disk) : nullptr;
			if (c && l)
			{
				long double rps = 0, wps = 0, mbr = 0, mbw = 0, msr = 0, msw = 0, busy = 0;
				uint64_t qlen = 0;
				const long double etime = m_cur.snap_time - m_last.snap_time;
				devstat_compute_statistics(c, l, etime,
					DSM_TRANSFERS_PER_SECOND_READ, &rps, DSM_TRANSFERS_PER_SECOND_WRITE, &wps,
					DSM_MB_PER_SECOND_READ, &mbr, DSM_MB_PER_SECOND_WRITE, &mbw,
					DSM_MS_PER_TRANSACTION_READ, &msr, DSM_MS_PER_TRANSACTION_WRITE, &msw,
					DSM_QUEUE_LENGTH, &qlen, DSM_BUSY_PCT, &busy, DSM_NONE);
				len = Append(buf, size, len, " r_s=%.1Lf w_s=%.1Lf mb_r_s=%.3Lf mb_w_s=%.3Lf ms_r=%.2Lf ms_w=%.2Lf qlen=%llu busy_pct=%.1Lf",
					rps, wps, mbr, mbw, msr, msw, static_cast<unsigned long long>(qlen), busy);
			}
			else
				len = Append(buf, size, len, c ? " r_s=- w_s=- mb_r_s=- mb_w_s=- ms_r=- ms_w=- qlen=- busy_pct=-"
					: " r_s=NA w_s=NA mb_r_s=NA mb_w_s=NA ms_r=NA ms_w=NA qlen=NA busy_pct=NA");
			// keep this snapshot as the next window's start (swap the buffers, devstat reuses them)
			struct statinfo tmp = m_last;
			m_last = m_cur;
			m_cur = tmp;
			m_haveLast = c != nullptr;
			return len;
		}

		static struct devstat* Find(struct statinfo& s, const char* disk)
		{
			for (int i = 0; i < s.dinfo->numdevs; ++i)
			{
				char name[DEVSTAT_NAME_LEN + 16];
				snprintf(name, sizeof(name), "%s%d", s.dinfo->devices[i].device_name, s.dinfo->devices[i].unit_number);
				if (!strcmp(name, disk))
					return &s.dinfo->devices[i];
			}
			return nullptr;
		}

		kvm_t* m_kd = nullptr;
		int m_page = 4096;
		bool m_devstat = false;
		bool m_haveLast = false;
		struct statinfo m_cur = {};
		struct statinfo m_last = {};
		dbstat::ProcPrev m_prev[64];
		int m_prevCount = 0;
	};

	void SleepUntil(const struct timespec& t)
	{
		while (!g_stop && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, nullptr) == EINTR)
			;
	}
}

int main(int argc, char** argv)
{
	Options o;
	if (!ParseArgs(argc, argv, o))
		return 2;

	signal(SIGPIPE, SIG_IGN);
	struct sigaction sa = {};
	sa.sa_handler = OnSignal;
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);

	if (mysql_library_init(0, nullptr, nullptr))
	{
		fprintf(stderr, "mysql_library_init failed\n");
		return 1;
	}

	char host[MAXHOSTNAMELEN] = "-";
	gethostname(host, sizeof(host));
	char safeHost[MAXHOSTNAMELEN];
	SafeCopy(safeHost, sizeof(safeHost), host);

	DailyFile out(o.out, o.keepDays, o.toStdout);
	Db db(o);
	Os os;

	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);
	char line[4096];
	for (long n = 0; !g_stop && (o.samples < 0 || n < o.samples); ++n)
	{
		const time_t now = time(nullptr);

		size_t len = Prefix(line, sizeof(line), now, safeHost, "db");
		len = db.Sample(line, sizeof(line), len, now, out.Errors());
		out.Write(line, len, now);

		len = Prefix(line, sizeof(line), now, safeHost, "os");
		len = os.Sample(line, sizeof(line), len, o.disk ? o.disk : db.Disk());
		len = Append(line, sizeof(line), len, "\n");
		out.Write(line, len, now);

		os.Procs(o, [&](const char* name, int pid, dbstat::ProcState st, uint64_t cpuUs, unsigned long long rssKb)
		{
			char safeName[64];
			SafeCopy(safeName, sizeof(safeName), name);
			size_t l = Prefix(line, sizeof(line), now, safeHost, "proc");
			l = Append(line, sizeof(line), l, " name=%s pid=%d", safeName, pid);
			if (st == dbstat::ProcState::Value)
				l = Append(line, sizeof(line), l, " first=0 restart=0 cpu_us=%llu", static_cast<unsigned long long>(cpuUs));
			else
				l = Append(line, sizeof(line), l, " first=%d restart=%d cpu_us=-", st == dbstat::ProcState::First ? 1 : 0,
					st == dbstat::ProcState::Restart ? 1 : 0);
			l = Append(line, sizeof(line), l, " rss_kb=%llu\n", rssKb);
			out.Write(line, l, now);
		});

		next.tv_sec += o.interval;
		if (o.samples < 0 || n + 1 < o.samples)
			SleepUntil(next);
	}

	mysql_library_end();
	return 0;
}
