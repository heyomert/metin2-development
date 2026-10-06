// Characterization tests for libsql CAsyncSQL: records how the CURRENT code behaves under errors, so that a later fix
// can be checked against the same scenarios. Runs only against a temporary MariaDB started by run.sh (never the live
// server). One scenario per process: fresh schema, fresh CAsyncSQL, fresh syserr.log in the working directory.
//
//   sqlrt <S1..S12|S9b|versions>      environment (set by run.sh): RT_PORT, RT_PW (TCP user rt), RT_SOCK (admin
//                                     socket, root), RT_TOKEN (per-run token; see guard(): refuses any other server)
//
// Evidence used, in this order: rows in the database (was a statement really applied?), the client's own syserr.log
// lines ("AsyncSQL: query failed ... errno: N", "AsyncSQL: retrying"), queue counters. The copy queue is read only
// after a quiet period, when the worker is waiting on its condition variable (the worker owns it without a lock).
//
// Every report is followed by a "stats" line: CAsyncSQL::CollectStats() (DB step 1c telemetry) next to the same
// evidence, so the counters can be checked against what really happened. Only deterministic values are printed there
// (counts and 0/1 flags; no durations, and no exec count: how many queries the worker finishes before Quit() and
// how many the untimed shutdown loop runs depends on thread timing, 1c S9b), so runs stay comparable. Drop the
// "stats" lines to compare with output from before 1c.
#include "libsql/AsyncSQL.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

namespace
{
	const char* g_scn = "";
	MYSQL* g_admin = nullptr;

	// Fails with ER_PASSWORD_NO_MATCH (1133, in AsyncSQL's retry list) until the user ghost@localhost exists; the same
	// statement then succeeds. Measured on MariaDB 11.8.9 (probe before this tool).
	const char* SETPW = "SET PASSWORD FOR ghost@localhost = PASSWORD('x')";

	[[noreturn]] void die(const std::string& what, MYSQL* h)
	{
		printf("%s FATAL %s: %s\n", g_scn, what.c_str(), h ? mysql_error(h) : "");
		fflush(stdout);
		_exit(3);
	}

	MYSQL* connect_admin()
	{
		MYSQL* h = mysql_init(nullptr);
		if (!mysql_real_connect(h, nullptr, "root", nullptr, nullptr, 0, getenv("RT_SOCK"), 0))
			die("admin connect", h);
		return h;
	}

	unsigned sqlrun(MYSQL* h, const std::string& q)
	{
		if (mysql_real_query(h, q.c_str(), q.size()))
			return mysql_errno(h);
		do
		{
			if (MYSQL_RES* r = mysql_store_result(h))
				mysql_free_result(r);
		} while (mysql_next_result(h) == 0);
		return 0;
	}

	void must(MYSQL* h, const std::string& q)
	{
		if (sqlrun(h, q))
			die(q, h);
	}

	std::string sscalar(MYSQL* h, const std::string& q)
	{
		if (mysql_real_query(h, q.c_str(), q.size()))
			die(q, h);
		std::string v = "NULL";
		if (MYSQL_RES* r = mysql_store_result(h))
		{
			MYSQL_ROW row = mysql_fetch_row(r);
			if (row && row[0])
				v = row[0];
			mysql_free_result(r);
		}
		return v;
	}

	long long scalar(MYSQL* h, const std::string& q)
	{
		const std::string v = sscalar(h, q);
		return v == "NULL" ? -1 : atoll(v.c_str());
	}

	void ms(int n)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(n));
	}

	long long marker(const std::string& tag)
	{
		return scalar(g_admin, "SELECT COUNT(*) FROM rt.m WHERE tag='" + tag + "'");
	}

	long long markers(const std::string& prefix)
	{
		return scalar(g_admin, "SELECT COUNT(*) FROM rt.m WHERE tag LIKE '" + prefix + "%'");
	}

	long long ghost_password_set()
	{
		return scalar(g_admin, "SELECT COUNT(*) FROM mysql.global_priv WHERE User='ghost' "
			"AND JSON_VALUE(Priv, '$.authentication_string') <> ''");
	}

	// Client-side evidence: lines the AsyncSQL worker wrote. spdlog flushes every second; callers wait >1 s first.
	long long syserr(const char* needle)
	{
		std::ifstream in("syserr.log");
		std::string line;
		long long n = 0;
		while (std::getline(in, line))
			if (line.find(needle) != std::string::npos)
				++n;
		return n;
	}

	std::string errnos()
	{
		std::string s;
		for (int e : { 1133, 1064, 1205, 1213, 2006, 2013, 2014 })
		{
			const std::string needle = "errno: " + std::to_string(e) + ")";
			if (const long long n = syserr(needle.c_str()))
				s += "e" + std::to_string(e) + "=" + std::to_string(n) + " ";
		}
		return s + "retrying=" + std::to_string(syserr("AsyncSQL: retrying")) + " reconnected="
			+ std::to_string(syserr("was reconnected"));
	}

	long long status(const char* name)
	{
		return scalar(g_admin, std::string("SELECT VARIABLE_VALUE FROM information_schema.GLOBAL_STATUS WHERE VARIABLE_NAME='")
			+ name + "'");
	}

	// Makes a transaction "heavy" (2000 inserted rows in a table nobody else touches) so InnoDB chooses the other
	// transaction as the deadlock victim. Inserting into rt.h locks no rt.d rows (a range UPDATE on rt.d did: run 1).
	void make_heavy(MYSQL* h)
	{
		must(h, "SET SESSION max_recursive_iterations = 5000");
		must(h, "INSERT INTO rt.h WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM s WHERE i < 2000) "
			"SELECT i, 0 FROM s");
	}

	// Fail-closed guard, before any write: this tool drops a database, kills connections and provokes lock waits and
	// deadlocks, so it must only ever reach the throwaway server run.sh starts. Three independent checks:
	//   1. the admin connection's data directory is run.sh's temporary one,
	//   2. that server holds the random token run.sh wrote for this run (a real server never has it),
	//   3. (in start) the AsyncSQL connection, which goes over TCP, reached that same server.
	const char* TEMP_DATADIR = "/var/tmp/m2sqlrt/";

	void guard()
	{
		const char* token = getenv("RT_TOKEN");
		if (!token || strlen(token) < 32)
			die("guard: RT_TOKEN missing; run through tools/sql-reliability/run.sh", nullptr);
		const std::string datadir = sscalar(g_admin, "SELECT @@datadir");
		if (datadir.rfind(TEMP_DATADIR, 0) != 0)
			die("guard: refusing to run against datadir " + datadir, nullptr);
		const std::string q = "SELECT token FROM m2sqlrt_guard.token";
		if (mysql_real_query(g_admin, q.c_str(), q.size()))
			die("guard: no run token on this server", g_admin);
		std::string found;
		if (MYSQL_RES* r = mysql_store_result(g_admin))
		{
			if (MYSQL_ROW row = mysql_fetch_row(r); row && row[0])
				found = row[0];
			mysql_free_result(r);
		}
		if (found != token)
			die("guard: run token does not match", nullptr);
	}

	struct Probe : public CAsyncSQL
	{
		size_t CopySize() const { return m_queue_query_copy.size(); } // quiet periods only
	};

	void setup_schema()
	{
		must(g_admin, "DROP DATABASE IF EXISTS rt");
		must(g_admin, "DROP USER IF EXISTS ghost@localhost");
		must(g_admin, "CREATE DATABASE rt");
		must(g_admin, "CREATE TABLE rt.m (id INT AUTO_INCREMENT PRIMARY KEY, tag VARCHAR(32) NOT NULL) ENGINE=InnoDB");
		must(g_admin, "CREATE TABLE rt.d (id INT PRIMARY KEY, v INT NOT NULL) ENGINE=InnoDB");
		must(g_admin, "CREATE TABLE rt.h (id INT PRIMARY KEY, v INT NOT NULL) ENGINE=InnoDB");
		must(g_admin, "SET SESSION max_recursive_iterations = 5000");
		must(g_admin, "INSERT INTO rt.d WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM s WHERE i < 2000) "
			"SELECT i, 0 FROM s");
	}

	void start(Probe& sql)
	{
		sql.Setup("127.0.0.1", "rt", getenv("RT_PW"), "rt", "latin1", false, atoi(getenv("RT_PORT")));
		for (int i = 0; i < 100 && !sql.IsConnected(); ++i)
			ms(50);
		if (!sql.IsConnected())
			die("asyncsql connect", nullptr);
		// Guard 3: the TCP connection must be visible from the admin connection, i.e. the same (temporary) server
		if (scalar(g_admin, "SELECT COUNT(*) FROM information_schema.processlist WHERE user = 'rt'") < 1)
			die("guard: AsyncSQL connection is not on the temporary server", nullptr);
	}

	void stats(const char* phase, Probe& sql)
	{
		SQLStats s;
		sql.CollectStats(s);
		const auto u = [](uint64_t v) { return (unsigned long long) v; };
		uint64_t buckets = 0;
		for (uint64_t e : s.errnoCount)
			buckets += e;
		// q/cq cross-checks against the legacy view are valid because reports run in quiet periods only
		printf("%s stats phase=%s q=%llu cq=%llu rq=%llu oldest=%d stuck=%d worker=%d pushed=%llu ok=%llu err=%llu"
			" retry=%llu reconnect_seen=%llu e2006=%llu e2013=%llu e2014=%llu e1205=%llu e1213=%llu e_other=%llu"
			" exec_n_le_ok=%d unexecuted_at_quit=%llu q_matches=%d cq_matches=%d buckets_eq_err_plus_retry=%d\n",
			g_scn, phase, u(s.queued), u(s.copied), u(s.results), s.oldestAgeMs > 0 ? 1 : 0, s.stuckMs > 0 ? 1 : 0,
			s.workerRunning ? 1 : 0, u(s.pushed), u(s.ok), u(s.err), u(s.retry), u(s.reconnectSeen),
			u(s.errnoCount[SQL_ERRNO_2006]), u(s.errnoCount[SQL_ERRNO_2013]), u(s.errnoCount[SQL_ERRNO_2014]),
			u(s.errnoCount[SQL_ERRNO_1205]), u(s.errnoCount[SQL_ERRNO_1213]), u(s.errnoCount[SQL_ERRNO_OTHER]),
			s.execCount <= s.ok + s.err ? 1 : 0, u(s.unexecutedAtQuit), s.queued == sql.CountQuery() ? 1 : 0, s.copied == sql.CopySize() ? 1 : 0,
			buckets == s.err + s.retry ? 1 : 0);
		fflush(stdout);
	}

	void report(const char* phase, Probe& sql, const std::string& extra)
	{
		printf("%s phase=%s q=%u cq=%zu finished=%d %s %s\n", g_scn, phase, (unsigned) sql.CountQuery(), sql.CopySize(),
			sql.CountQueryFinished(), extra.c_str(), errnos().c_str());
		fflush(stdout);
		stats(phase, sql);
	}

	std::string kv(const char* k, long long v)
	{
		return std::string(k) + "=" + std::to_string(v) + " ";
	}

	void push_marker(Probe& sql, const std::string& tag)
	{
		sql.AsyncQuery(("INSERT INTO rt.m (tag) VALUES ('" + tag + "')").c_str());
	}

	// --- scenarios ---------------------------------------------------------------------------------------------------

	// S1: one-query batch, error in the retry list that goes away; then a new query arrives
	void S1()
	{
		Probe sql;
		start(sql);
		sql.AsyncQuery(SETPW);
		ms(1500);
		report("pushed", sql, kv("applied", ghost_password_set()));
		must(g_admin, "CREATE USER ghost@localhost"); // the statement would now succeed
		ms(3000);
		report("error_gone_no_new_query", sql, kv("applied", ghost_password_set()));
		push_marker(sql, "s1-after");
		ms(1500);
		report("new_query", sql, kv("applied", ghost_password_set()) + kv("marker", marker("s1-after")));
		sql.Quit();
	}

	// S2: multi-query batch (worker kept busy so five queries land in one batch), same error, then it goes away.
	// DO SLEEP returns no result set: a SELECT here poisoned the connection (run 1, see S11).
	void S2()
	{
		Probe sql;
		start(sql);
		sql.AsyncQuery("DO SLEEP(1)");
		ms(100);
		sql.AsyncQuery(SETPW);
		for (int i = 1; i <= 4; ++i)
			push_marker(sql, "s2-m" + std::to_string(i));
		ms(2500);
		report("batch_of_5", sql, kv("applied", ghost_password_set()) + kv("markers", markers("s2-m")));
		must(g_admin, "CREATE USER ghost@localhost");
		ms(3000);
		report("error_gone_no_new_query", sql, kv("applied", ghost_password_set()) + kv("markers", markers("s2-m")));
		push_marker(sql, "s2-m5");
		ms(1500);
		report("new_query", sql, kv("applied", ghost_password_set()) + kv("markers", markers("s2-m")));
		sql.Quit();
	}

	// S3: error in the retry list that never goes away; new queries keep arriving
	void S3()
	{
		Probe sql;
		start(sql);
		sql.AsyncQuery(SETPW);
		ms(1500);
		report("pushed", sql, kv("markers", markers("s3-")));
		for (int i = 1; i <= 3; ++i)
		{
			push_marker(sql, "s3-" + std::to_string(i));
			ms(1500);
			report(("new_query_" + std::to_string(i)).c_str(), sql, kv("markers", markers("s3-")));
		}
		sql.Quit();
	}

	// S4: permanent error NOT in the retry list (syntax error), followed by a marker
	void S4()
	{
		Probe sql;
		start(sql);
		sql.AsyncQuery("SELEC 1");
		push_marker(sql, "s4-after");
		ms(1500);
		report("pushed", sql, kv("marker", marker("s4-after")));
		sql.Quit();
	}

	// S5: 1205 lock wait timeout (another session holds the row)
	void S5()
	{
		MYSQL* other = connect_admin();
		must(other, "BEGIN");
		must(other, "SELECT * FROM rt.d WHERE id = 1 FOR UPDATE");
		Probe sql;
		start(sql);
		sql.AsyncQuery("SET SESSION innodb_lock_wait_timeout = 1");
		sql.AsyncQuery("UPDATE rt.d SET v = 5 WHERE id = 1");
		push_marker(sql, "s5-after");
		ms(3000);
		report("lock_held", sql, kv("v1", scalar(g_admin, "SELECT v FROM rt.d WHERE id = 1")) + kv("marker", marker("s5-after")));
		must(other, "ROLLBACK");
		ms(1500);
		report("lock_released", sql, kv("v1", scalar(g_admin, "SELECT v FROM rt.d WHERE id = 1")));
		sql.Quit();
		mysql_close(other);
	}

	// S6: 1213 deadlock on an autocommit AsyncSQL statement. The other transaction holds row 2 and is made heavier, the
	// AsyncSQL statement locks row 1 then waits for row 2, the other then asks for row 1. Which side failed, and whether
	// InnoDB really saw a deadlock (Innodb_deadlocks), are recorded, not assumed.
	void S6()
	{
		const long long dl0 = status("Innodb_deadlocks");
		MYSQL* other = connect_admin();
		must(other, "BEGIN");
		make_heavy(other);
		must(other, "UPDATE rt.d SET v = 1 WHERE id = 2");
		Probe sql;
		start(sql);
		sql.AsyncQuery("UPDATE rt.d SET v = 7 WHERE id IN (1, 2)");
		ms(700);
		const unsigned otherErr = sqlrun(other, "UPDATE rt.d SET v = 1 WHERE id = 1");
		ms(800);
		push_marker(sql, "s6-after");
		ms(1500);
		report("deadlock", sql, kv("deadlocks", status("Innodb_deadlocks") - dl0) + kv("other_errno", otherErr)
			+ kv("marker", marker("s6-after")));
		must(other, "ROLLBACK");
		ms(500);
		report("after_rollback", sql, kv("v1", scalar(g_admin, "SELECT v FROM rt.d WHERE id = 1"))
			+ kv("v2", scalar(g_admin, "SELECT v FROM rt.d WHERE id = 2")));
		sql.Quit();
		mysql_close(other);
	}

	// S7: connection killed while a write is running
	void S7()
	{
		Probe sql;
		start(sql);
		sql.AsyncQuery("INSERT INTO rt.m (tag) SELECT 's7-slow' FROM DUAL WHERE SLEEP(3) = 0");
		ms(800);
		const long long cid = scalar(g_admin, "SELECT id FROM information_schema.processlist WHERE user = 'rt' "
			"AND info LIKE 'INSERT INTO rt.m%'");
		const unsigned killErr = sqlrun(g_admin, "KILL CONNECTION " + std::to_string(cid));
		ms(1500);
		report("killed_during_query", sql, kv("kill_errno", killErr) + kv("slow", marker("s7-slow")));
		push_marker(sql, "s7-after");
		ms(1500);
		report("new_query", sql, kv("slow", marker("s7-slow")) + kv("marker", marker("s7-after")));
		sql.Quit();
	}

	// S8: connection killed while idle, then a write
	void S8()
	{
		Probe sql;
		start(sql);
		push_marker(sql, "s8-before");
		ms(1000);
		const long long cid = scalar(g_admin, "SELECT id FROM information_schema.processlist WHERE user = 'rt'");
		const unsigned killErr = sqlrun(g_admin, "KILL CONNECTION " + std::to_string(cid));
		ms(500);
		push_marker(sql, "s8-after");
		ms(1500);
		report("killed_idle", sql, kv("kill_errno", killErr) + kv("before", marker("s8-before"))
			+ kv("after", marker("s8-after")));
		push_marker(sql, "s8-after2");
		ms(1500);
		report("next_query", sql, kv("after", marker("s8-after")) + kv("after2", marker("s8-after2")));
		sql.Quit();
	}

	// S9: shutdown (Quit) while a query is stuck at the head of the copy queue with work behind it
	void S9()
	{
		Probe sql;
		start(sql);
		sql.AsyncQuery(SETPW);
		ms(1500);
		for (int i = 1; i <= 3; ++i)
			push_marker(sql, "s9-" + std::to_string(i));
		ms(2500);
		report("before_quit", sql, kv("markers", markers("s9-")));
		sql.Quit();
		ms(1500);
		printf("%s phase=after_quit markers=%lld %s\n", g_scn, markers("s9-"), errnos().c_str());
		stats("after_quit", sql);
	}

	// S9b: shutdown with a normal backlog (no errors)
	void S9b()
	{
		Probe sql;
		start(sql);
		for (int i = 1; i <= 200; ++i)
			push_marker(sql, "s9b-" + std::to_string(i));
		sql.Quit();
		printf("%s phase=after_quit markers=%lld %s\n", g_scn, markers("s9b-"), errnos().c_str());
		stats("after_quit", sql);
	}

	// S12: a ReturnQuery fails with an error in the retry list, then succeeds on a later attempt. What does the caller
	// see in the result? (db reads uiSQLErrno for QID_LOGIN_BY_KEY, db/ClientManagerLogin.cpp)
	void S12()
	{
		Probe sql;
		start(sql);
		sql.ReturnQuery(SETPW, nullptr);
		ms(1500);
		report("failing", sql, kv("applied", ghost_password_set()));
		must(g_admin, "CREATE USER ghost@localhost");
		push_marker(sql, "s12-after"); // wakes the worker: the stuck statement is attempted again and now succeeds
		ms(1500);
		report("retried", sql, kv("applied", ghost_password_set()) + kv("marker", marker("s12-after")));
		std::unique_ptr<SQLMsg> res;
		const bool got = sql.PopResult(res);
		printf("%s phase=result popped=%d uiSQLErrno=%u uiFinalErrno=%u applied=%lld\n", g_scn, got ? 1 : 0,
			got ? res->uiSQLErrno : 0, got ? res->uiFinalErrno : 0, ghost_password_set());
		stats("result_popped", sql);
		sql.Quit();
	}

	// S11: a result-returning statement sent through AsyncQuery (no Store()), then writes. Found in run 1 (S2 with
	// SELECT SLEEP). No production call site does this today (grep: game/db async paths send no SELECT/SHOW/CALL).
	void S11()
	{
		Probe sql;
		start(sql);
		sql.AsyncQuery("SELECT 1");
		ms(500);
		for (int i = 1; i <= 3; ++i)
		{
			push_marker(sql, "s11-" + std::to_string(i));
			ms(500);
		}
		ms(1000);
		report("after_select", sql, kv("markers", markers("s11-")));
		ms(3000);
		push_marker(sql, "s11-late");
		ms(1500);
		report("later", sql, kv("markers", markers("s11-")));
		sql.Quit();
	}

	// S10: MariaDB itself (no AsyncSQL): what does 1205 vs 1213 roll back inside an explicit transaction?
	void S10()
	{
		{ // 1205
			MYSQL* a = connect_admin();
			MYSQL* b = connect_admin();
			must(b, "BEGIN");
			must(b, "SELECT * FROM rt.d WHERE id = 1 FOR UPDATE");
			must(a, "SET SESSION innodb_lock_wait_timeout = 1");
			must(a, "BEGIN");
			must(a, "INSERT INTO rt.m (tag) VALUES ('s10-1205-first')");
			const unsigned e = sqlrun(a, "UPDATE rt.d SET v = 5 WHERE id = 1");
			const long long inTxn = scalar(a, "SELECT @@in_transaction");
			must(a, "COMMIT");
			must(b, "ROLLBACK");
			printf("%s case=1205 errno=%u in_transaction_after=%lld first_statement_kept=%lld\n", g_scn, e, inTxn,
				marker("s10-1205-first"));
			mysql_close(a);
			mysql_close(b);
		}
		{ // 1213: b heavier, a is the victim
			const long long dl0 = status("Innodb_deadlocks");
			MYSQL* a = connect_admin();
			MYSQL* b = connect_admin();
			must(a, "BEGIN");
			must(a, "INSERT INTO rt.m (tag) VALUES ('s10-1213-first')");
			must(a, "UPDATE rt.d SET v = 9 WHERE id = 2");
			must(b, "BEGIN");
			make_heavy(b);
			must(b, "UPDATE rt.d SET v = 1 WHERE id = 1");
			unsigned ea = 0;
			std::thread t([&] { ea = sqlrun(a, "UPDATE rt.d SET v = 9 WHERE id = 1"); });
			ms(500);
			const unsigned eb = sqlrun(b, "UPDATE rt.d SET v = 1 WHERE id = 2");
			t.join();
			const long long inTxn = scalar(a, "SELECT @@in_transaction");
			sqlrun(a, "COMMIT");
			must(b, "ROLLBACK");
			printf("%s case=1213 deadlocks=%lld errno_a=%u errno_b=%u in_transaction_after=%lld first_statement_kept=%lld\n",
				g_scn, status("Innodb_deadlocks") - dl0, ea, eb, inTxn, marker("s10-1213-first"));
			mysql_close(a);
			mysql_close(b);
		}
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <S1..S12|S9b|versions>\n", argv[0]);
		return 2;
	}
	g_scn = argv[1];
	g_admin = connect_admin();
	guard();

	if (!strcmp(g_scn, "versions"))
	{
		printf("server=%s client=%s\n", sscalar(g_admin, "SELECT VERSION()").c_str(), mysql_get_client_info());
		return 0;
	}

	log_init();
	setup_schema();

	if (!strcmp(g_scn, "S1")) S1();
	else if (!strcmp(g_scn, "S2")) S2();
	else if (!strcmp(g_scn, "S3")) S3();
	else if (!strcmp(g_scn, "S4")) S4();
	else if (!strcmp(g_scn, "S5")) S5();
	else if (!strcmp(g_scn, "S6")) S6();
	else if (!strcmp(g_scn, "S7")) S7();
	else if (!strcmp(g_scn, "S8")) S8();
	else if (!strcmp(g_scn, "S9")) S9();
	else if (!strcmp(g_scn, "S9b")) S9b();
	else if (!strcmp(g_scn, "S10")) S10();
	else if (!strcmp(g_scn, "S11")) S11();
	else if (!strcmp(g_scn, "S12")) S12();
	else
		die("unknown scenario", nullptr);

	ms(1200); // let spdlog flush syserr.log before the process ends
	mysql_close(g_admin);
	return 0;
}
