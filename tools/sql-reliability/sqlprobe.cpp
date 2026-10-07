// TEST/PROBE TOOL ONLY (no runtime code).
// Analysis probes for the AsyncSQL fix design (DB step 2, docs/engineering/db-step2-asyncsql-fix.md). Unlike sqlrt
// (the frozen baseline) these answer design questions with the REAL table definitions (server/sql/player.sql: item and
// quest InnoDB, player and guild_member Aria) and the real statement shapes of db/Cache.cpp, db/ClientManager*.cpp.
// Runs only against the temporary MariaDB started by probe.sh; same fail-closed guard as sqlrt.
//
//   sqlprobe <R1|R2|R3|R4|R5|G1|G2|K7a|K7b|K7c|K7d|P0|P1|P1b|P1c|P2|P3|P4|P5|C1|C2|C3|C4|H1a|H1b|H1c|F1|L1|Q1|O1>
//   env (probe.sh): RT_PORT, RT_PW, RT_SOCK, RT_TOKEN, RT_START (shell command that starts the temporary server)
//   Built against the libsql before or after DB step 2a; probe.sh defines HAVE_2A for the 2a library, which adds the
//   2a-only scenarios F1 (family labels), L1 (no raw SQL in any log), Q1 (shutdown with the server down), O1 (order
//   under an outage). Log helpers count the old and the new syserr wording, so one tool compares before and after.
#include "libsql/AsyncSQL.h"
#ifdef HAVE_2A
#include "libsql/SQLFamily.h"
#include "libsql/SQLRead.h"
#include "common/sql_failure_ledger.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <glob.h>
// game/db define this in their version.cpp; the probe has no build identity
const char* M2BuildFields() { return " build=probe build_dirty=0 build_src=probe"; }
#endif

#include <sys/resource.h>
#include <sys/socket.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
	const char* g_scn = "";
	MYSQL* g_admin = nullptr;
	using clk = std::chrono::steady_clock;

	[[noreturn]] void die(const std::string& what, MYSQL* h)
	{
		printf("%s FATAL %s: %s\n", g_scn, what.c_str(), h ? mysql_error(h) : "");
		fflush(stdout);
		_exit(3);
	}

	MYSQL* connect_admin(bool must_succeed = true)
	{
		MYSQL* h = mysql_init(nullptr);
		if (!mysql_real_connect(h, nullptr, "root", nullptr, nullptr, 0, getenv("RT_SOCK"), 0))
		{
			if (must_succeed)
				die("admin connect", h);
			mysql_close(h);
			return nullptr;
		}
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

	long long scalar(MYSQL* h, const std::string& q)
	{
		if (mysql_real_query(h, q.c_str(), q.size()))
			die(q, h);
		long long v = -1;
		if (MYSQL_RES* r = mysql_store_result(h))
		{
			MYSQL_ROW row = mysql_fetch_row(r);
			if (row && row[0])
				v = atoll(row[0]);
			mysql_free_result(r);
		}
		return v;
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

	void ms(int n) { std::this_thread::sleep_for(std::chrono::milliseconds(n)); }

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

	// syslog.log: the 2a library writes "AsyncSQL: reconnected" there (the old one wrote "was reconnected" to syserr)
	long long syslog_lines(const char* needle)
	{
		std::ifstream in("syslog.log");
		std::string line;
		long long n = 0;
		while (std::getline(in, line))
			if (line.find(needle) != std::string::npos)
				++n;
		return n;
	}

	// syserr lines per errno, old wording "errno: N)" and 2a wording "errno=N "
	std::string errnos()
	{
		std::string s;
		for (int e : { 1205, 1213, 1927, 1053, 2002, 2006, 2013, 2014 })
		{
			const long long n = syserr(("errno: " + std::to_string(e) + ")").c_str()) + syserr(("errno=" + std::to_string(e) + " ").c_str());
			if (n)
				s += "e" + std::to_string(e) + "=" + std::to_string(n) + " ";
		}
		return s + "retrying=" + std::to_string(syserr("AsyncSQL: retrying") + syserr("attempt failed, retrying"))
			+ " reconnected=" + std::to_string(syserr("was reconnected") + syslog_lines("AsyncSQL: reconnected"))
			+ " locale_fail=" + std::to_string(syserr("cannot set locale"));
	}

	double cpu_s()
	{
		rusage u{};
		getrusage(RUSAGE_SELF, &u);
		return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
	}

	// Same guard as sqlrt: temporary datadir + per-run token on this server
	void guard()
	{
		const char* token = getenv("RT_TOKEN");
		if (!token || strlen(token) < 32)
			die("guard: RT_TOKEN missing; run through tools/sql-reliability/probe.sh", nullptr);
		if (sscalar(g_admin, "SELECT @@datadir").rfind("/var/tmp/m2sqlprobe/", 0) != 0)
			die("guard: refusing to run against datadir " + sscalar(g_admin, "SELECT @@datadir"), nullptr);
		if (sscalar(g_admin, "SELECT token FROM m2sqlrt_guard.token") != token)
			die("guard: run token does not match", nullptr);
	}

	void server_stop()
	{
		mysql_close(g_admin);
		g_admin = nullptr;
		if (system(("mariadb-admin --socket=" + std::string(getenv("RT_SOCK")) + " shutdown > /dev/null 2>&1").c_str()) != 0)
			die("server stop", nullptr);
		for (int i = 0; i < 100; ++i)
		{
			if (MYSQL* h = connect_admin(false)) { mysql_close(h); ms(100); continue; }
			return;
		}
		die("server did not stop", nullptr);
	}

	void server_start()
	{
		if (system(getenv("RT_START")) != 0)
			die("server start", nullptr);
		for (int i = 0; i < 200; ++i)
		{
			if ((g_admin = connect_admin(false)))
			{
				guard();
				return;
			}
			ms(100);
		}
		die("server did not start", nullptr);
	}

	struct Probe : public CAsyncSQL
	{
	};

	void start(Probe& sql, bool threaded)
	{
		if (!sql.Setup("127.0.0.1", "rt", getenv("RT_PW"), "rt", "latin1", !threaded, atoi(getenv("RT_PORT"))))
			die("asyncsql setup", nullptr);
		for (int i = 0; i < 100 && !sql.IsConnected(); ++i)
			ms(50);
		if (!sql.IsConnected())
			die("asyncsql connect", nullptr);
		if (scalar(g_admin, "SELECT COUNT(*) FROM information_schema.processlist WHERE user = 'rt'") < 1)
			die("guard: AsyncSQL connection is not on the temporary server", nullptr);
	}

	void start_on(Probe& sql, bool threaded, const char* db)
	{
		if (!sql.Setup("127.0.0.1", "rt", getenv("RT_PW"), db, "latin1", !threaded, atoi(getenv("RT_PORT"))))
			die("asyncsql setup", nullptr);
		for (int i = 0; i < 100 && !sql.IsConnected(); ++i)
			ms(50);
		if (!sql.IsConnected())
			die("asyncsql connect", nullptr);
	}

	long long marker(const std::string& tag) { return scalar(g_admin, "SELECT COUNT(*) FROM rt.m WHERE tag='" + tag + "'"); }
	std::string mark(const std::string& tag) { return "INSERT INTO rt.m (tag) VALUES ('" + tag + "')"; }

	// item row as db/Cache.cpp writes it (no sockets/attrs: isSocket/isAttr false)
	std::string item_replace(int id, int owner, int count)
	{
		return "REPLACE INTO player.item (id, owner_id, window, pos, count, vnum) VALUES(" + std::to_string(id) + ", "
			+ std::to_string(owner) + ", 'INVENTORY', " + std::to_string(id % 90) + ", " + std::to_string(count) + ", 19)";
	}
	long long item_count(int id) { return scalar(g_admin, "SELECT IFNULL(MAX(count), -1) FROM player.item WHERE id=" + std::to_string(id)); }
	long long item_rows(int id) { return scalar(g_admin, "SELECT COUNT(*) FROM player.item WHERE id=" + std::to_string(id)); }

	void heavy(MYSQL* h)
	{
		must(h, "SET SESSION max_recursive_iterations = 5000");
		must(h, "INSERT INTO rt.h WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM s WHERE i < 2000) SELECT i, 0 FROM s");
	}

	// --- R: lock errors on the real statement families ---------------------------------------------------------

	// R1: db/Cache.cpp item save (REPLACE, InnoDB) waits for a row lock -> 1205. Is anything of it applied?
	void R1()
	{
		must(g_admin, item_replace(1001, 1, 1));
		MYSQL* other = connect_admin();
		must(other, "BEGIN");
		must(other, "SELECT * FROM player.item WHERE id = 1001 FOR UPDATE");
		MYSQL* b = connect_admin();
		must(b, "SET SESSION innodb_lock_wait_timeout = 1");
		const unsigned e = sqlrun(b, item_replace(1001, 1, 5));
		const long long autocommit = scalar(b, "SELECT @@autocommit"), inTxn = scalar(b, "SELECT @@in_transaction");
		const long long cFail = item_count(1001), rFail = item_rows(1001);
		must(other, "ROLLBACK");
		const unsigned e2 = sqlrun(b, item_replace(1001, 1, 5));
		printf("%s family=item_REPLACE engine=InnoDB errno=%u autocommit=%lld in_txn_after=%lld after_fail count=%lld rows=%lld"
			" | same statement again errno=%u count=%lld rows=%lld\n", g_scn, e, autocommit, inTxn, cFail, rFail, e2,
			item_count(1001), item_rows(1001));
		mysql_close(b);
		mysql_close(other);
	}

	// R2: player delete's multi-row item DELETE (db/ClientManagerPlayer.cpp:1153 shape) as the deadlock victim -> 1213
	void R2()
	{
		must(g_admin, item_replace(2001, 7, 1));
		must(g_admin, item_replace(2002, 7, 1));
		const long long dl0 = scalar(g_admin, "SELECT VARIABLE_VALUE FROM information_schema.GLOBAL_STATUS WHERE VARIABLE_NAME='Innodb_deadlocks'");
		MYSQL* other = connect_admin();
		must(other, "BEGIN");
		heavy(other);
		must(other, "UPDATE player.item SET count = 9 WHERE id = 2002");
		MYSQL* b = connect_admin();
		unsigned eb = 0;
		std::thread t([&] { eb = sqlrun(b, "DELETE FROM player.item WHERE owner_id=7 AND (window < 3 or window = 4)"); });
		ms(700);
		const unsigned eo = sqlrun(other, "UPDATE player.item SET count = 9 WHERE id = 2001");
		t.join();
		const long long r1 = item_rows(2001), r2 = item_rows(2002);
		must(other, "ROLLBACK");
		const unsigned e2 = sqlrun(b, "DELETE FROM player.item WHERE owner_id=7 AND (window < 3 or window = 4)");
		printf("%s family=item_DELETE_by_owner engine=InnoDB deadlocks=%lld errno_victim=%u errno_other=%u after_fail rows_2001=%lld"
			" rows_2002=%lld | same statement again errno=%u rows=%lld\n", g_scn,
			scalar(g_admin, "SELECT VARIABLE_VALUE FROM information_schema.GLOBAL_STATUS WHERE VARIABLE_NAME='Innodb_deadlocks'") - dl0,
			eb, eo, r1, r2, e2, item_rows(2001) + item_rows(2002));
		mysql_close(b);
		mysql_close(other);
	}

	// R3: db/ClientManagerGuild.cpp:112 REPLACE INTO quest (InnoDB) SELECT ... FROM guild_member (Aria), one target row locked
	void R3()
	{
		must(g_admin, "INSERT INTO player.guild_member (pid, guild_id, grade) VALUES (31, 3, 1), (32, 3, 1)");
		must(g_admin, "REPLACE INTO player.quest (dwPID, szName, szState, lValue) VALUES (32, 'guild_manage', 'withdraw_time', 1)");
		MYSQL* other = connect_admin();
		must(other, "BEGIN");
		must(other, "SELECT * FROM player.quest WHERE dwPID = 32 FOR UPDATE");
		MYSQL* b = connect_admin();
		must(b, "SET SESSION innodb_lock_wait_timeout = 1");
		const std::string q = "REPLACE INTO player.quest (dwPID, szName, szState, lValue) SELECT pid, 'guild_manage', 'withdraw_time', 777 "
			"FROM player.guild_member WHERE guild_id = 3";
		const unsigned e = sqlrun(b, q);
		const long long n777 = scalar(g_admin, "SELECT COUNT(*) FROM player.quest WHERE lValue = 777");
		must(other, "ROLLBACK");
		const unsigned e2 = sqlrun(b, q);
		printf("%s family=quest_REPLACE_SELECT_guild_member engine=InnoDB<-Aria errno=%u after_fail rows_with_new_value=%lld"
			" | same statement again errno=%u rows_with_new_value=%lld\n", g_scn, e, n777, e2,
			scalar(g_admin, "SELECT COUNT(*) FROM player.quest WHERE lValue = 777"));
		mysql_close(b);
		mysql_close(other);
	}

	// R4: player save (UPDATE player ... WHERE id, Aria) while another session holds the table (LOCK TABLES WRITE)
	void R4()
	{
		must(g_admin, "INSERT INTO player.player (id, account_id, name, level) VALUES (41, 1, 'probe41', 1)");
		MYSQL* other = connect_admin();
		must(other, "LOCK TABLES player.player WRITE");
		MYSQL* b = connect_admin();
		must(b, "SET SESSION lock_wait_timeout = 1");
		const auto t0 = clk::now();
		const unsigned e = sqlrun(b, "UPDATE player.player SET level = 55 WHERE id = 41");
		const long long waited = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
		// Read through the lock holder: any other session would itself wait for the table (default lock_wait_timeout 1 day)
		const long long lFail = scalar(other, "SELECT level FROM player.player WHERE id = 41");
		must(other, "UNLOCK TABLES");
		const unsigned e2 = sqlrun(b, "UPDATE player.player SET level = 55 WHERE id = 41");
		printf("%s family=player_UPDATE engine=Aria errno=%u waited_ms_ge_900=%d after_fail level=%lld | same statement again"
			" errno=%u level=%lld; server default lock_wait_timeout=%s\n", g_scn, e, waited >= 900 ? 1 : 0, lFail, e2,
			scalar(g_admin, "SELECT level FROM player.player WHERE id = 41"),
			sscalar(g_admin, "SELECT @@global.lock_wait_timeout").c_str());
		mysql_close(b);
		mysql_close(other);
	}

	// R5: db/ClientManager.cpp:3587 ChargeCash, a RELATIVE update (cash = cash + n) on account (InnoDB) -> 1205
	void R5()
	{
		must(g_admin, "INSERT INTO account.account (id, login, cash) VALUES (61, 'probe61', 100)");
		MYSQL* other = connect_admin();
		must(other, "BEGIN");
		must(other, "SELECT * FROM account.account WHERE id = 61 FOR UPDATE");
		MYSQL* b = connect_admin();
		must(b, "SET SESSION innodb_lock_wait_timeout = 1");
		const std::string q = "update account.account set `cash` = `cash` + 50 where id = 61 limit 1";
		const unsigned e = sqlrun(b, q);
		const long long cFail = scalar(other, "SELECT cash FROM account.account WHERE id = 61");
		must(other, "ROLLBACK");
		const unsigned e2 = sqlrun(b, q);
		printf("%s family=account_cash_relative_UPDATE engine=InnoDB errno=%u after_fail cash=%lld (start 100) | same statement again"
			" errno=%u cash=%lld\n", g_scn, e, cFail, e2, scalar(g_admin, "SELECT cash FROM account.account WHERE id = 61"));
		mysql_close(b);
		mysql_close(other);
	}

	// --- G: which errnos does today's worker see when the server goes away? -------------------------------------

	// G1: server stopped while the connection is idle, a write is queued, server started again
	void G1()
	{
		Probe sql;
		start(sql, true);
		sql.AsyncQuery(mark("g1-before").c_str());
		ms(800);
		server_stop();
		sql.AsyncQuery(mark("g1-during").c_str());
		ms(2500);
		const std::string during = errnos();
		server_start();
		ms(1500);
		// Before any new statement: is the queued write applied on its own (2a), or stuck until something new arrives?
		const long long duringBeforeNew = marker("g1-during");
		sql.AsyncQuery(mark("g1-after").c_str()); // wakes a stuck head, if any
		ms(2500);
		printf("%s down: %s | after restart: during_applied_before_new_query=%lld before=%lld during=%lld after=%lld %s\n",
			g_scn, during.c_str(), duringBeforeNew, marker("g1-before"), marker("g1-during"), marker("g1-after"), errnos().c_str());
		sql.Quit();
	}

	// G2: server shut down while a write is executing (sent, not answered)
	void G2()
	{
		Probe sql;
		start(sql, true);
		sql.AsyncQuery("INSERT INTO rt.m (tag) SELECT 'g2-slow' FROM DUAL WHERE SLEEP(3) = 0");
		ms(800);
		server_stop();
		ms(1500);
		server_start();
		ms(500);
		sql.AsyncQuery(mark("g2-after").c_str());
		ms(2500);
		printf("%s slow_applied=%lld after=%lld %s\n", g_scn, marker("g2-slow"), marker("g2-after"), errnos().c_str());
		sql.Quit();
	}

	// --- K7: while (!QueryLocaleSet()); after a reconnect, with the server gone ------------------------------------

	// Kill the connection, let one statement reconnect silently (thread id changes, AsyncSQL only notices on the next
	// statement), stop the server, then send the next statement.
	void k7_prepare(Probe& sql, bool threaded, const char* tag)
	{
		auto run = [&](const std::string& q) { if (threaded) sql.AsyncQuery(q.c_str()); else sql.DirectQuery(q.c_str()); };
		run(mark(std::string(tag) + "-1"));
		ms(500);
		const long long cid = scalar(g_admin, "SELECT id FROM information_schema.processlist WHERE user = 'rt'");
		must(g_admin, "KILL CONNECTION " + std::to_string(cid));
		ms(300);
		run(mark(std::string(tag) + "-2")); // silent reconnect inside this statement (S8)
		ms(800);
	}

	void K7a() // worker thread
	{
		Probe sql;
		start(sql, true);
		k7_prepare(sql, true, "k7a");
		server_stop();
		sql.AsyncQuery(mark("k7a-3").c_str());
		const double c0 = cpu_s();
		ms(5000);
		const double busy = cpu_s() - c0;
		const long long loopLines = syserr("cannot set locale");
		server_start();
		ms(2500);
		printf("%s outage_s=5 process_cpu_s_during_outage=%.1f locale_fail_lines_after_5s=%s marker3_applied_after_restart=%lld %s\n",
			g_scn, busy, loopLines > 1000 ? ">1000" : std::to_string(loopLines).c_str(), marker("k7a-3"), errnos().c_str());
		sql.Quit();
	}

	void K7b() // DirectQuery: the calling thread (game main thread in game)
	{
		Probe sql;
		start(sql, false);
		k7_prepare(sql, false, "k7b");
		server_stop();
		std::thread restarter([] { ms(5000); server_start(); });
		const double c0 = cpu_s();
		const auto t0 = clk::now();
		sql.DirectQuery(mark("k7b-3").c_str());
		const long long blocked = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
		const double busy = cpu_s() - c0;
		restarter.join();
		ms(1500);
		printf("%s outage_s=5 caller_blocked_s=%.1f process_cpu_s=%.1f marker3_applied=%lld %s\n", g_scn, blocked / 1000.0, busy,
			marker("k7b-3"), errnos().c_str());
	}

	void K7c() // control: server stopped without a prior reconnect
	{
		Probe sql;
		start(sql, false);
		sql.DirectQuery(mark("k7c-1").c_str());
		server_stop();
		const auto t0 = clk::now();
		auto r = sql.DirectQuery(mark("k7c-2").c_str());
		const long long took = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
		const unsigned e = r->uiSQLErrno;
		server_start();
		ms(1500);
		printf("%s direct_errno=%u returned_within_1s=%d %s\n", g_scn, e, took < 1000 ? 1 : 0, errnos().c_str());
	}

	// --- H-1: three connections writing the same item row -------------------------------------------------------

	// main (ReturnQuery) saves item X after a slow statement; another connection deletes X meanwhile
	void h1(bool viaDirect, bool barrier)
	{
		Probe mainSql, asyncSql, directSql;
		start(mainSql, true);
		start(asyncSql, true);
		start(directSql, false);
		must(g_admin, item_replace(5001, 50, 1));
		mainSql.ReturnQuery("DO SLEEP(1)", nullptr);	// main busy (e.g. a backlog or a lock wait)
		mainSql.ReturnQuery(item_replace(5001, 50, 2).c_str(), nullptr);	// cache flush of X, queued before the delete
		if (barrier)
		{
			// db player delete: a ReturnQuery on main first (QID_PLAYER_DELETE), DELETEs only in its result handler
			mainSql.ReturnQuery("SELECT 1", nullptr);
			int popped = 0;
			for (int i = 0; i < 100 && popped < 3; ++i)
			{
				std::unique_ptr<SQLMsg> r;
				while (mainSql.PopResult(r))
					++popped;
				ms(50);
			}
		}
		const long long beforeDelete = item_rows(5001);
		if (viaDirect)
			directSql.DirectQuery("DELETE FROM player.item WHERE owner_id=50 AND (window < 3 or window = 4)");
		else
			asyncSql.AsyncQuery("DELETE FROM player.item WHERE id=5001");
		ms(300);
		const long long afterDelete = item_rows(5001);
		ms(2500); // main finishes its queue
		printf("%s delete_via=%s barrier=%d rows_before_delete=%lld rows_right_after_delete=%lld rows_at_end=%lld => %s\n", g_scn,
			viaDirect ? "direct(owner)" : "async(id)", barrier ? 1 : 0, beforeDelete, afterDelete, item_rows(5001),
			item_rows(5001) ? "DELETED ITEM CAME BACK" : "stays deleted");
		mainSql.Quit();
		asyncSql.Quit();
	}
	// K7d: does Connector/C's own reconnect restore the client character set? (mariadb_reconnect calls
	// mysql_set_character_set with the old charset.) Plain MYSQL with AsyncSQL's options, no AsyncSQL code involved.
	void K7d()
	{
		MYSQL* h = mysql_init(nullptr);
		mysql_options(h, MYSQL_SET_CHARSET_NAME, "latin1");
		if (!mysql_real_connect(h, "127.0.0.1", "rt", getenv("RT_PW"), "rt", atoi(getenv("RT_PORT")), nullptr, CLIENT_MULTI_STATEMENTS))
			die("connect", h);
		my_bool reconnect = true;
		mysql_options(h, MYSQL_OPT_RECONNECT, &reconnect);
		const std::string def = sscalar(g_admin, "SELECT @@global.character_set_client");
		const std::string before = sscalar(h, "SELECT CONCAT(@@character_set_client, '/', @@character_set_connection, '/', @@character_set_results)");
		const unsigned long id1 = mysql_thread_id(h);
		must(g_admin, "KILL CONNECTION " + std::to_string(id1));
		ms(300);
		const std::string after = sscalar(h, "SELECT CONCAT(@@character_set_client, '/', @@character_set_connection, '/', @@character_set_results)");
		printf("%s server_default=%s before=%s after_silent_reconnect=%s thread_id_changed=%d\n", g_scn, def.c_str(), before.c_str(),
			after.c_str(), mysql_thread_id(h) != id1 ? 1 : 0);
		mysql_close(h);
	}

	// --- P: does the server execute a COM_QUERY whose packet did not arrive completely? ---------------------------
	// Raw bytes on an authenticated connection (mysql_get_socket): 3-byte length + sequence 0 + 0x03 COM_QUERY + SQL.
	// mode 0: whole packet, read the answer (control: the method works)
	// mode 1: header announces the whole packet, only half of the payload is sent, then the socket is shut down
	// mode 2: whole packet, socket shut down at once without reading the answer (what a client sees as 2013)
	void packet(int mode, const char* tag)
	{
		MYSQL* h = mysql_init(nullptr);
		if (!mysql_real_connect(h, "127.0.0.1", "rt", getenv("RT_PW"), "rt", atoi(getenv("RT_PORT")), nullptr, 0))
			die("raw connect", h);
		const std::string sql = "INSERT INTO rt.m (tag) SELECT '" + std::string(tag) + "' FROM DUAL WHERE SLEEP(" +
			(mode == 2 ? "1" : "0") + ") = 0";
		std::string pkt(4, '\0');
		const size_t len = sql.size() + 1;
		pkt[0] = char(len & 0xff); pkt[1] = char((len >> 8) & 0xff); pkt[2] = char((len >> 16) & 0xff); pkt[3] = 0;
		pkt += '\x03';
		pkt += sql;
		const int fd = mysql_get_socket(h);
		// mode 3: only 2 of the 4 header bytes; mode 4: everything but the last byte
		const size_t n = mode == 1 ? 4 + 1 + sql.size() / 2 : mode == 3 ? 2 : mode == 4 ? pkt.size() - 1 : pkt.size();
		if (send(fd, pkt.data(), n, 0) != (ssize_t) n)
			die("raw send", nullptr);
		if (mode == 0)
		{
			char buf[256];
			recv(fd, buf, sizeof(buf), 0);
		}
		shutdown(fd, SHUT_RDWR);
		mysql_close(h);
		ms(2500);
		printf("%s mode=%s bytes_sent=%zu_of_%zu applied=%lld\n", g_scn,
			mode == 0 ? "whole_packet_answer_read" : mode == 1 ? "half_packet_then_disconnect" : mode == 2 ? "whole_packet_disconnect_before_answer"
			: mode == 3 ? "half_header_then_disconnect" : "all_but_last_byte_then_disconnect",
			n, pkt.size(), marker(tag));
	}
	void P0() { packet(0, "p0"); }
	void P1() { packet(1, "p1"); }
	void P2() { packet(2, "p2"); }
	void P1b() { packet(3, "p1b"); }
	void P1c() { packet(4, "p1c"); }

	// --- Phase: mysql_real_query returns -1 when sending failed (ma_simple_command, mariadb_lib.c:3005) and 1 when
	// reading the answer failed (:3007). Plain MYSQL with AsyncSQL's options; which phase and errno does each failure give?
	MYSQL* raw_client()
	{
		MYSQL* h = mysql_init(nullptr);
		mysql_options(h, MYSQL_SET_CHARSET_NAME, "latin1");
		if (!mysql_real_connect(h, "127.0.0.1", "rt", getenv("RT_PW"), "rt", atoi(getenv("RT_PORT")), nullptr, CLIENT_MULTI_STATEMENTS))
			die("connect", h);
		my_bool reconnect = true;
		mysql_options(h, MYSQL_OPT_RECONNECT, &reconnect);
		return h;
	}

	void P3() // server gone while idle: the statement cannot be sent
	{
		MYSQL* h = raw_client();
		server_stop();
		const std::string q = mark("p3");
		const int rc = mysql_real_query(h, q.c_str(), q.size());
		const unsigned e = mysql_errno(h);
		server_start();
		ms(500);
		printf("%s case=server_down_before_send rc=%d errno=%u applied=%lld\n", g_scn, rc, e, marker("p3"));
		mysql_close(h);
	}

	void P4() // connection killed while the statement executes: sent, answer lost
	{
		MYSQL* h = raw_client();
		const std::string q = "INSERT INTO rt.m (tag) SELECT 'p4' FROM DUAL WHERE SLEEP(2) = 0";
		const unsigned long id = mysql_thread_id(h);
		std::thread killer([id] { ms(700); MYSQL* a = connect_admin(); sqlrun(a, "KILL CONNECTION " + std::to_string(id)); mysql_close(a); });
		const int rc = mysql_real_query(h, q.c_str(), q.size());
		const unsigned e = mysql_errno(h);
		killer.join();
		ms(2500);
		printf("%s case=killed_during_execution rc=%d errno=%u applied=%lld\n", g_scn, rc, e, marker("p4"));
		mysql_close(h);
	}

	void P5() // server shut down while the statement executes
	{
		MYSQL* h = raw_client();
		const std::string q = "INSERT INTO rt.m (tag) SELECT 'p5' FROM DUAL WHERE SLEEP(3) = 0";
		std::thread stopper([] { ms(700); server_stop(); });
		const int rc = mysql_real_query(h, q.c_str(), q.size());
		const unsigned e = mysql_errno(h);
		stopper.join();
		server_start();
		ms(500);
		printf("%s case=server_shutdown_during_execution rc=%d errno=%u applied=%lld\n", g_scn, rc, e, marker("p5"));
		mysql_close(h);
	}

	// --- C: connection/config failures at reconnect: which phase and errno, and what does today's worker do? ----
	// The connection is killed, the condition is set, the next statement has to reconnect.
	//   C1 access denied (password changed)   C2 unknown database (dropped)
	//   C3 too many connections (server-wide)  C4 per-user connection limit
	// C3/C4 use rtl, a user without global privileges (rt has GRANT ALL, and privileged users get an extra slot).
	// phase/errno/result/policy of the first "AsyncSQL: failed" syserr line (2a format; empty for the old library)
	std::string failure_fields()
	{
		std::ifstream in("syserr.log");
		std::string line;
		while (std::getline(in, line))
		{
			if (line.find("AsyncSQL: failed") == std::string::npos)
				continue;
			std::string out;
			for (const char* key : { "phase=", "errno=", "result=", "policy=" })
			{
				const size_t p = line.find(std::string(" ") + key);
				if (p != std::string::npos)
					out += " " + line.substr(p + 1, line.find(' ', p + 1) - p - 1);
			}
			return " | first_failure:" + out;
		}
		return "";
	}

	void conn_case(int which)
	{
		const bool limited = which >= 3;
		const std::string user = limited ? "rtl" : "rt";
		const char* dbName = which == 2 ? "rtc" : "rt";
		if (limited)
		{
			must(g_admin, std::string("CREATE USER IF NOT EXISTS rtl@'127.0.0.1' IDENTIFIED BY '") + getenv("RT_PW") + "'");
			must(g_admin, "GRANT ALL ON rt.* TO rtl@'127.0.0.1'");
		}
		if (which == 2)
			must(g_admin, "CREATE DATABASE IF NOT EXISTS rtc");

		MYSQL* holder = nullptr; // C4: keeps the user's only allowed connection busy during the reconnect
		std::vector<MYSQL*> fill; // C3: rtl connections filling every non-privileged slot (max_connections minimum is 10)
		auto setCond = [&](bool on) {
			if (which == 1)
				must(g_admin, on ? "ALTER USER rt@'127.0.0.1' IDENTIFIED BY 'changed-by-probe'"
					: std::string("ALTER USER rt@'127.0.0.1' IDENTIFIED BY '") + getenv("RT_PW") + "'");
			else if (which == 2)
				must(g_admin, on ? "DROP DATABASE rtc" : "CREATE DATABASE IF NOT EXISTS rtc");
			else if (which == 3)
			{
				if (on)
				{
					must(g_admin, "SET GLOBAL max_connections = 10");
					for (int i = 0; i < 30; ++i)
					{
						MYSQL* f = mysql_init(nullptr);
						if (!mysql_real_connect(f, "127.0.0.1", "rtl", getenv("RT_PW"), "rt", atoi(getenv("RT_PORT")), nullptr, 0))
						{
							mysql_close(f);
							break; // server-wide limit reached
						}
						fill.push_back(f);
					}
				}
				else
				{
					for (MYSQL* f : fill)
						mysql_close(f);
					fill.clear();
					must(g_admin, "SET GLOBAL max_connections = 151");
				}
			}
			else if (on)
			{
				must(g_admin, "ALTER USER rtl@'127.0.0.1' WITH MAX_USER_CONNECTIONS 1");
				holder = mysql_init(nullptr);
				if (!mysql_real_connect(holder, "127.0.0.1", "rtl", getenv("RT_PW"), "rt", atoi(getenv("RT_PORT")), nullptr, 0))
					die("holder connect", holder);
			}
			else
			{
				if (holder) { mysql_close(holder); holder = nullptr; }
				must(g_admin, "ALTER USER rtl@'127.0.0.1' WITH MAX_USER_CONNECTIONS 0");
			}
		};
		auto killUser = [&] {
			must(g_admin, "KILL CONNECTION " + std::to_string(scalar(g_admin,
				"SELECT MIN(id) FROM information_schema.processlist WHERE user = '" + user + "'")));
			ms(300);
		};

		// raw client: phase and errno
		MYSQL* h = mysql_init(nullptr);
		mysql_options(h, MYSQL_SET_CHARSET_NAME, "latin1");
		if (!mysql_real_connect(h, "127.0.0.1", user.c_str(), getenv("RT_PW"), dbName, atoi(getenv("RT_PORT")), nullptr, CLIENT_MULTI_STATEMENTS))
			die("connect", h);
		my_bool reconnect = true;
		mysql_options(h, MYSQL_OPT_RECONNECT, &reconnect);
		killUser();
		setCond(true);
		const int rc = mysql_real_query(h, "DO 1", 4);
		const unsigned e = mysql_errno(h);
		setCond(false);
		mysql_close(h);

		// today's AsyncSQL worker: does it loop on this failure?
		Probe sql;
		if (!sql.Setup("127.0.0.1", user.c_str(), getenv("RT_PW"), dbName, "latin1", false, atoi(getenv("RT_PORT"))))
			die("asyncsql setup", nullptr);
		for (int i = 0; i < 100 && !sql.IsConnected(); ++i)
			ms(50);
		sql.AsyncQuery("DO 1");
		ms(500);
		killUser();
		setCond(true);
		sql.AsyncQuery("DO 2");
		ms(3000);
		const long long fails = syserr("query failed") + syserr("AsyncSQL: failed"),
			retries = syserr("AsyncSQL: retrying") + syserr("attempt failed, retrying");
		setCond(false);
		sql.Quit();
		static const char* names[] = { "", "access_denied", "unknown_database", "too_many_connections", "user_connection_limit" };
		printf("%s case=%s raw rc=%d errno=%u | asyncsql_today: failures_logged_in_3s=%lld retrying_lines=%lld%s\n", g_scn,
			names[which], rc, e, fails, retries, failure_fields().c_str());
	}
	void C1() { conn_case(1); }
	void C2() { conn_case(2); }
	void C3() { conn_case(3); }
	void C4() { conn_case(4); }

#ifdef HAVE_2A
	// --- F1: family labels never carry a value (adversarial inputs) --------------------------------------------
	void F1()
	{
		struct Case { const char* sql; const char* expect; };
		const Case cases[] = {
			{ "SELECT 'S3CR3T',password,social_id FROM account WHERE login='S3CR3T'", "select.account" },
			{ "UPDATE safebox SET password='S3CR3T' WHERE account_id=7", "update.safebox" },
			{ "REPLACE INTO item (id, owner_id) VALUES(1, 2)", "replace.item" },
			{ "INSERT DELAYED INTO log (ip) VALUES('S3CR3T')", "insert.log" },
			{ "DELETE FROM player.item WHERE owner_id=1", "delete.player.item" },
			{ "SELECT 'x FROM S3CR3T' FROM account", "select.account" },
			{ "SELECT 'it\\'s FROM S3CR3T' FROM account", "select.account" },
			{ "SELECT 'a''b FROM S3CR3T' FROM account", "select.account" },
			{ "SELECT \"x FROM S3CR3T\" FROM account", "select.account" },
			{ "SELECT 'unterminated FROM S3CR3T", "select" },
			{ "INSERT INTO item_award (login, vnum)select 'S3CR3T', 1 from DUAL where not exists (select login from item_award) ;", "insert.item_award" },
			{ "update account set `cash` = `cash` + 50 where id = 1 limit 1", "update.account" },
			{ "SET @i = (SELECT MAX(id) FROM loginlog2 WHERE account_id=1)", "set" },
			{ "S3CR3T", "unknown" },
			{ "/* S3CR3T */ SELECT 1", "select" },
			{ "", "unknown" },
		};
		int ok = 0, leaks = 0, n = 0;
		for (const Case& c : cases)
		{
			char out[64];
			SQLFamily(c.sql, out, sizeof(out));
			++n;
			if (std::strstr(out, "S3CR3T") || std::strstr(out, "s3cr3t"))
				++leaks;
			if (std::strcmp(out, c.expect) == 0)
				++ok;
			else
				printf("%s mismatch: got=%s expected=%s\n", g_scn, out, c.expect);
		}
		printf("%s cases=%d as_expected=%d labels_with_secret=%d\n", g_scn, n, ok, leaks);
	}

	// --- L1: no raw SQL (or any value) in syserr, syslog or the failure ledger --------------------------------
	// Every path that wrote raw SQL before 2a is triggered with a statement carrying a marker value.
	long long file_count(const char* path, const char* needle)
	{
		std::ifstream in(path);
		std::string line;
		long long n = 0;
		while (std::getline(in, line))
			if (line.find(needle) != std::string::npos)
				++n;
		return n;
	}

	void L1()
	{
		const char* SECRET = "S3CR3T-L1";
		if (!sql_failure_ledger::Instance().Start("probe", (long) getpid(), 14, 4096))
			die("ledger start", nullptr);

		Probe worker, direct;
		start(worker, true);
		start(direct, false);
		worker.SetLabel("probe.rt.main");
		direct.SetLabel("probe.rt.direct");

		// worker failure (old :580), direct failure (old :298), slow statement (old :622), escape overflow (old :756)
		worker.AsyncQuery((std::string("SELEC '") + SECRET + "'").c_str());
		direct.DirectQuery((std::string("SELEC '") + SECRET + "'").c_str());
		worker.AsyncQuery((std::string("INSERT INTO rt.m (tag) SELECT '") + SECRET + "' FROM DUAL WHERE SLEEP(0.7) = 0").c_str());
		char small[8];
		worker.EscapeString(small, sizeof(small), SECRET, std::strlen(SECRET));
		ms(1500);

		// shutdown with pending statements while the server is down (old :679 and :704)
		server_stop();
		for (int i = 0; i < 3; ++i)
			worker.AsyncQuery((std::string("INSERT INTO rt.m (tag) VALUES ('") + SECRET + "')").c_str());
		ms(300);
		worker.Quit();
		server_start();
		ms(1500); // spdlog flushes every second; the ledger writes per line

		glob_t g{};
		long long ledgerLines = 0, ledgerSecret = 0;
		int ledgerMode = -1;
		if (glob("log/sql_failures_*.log", 0, nullptr, &g) == 0)
		{
			for (size_t i = 0; i < g.gl_pathc; ++i)
			{
				ledgerLines += file_count(g.gl_pathv[i], "src=sql_failure");
				ledgerSecret += file_count(g.gl_pathv[i], SECRET);
				struct stat st{};
				if (stat(g.gl_pathv[i], &st) == 0)
					ledgerMode = st.st_mode & 0777;
			}
			globfree(&g);
		}
		printf("%s secret_in_syserr=%lld secret_in_syslog=%lld secret_in_ledger=%lld ledger_lines=%lld ledger_mode=%o"
			" metadata_lines_in_syserr=%lld\n", g_scn, file_count("syserr.log", SECRET), file_count("syslog.log", SECRET),
			ledgerSecret, ledgerLines, ledgerMode, file_count("syserr.log", "family="));
	}

	// --- Q1: shutdown with the server down: nothing silent, everything counted ---------------------------------
	void Q1()
	{
		Probe sql;
		start(sql, true);
		sql.AsyncQuery(mark("q1-before").c_str());
		ms(800);
		server_stop();
		for (int i = 1; i <= 5; ++i)
			sql.AsyncQuery(mark("q1-" + std::to_string(i)).c_str());
		ms(500);
		const auto t0 = clk::now();
		sql.Quit();
		const long long quitMs = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
		server_start();
		SQLStats st;
		sql.CollectStats(st);
		ms(1200);
		printf("%s before=%lld queued_while_down=5 applied_after=%lld not_delivered=%llu unexecuted_at_quit=%llu pending=%u"
			" quit_returned_within_2s=%d drain_line=%lld\n", g_scn, marker("q1-before"),
			scalar(g_admin, "SELECT COUNT(*) FROM rt.m WHERE tag LIKE 'q1-_'"), (unsigned long long) st.resNotDelivered,
			(unsigned long long) st.resUnexecutedAtQuit, (unsigned) sql.CountPending(), quitMs < 2000 ? 1 : 0,
			syserr("quit role="));
	}

	// --- O1: order of writes to one row survives an outage (FIFO + in-place retry) ------------------------------
	void O1()
	{
		Probe mainSql;
		start(mainSql, true);
		must(g_admin, item_replace(7001, 70, 1));
		server_stop();
		mainSql.ReturnQuery(item_replace(7001, 70, 2).c_str(), nullptr);
		mainSql.ReturnQuery(item_replace(7001, 70, 3).c_str(), nullptr);
		mainSql.ReturnQuery("SELECT 1", nullptr); // barrier: its result comes after both writes
		ms(1000);
		server_start();
		std::vector<int> order;
		for (int i = 0; i < 100 && order.size() < 3; ++i)
		{
			std::unique_ptr<SQLMsg> r;
			while (mainSql.PopResult(r))
				order.push_back(r->iID);
			ms(50);
		}
		const bool inOrder = order.size() == 3 && order[0] < order[1] && order[1] < order[2];
		printf("%s final_count=%lld (last write = 3) results=%zu results_in_order=%d\n", g_scn, item_count(7001), order.size(),
			inOrder ? 1 : 0);
		mainSql.Quit();
	}

	// --- G3: db/GuildManager.cpp GetAverageGuildMemberLevel / GetGuildMemberCount on a database failure ----------
	// Same statements (unqualified tables, as db sends them). "old" = the code before the fix, run in a child process
	// because it dereferences the row of a missing result; "new" = libsql/SQLRead.h, which the fixed functions call.
	static int OldShape(SQLMsg* msg) // copied from GuildManager.cpp before step 2a
	{
		MYSQL_ROW row;
		row = mysql_fetch_row(msg->Get()->pSQLResult);
		int n = 0;
		if (row[0] && row[0][0])
			n = (int) strtol(row[0], nullptr, 10);
		return n;
	}

	std::string old_shape_in_child(SQLMsg* msg)
	{
		fflush(stdout);
		const pid_t pid = fork();
		if (pid == 0)
		{
			OldShape(msg);
			_exit(0);
		}
		int status = 0;
		waitpid(pid, &status, 0);
		if (WIFSIGNALED(status))
			return std::string("killed_by_signal_") + std::to_string(WTERMSIG(status));
		return "exit_" + std::to_string(WEXITSTATUS(status));
	}

	void G3()
	{
		must(g_admin, "INSERT INTO player.player (id, account_id, name, level) VALUES (51, 1, 'g3a', 10), (52, 1, 'g3b', 20)");
		must(g_admin, "INSERT INTO player.guild_member (pid, guild_id, grade) VALUES (51, 7, 1), (52, 7, 1)");
		Probe sql;
		start_on(sql, false, "player");
		auto avgQ = [](int gid) { return "SELECT AVG(level) FROM guild_member, player AS p WHERE guild_id=" + std::to_string(gid) + " AND guild_member.pid=p.id"; };
		auto cntQ = [](int gid) { return "SELECT COUNT(*) FROM guild_member WHERE guild_id=" + std::to_string(gid); };

		std::string out;
		for (int gid : { 7, 8 }) // 8: no members (AVG is NULL)
		{
			auto a = sql.DirectQuery(avgQ(gid).c_str());
			auto c = sql.DirectQuery(cntQ(gid).c_str());
			if (a->uiSQLErrno || c->uiSQLErrno) // the old shape would crash this process
			{
				out += " guild" + std::to_string(gid) + ":{query_failed avg_errno=" + std::to_string(a->uiSQLErrno) + " count_errno="
					+ std::to_string(c->uiSQLErrno) + "}";
				continue;
			}
			// Both readers read the same single row: the old shape first, then rewind for the new one
			const int oldAvg = OldShape(a.get()), oldCnt = OldShape(c.get());
			mysql_data_seek(a->Get()->pSQLResult, 0);
			mysql_data_seek(c->Get()->pSQLResult, 0);
			long long av = -1, cv = -1;
			const bool aok = SQLReadFirstInt(a.get(), av), cok = SQLReadFirstInt(c.get(), cv);
			out += " guild" + std::to_string(gid) + ":{old_avg=" + std::to_string(oldAvg) + " new_avg=" + (aok ? std::to_string(av) : "fail")
				+ " old_count=" + std::to_string(oldCnt) + " new_count=" + (cok ? std::to_string(cv) : "fail") + "}";
		}

		server_stop();
		auto fa = sql.DirectQuery(avgQ(7).c_str());
		auto fc = sql.DirectQuery(cntQ(7).c_str());
		long long v = -1;
		const bool newAvg = SQLReadFirstInt(fa.get(), v), newCnt = SQLReadFirstInt(fc.get(), v);
		out += " db_down:{errno=" + std::to_string(fa->uiSQLErrno) + " old_avg=" + old_shape_in_child(fa.get()) + " old_count="
			+ old_shape_in_child(fc.get()) + " new_avg=" + (newAvg ? "value" : "refused") + " new_count=" + (newCnt ? "value" : "refused") + "}";
		server_start();
		printf("%s%s\n", g_scn, out.c_str());
	}

	// --- M1: one statement per call (CLIENT_MULTI_STATEMENTS off) ---------------------------------------------------
	// The only statements with a ';' in the sources are item_award's (game/questlua_pc.cpp:2726, 2754): trailing " ;".
	void M1()
	{
		must(g_admin, "CREATE TABLE rt.ia (id INT AUTO_INCREMENT PRIMARY KEY, login VARCHAR(30) NOT NULL, vnum INT NOT NULL,"
			" count INT NOT NULL, given_time DATETIME, why VARCHAR(128) NOT NULL, mall TINYINT NOT NULL) ENGINE=InnoDB");
		const char* award = "INSERT INTO rt.ia (login, vnum, count, given_time, why, mall)select 'm1login', 27001, 1, now(), 'm1why', 1"
			" from DUAL where not exists (select login, why from rt.ia where login = 'm1login' and why  = 'm1why') ;";

		Probe worker, direct;
		start(worker, true);
		start(direct, false);
		// One after another (two connections writing the same award at once race on InnoDB locks; not what is tested here)
		worker.AsyncQuery(award);	// as game/questlua_pc.cpp sends it (DBManager::Query)
		ms(500);
		worker.AsyncQuery(award);	// same award again: NOT EXISTS keeps one row
		ms(500);
		auto d = direct.DirectQuery(award);
		auto two = direct.DirectQuery("INSERT INTO rt.m (tag) VALUES ('m1-a'); INSERT INTO rt.m (tag) VALUES ('m1-b')");
		ms(500);

		SQLStats ws;
		worker.CollectStats(ws);
		printf("%s award_rows=%lld worker_ok=%llu worker_err=%llu direct_award_errno=%u | two_statements errno=%u result=%s"
			" applied_a=%lld applied_b=%lld\n", g_scn, scalar(g_admin, "SELECT COUNT(*) FROM rt.ia"), (unsigned long long) ws.ok,
			(unsigned long long) ws.err, d->uiSQLErrno, two->uiSQLErrno, SQLResultName(two->eResult), marker("m1-a"), marker("m1-b"));
		worker.Quit();
	}

	// --- L2: failure ledger overflow (queue 64): the caller never waits, drops are counted, nothing raw anywhere ------
	// A worker whose first connect fails never runs; Quit() reports every queued message as unexecuted_at_quit in a
	// tight loop on the caller's thread (no network per message), far faster than the ledger writer drains its queue.
	void L2()
	{
		const char* SECRET = "S3CR3T-L2";
		if (!sql_failure_ledger::Instance().Start("probe", (long) getpid(), 14, 64))
			die("ledger start", nullptr);

		Probe sql;
		sql.Setup("127.0.0.1", "nobody", "x", "none", "", false, 1); // nothing listens on port 1
		ms(300);
		sql.SetLabel("probe.rt.main");

		const int N = 20000;
		const std::string q = std::string("INSERT INTO rt.m (tag) VALUES ('") + SECRET + "')";
		for (int i = 0; i < N; ++i)
			sql.AsyncQuery(q.c_str());

		const auto t0 = clk::now();
		sql.Quit();
		const long long quitUs = std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - t0).count();
		ms(2000);

		SQLStats st;
		sql.CollectStats(st);
		const unsigned long long dropped = sql_failure_ledger::Instance().Dropped();
		const unsigned long long writeErrors = sql_failure_ledger::Instance().WriteErrors();
		glob_t g{};
		long long lines = 0, secret = 0;
		if (glob("log/sql_failures_*.log", 0, nullptr, &g) == 0)
		{
			for (size_t i = 0; i < g.gl_pathc; ++i)
			{
				lines += file_count(g.gl_pathv[i], "src=sql_failure");
				secret += file_count(g.gl_pathv[i], SECRET);
			}
			globfree(&g);
		}
		// Exact numbers depend on the writer thread's speed; the invariants do not
		printf("%s queued=%d events=%llu events_eq_queued=%d unexecuted=%llu dropped_gt_0=%d lines_plus_dropped_eq_events=%d"
			" write_errors=%llu quit_lt_1s=%d secret_in_syserr=%lld secret_in_syslog=%lld secret_in_ledger=%lld"
			" syserr_lines_for_quit=%lld\n", g_scn, N, (unsigned long long) st.failureEvents, st.failureEvents == (uint64_t) N ? 1 : 0,
			(unsigned long long) st.resUnexecutedAtQuit, dropped > 0 ? 1 : 0, (unsigned long long) lines + dropped == st.failureEvents ? 1 : 0,
			writeErrors, quitUs < 1000000 ? 1 : 0, file_count("syserr.log", SECRET), file_count("syslog.log", SECRET), secret,
			file_count("syserr.log", "AsyncSQL:"));
		fprintf(stderr, "L2 measured: quit_us=%lld per_event_ns=%lld lines=%lld dropped=%llu\n", quitUs, quitUs * 1000 / N, lines, dropped);
	}
#endif

	// --- D1-D3: what a DirectQuery caller sees (DirectQuery caller audit; same code against the old and the 2a library) --
	// Per call: errno, the fields callers read (Get() null?, uiNumRows, uiAffectedRows), whether the call held the caller
	// for >= 1 s, and whether the statement was applied in the end.
	std::string direct_call(Probe& sql, const std::string& tag)
	{
		const auto t0 = clk::now();
		auto r = sql.DirectQuery(mark(tag).c_str());
		const long long took = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
		SQLResult* res = r ? r->Get() : nullptr;
		char buf[256];
		std::snprintf(buf, sizeof(buf), " %s:{errno=%u get_null=%d rows=%u affected=%u held_ge_1s=%d}", tag.c_str(),
			r ? r->uiSQLErrno : 0u, res ? 0 : 1, res ? res->uiNumRows : 0u, res ? res->uiAffectedRows : 0u, took >= 1000 ? 1 : 0);
		return buf;
	}

	// D1: database down, no reconnect before it; three calls 0.5 s apart; the server comes back 3 s after it stopped
	void D1()
	{
		Probe sql;
		start(sql, false);
		std::string out = direct_call(sql, "d1-up");
		server_stop();
		const auto down = clk::now();
		std::thread restarter([] { ms(3000); server_start(); });
		for (int i = 1; i <= 3; ++i)
		{
			out += direct_call(sql, "d1-down" + std::to_string(i));
			ms(500);
		}
		restarter.join();
		const long long downMs = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - down).count();
		ms(500);
		out += direct_call(sql, "d1-back");
		printf("%s%s | applied: down1=%lld down2=%lld down3=%lld back=%lld outage_ge_3s=%d\n", g_scn, out.c_str(),
			marker("d1-down1"), marker("d1-down2"), marker("d1-down3"), marker("d1-back"), downMs >= 3000 ? 1 : 0);
	}

	// D2: as D1, but the connection was silently re-established by the statement before (K7 precondition)
	void D2()
	{
		Probe sql;
		start(sql, false);
		k7_prepare(sql, false, "d2");
		server_stop();
		std::thread restarter([] { ms(3000); server_start(); });
		std::string out;
		for (int i = 1; i <= 3; ++i)
		{
			out += direct_call(sql, "d2-down" + std::to_string(i));
			ms(500);
		}
		restarter.join();
		ms(500);
		out += direct_call(sql, "d2-back");
		printf("%s%s | applied: down1=%lld down2=%lld down3=%lld back=%lld\n", g_scn, out.c_str(), marker("d2-down1"),
			marker("d2-down2"), marker("d2-down3"), marker("d2-back"));
	}

	// D3: server up, statements the server rejects: what the caller's checks read
	void D3()
	{
		Probe sql;
		start(sql, false);
		std::string out;
		for (const char* q : { "UPDATE rt.no_such_table SET v=1 WHERE id=1", "SELECT v FROM rt.no_such_table WHERE id=1",
			"UPDATE rt.h SET v=1 WHERE id=424242", "SELECT v FROM rt.h WHERE id=424242", "INSERT INTO rt.m (tag) VALUES ('d3-ok')",
			"INSERT INTO rt.no_such_table (tag) VALUES ('d3-fail')" })
		{
			auto r = sql.DirectQuery(q);
			SQLResult* res = r ? r->Get() : nullptr;
			char buf[200];
			std::snprintf(buf, sizeof(buf), " {errno=%u get_null=%d result_set=%d rows=%u affected=%u insert_id=%u}",
				r ? r->uiSQLErrno : 0u, res ? 0 : 1, res && res->pSQLResult ? 1 : 0, res ? res->uiNumRows : 0u,
				res ? res->uiAffectedRows : 0u, res ? res->uiInsertID : 0u);
			out += buf;
		}
		printf("%s missing_table_update/missing_table_select/no_row_update/no_row_select/insert_ok/insert_fail_after_ok:%s\n",
			g_scn, out.c_str());
	}

	// D4: the checks of db/ClientManagerPlayer.cpp __QUERY_PLAYER_CREATE (:926 INSERT player, `uiAffectedRows <= 0` on a
	// uint32_t, then `player_id = uiInsertID`; :941 UPDATE player_index, same check), copied onto throwaway tables.
	// A replica of the caller's logic on the same libsql, not the db process itself.
	// mode 0: the INSERT fails because the server is down (and so does the UPDATE);
	// mode 1: server up, the INSERT is rejected (duplicate name; stands for any server-side error), the UPDATE runs.
	void player_create_replica(int mode)
	{
		must(g_admin, "CREATE TABLE rt.p (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(24) NOT NULL UNIQUE) ENGINE=InnoDB");
		must(g_admin, "CREATE TABLE rt.pi (id INT PRIMARY KEY, pid1 INT NOT NULL DEFAULT 0) ENGINE=InnoDB");
		must(g_admin, "INSERT INTO rt.pi (id) VALUES (2)");

		Probe sql;
		start(sql, false);
		// an earlier, successful character creation on the same connection (another account)
		auto prev = sql.DirectQuery("INSERT INTO rt.p (name) VALUES ('first')");
		const unsigned prevId = prev->Get()->uiInsertID;

		if (mode == 0)
			server_stop();

		auto ins = sql.DirectQuery(mode == 0 ? "INSERT INTO rt.p (name) VALUES ('second')" : "INSERT INTO rt.p (name) VALUES ('first')");
		const bool insCheckPasses = !(ins->Get()->uiAffectedRows <= 0);
		const unsigned playerId = ins->Get()->uiInsertID;

		auto upd = sql.DirectQuery(("UPDATE rt.pi SET pid1=" + std::to_string(playerId) + " WHERE id=2").c_str());
		const bool updCheckPasses = !(upd->Get()->uiAffectedRows <= 0);

		if (mode == 0)
			server_start();
		printf("%s insert_errno=%u insert_affected=%u insert_check_passes=%d player_id_used=%u equals_earlier_insert=%d"
			" update_errno=%u update_check_passes=%d => %s | player_index.pid1=%lld\n", g_scn, ins->uiSQLErrno,
			ins->Get()->uiAffectedRows, insCheckPasses ? 1 : 0, playerId, playerId == prevId && prevId != 0 ? 1 : 0, upd->uiSQLErrno,
			updCheckPasses ? 1 : 0, insCheckPasses && updCheckPasses ? "CREATE_SUCCESS sent" : "create failed",
			scalar(g_admin, "SELECT pid1 FROM rt.pi WHERE id=2"));
	}
	// D6: how long one DirectQuery holds the caller while the server is down (no reconnect before; D1 precondition)
	void D6()
	{
		Probe sql;
		start(sql, false);
		sql.DirectQuery(mark("d6-up").c_str());
		server_stop();
		ms(500);
		std::vector<long long> us;
		const int n = getenv("D6_N") ? atoi(getenv("D6_N")) : 200;
		for (int i = 0; i < n; ++i)
		{
			const auto c0 = clk::now();
			sql.DirectQuery(mark("d6-down").c_str());
			us.push_back(std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - c0).count());
		}
		server_start();
		std::sort(us.begin(), us.end());
		const long long slow = std::count_if(us.begin(), us.end(), [](long long v) { return v >= 900000; });
		printf("%s all_failed=%d\n", g_scn, marker("d6-down") == 0 ? 1 : 0);
		fprintf(stderr, "D6 measured: calls=%d median_us=%lld p99_us=%lld max_us=%lld calls_ge_0.9s=%lld\n", n, us[n / 2],
			us[n * 99 / 100], us[n - 1], slow);
	}

	std::string meta(SQLMsg* r)
	{
		SQLResult* res = r ? r->Get() : nullptr;
		std::string first = "-";
		if (res && res->pSQLResult && res->uiNumRows)
		{
			mysql_data_seek(res->pSQLResult, 0);
			if (MYSQL_ROW row = mysql_fetch_row(res->pSQLResult))
				first = std::string(row[0] ? row[0] : "NULL") + (mysql_num_fields(res->pSQLResult) > 1 && row[1] ? std::string(":") + row[1] : "");
			mysql_data_seek(res->pSQLResult, 0);
		}
		char buf[200];
		std::snprintf(buf, sizeof(buf), "{errno=%u get_null=%d result_set=%d rows=%u affected=%u insert_id=%u first=%s}",
			r ? r->uiSQLErrno : 0u, res ? 0 : 1, res && res->pSQLResult ? 1 : 0, res ? res->uiNumRows : 0u,
			res ? res->uiAffectedRows : 0u, res ? res->uiInsertID : 0u, first.c_str());
		return buf;
	}

	// D7: what callers read after statements that succeed, through DirectQuery and through ReturnQuery (the worker).
	// Must be identical for the library before and after the change (only failures change).
	void D7()
	{
		const char* steps[][2] = {
			{ "ins1", "INSERT INTO rt.s (v) VALUES (1)" },
			{ "ins3", "INSERT INTO rt.s (v) VALUES (2), (3), (4)" },
			{ "upd3", "UPDATE rt.s SET v = v + 10 WHERE v >= 2" },
			{ "upd_same", "UPDATE rt.s SET v = v WHERE id = 1" },
			{ "repl", "REPLACE INTO rt.s (id, v) VALUES (1, 100)" },
			{ "del", "DELETE FROM rt.s WHERE id = 4" },
			{ "sel", "SELECT id, v FROM rt.s ORDER BY id" },
			{ "sel_none", "SELECT id FROM rt.s WHERE id = 999" },
			{ "ins_sel", "INSERT INTO rt.s (v) SELECT v FROM rt.s" },
			{ "sel_count", "SELECT COUNT(*) FROM rt.s" },
			{ "ins_ignore_dup", "INSERT IGNORE INTO rt.s (id, v) VALUES (1, 5)" },
		};
		for (int path = 0; path < 2; ++path)
		{
			sqlrun(g_admin, "DROP TABLE IF EXISTS rt.s");
			must(g_admin, "CREATE TABLE rt.s (id INT AUTO_INCREMENT PRIMARY KEY, v INT NOT NULL) ENGINE=InnoDB");
			Probe sql;
			start(sql, path == 1);
			std::string out;
			for (auto& st : steps)
			{
				std::unique_ptr<SQLMsg> r;
				if (path == 0)
					r = sql.DirectQuery(st[1]);
				else
				{
					sql.ReturnQuery(st[1], nullptr);
					for (int i = 0; i < 200 && !sql.PopResult(r); ++i)
						ms(10);
				}
				out += std::string(" ") + st[0] + "=" + meta(r.get());
			}
			printf("%s %s:%s\n", g_scn, path == 0 ? "direct" : "worker", out.c_str());
			if (path == 1)
				sql.Quit();
		}
	}

	// D8: the rows of a result set do not all arrive: the query is killed while the server is sending them
	// (mysql_read_query_result has already returned the column definitions). Through DirectQuery and ReturnQuery.
	void D8()
	{
		must(g_admin, "CREATE TABLE rt.big (id INT PRIMARY KEY) ENGINE=InnoDB");
		std::string values;
		for (int i = 1; i <= 200; ++i)
			values += (i > 1 ? "," : "") + std::string("(") + std::to_string(i) + ")";
		must(g_admin, "INSERT INTO rt.big (id) VALUES " + values);
		const char* q = "SELECT id, REPEAT('x', 1000) AS pad, IF(id = 100, SLEEP(3), 0) AS s FROM rt.big";

		std::string out;
		for (int path = 0; path < 2; ++path)
		{
			Probe sql;
			start(sql, path == 1);
			std::thread killer([] {
				ms(1000);
				const long long cid = scalar(g_admin,
					"SELECT IFNULL(MAX(id), 0) FROM information_schema.processlist WHERE user = 'rt' AND info LIKE 'SELECT id, REPEAT%'");
				if (cid)
					sqlrun(g_admin, "KILL QUERY " + std::to_string(cid));
			});
			std::unique_ptr<SQLMsg> r;
			if (path == 0)
				r = sql.DirectQuery(q);
			else
			{
				sql.ReturnQuery(q, nullptr);
				for (int i = 0; i < 600 && !sql.PopResult(r); ++i)
					ms(10);
			}
			killer.join();
			out += std::string(" ") + (path == 0 ? "direct" : "worker") + "=" + meta(r.get());
			if (path == 1)
				sql.Quit();
		}
		printf("%s killed_while_sending_rows:%s\n", g_scn, out.c_str());
	}

	void D4() { player_create_replica(0); }
	void D5() { player_create_replica(1); }

	void H1a() { h1(false, false); }
	void H1b() { h1(true, false); }
	void H1c() { h1(true, true); }
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <R1|R2|R3|R4|R5|G1|G2|K7a|K7b|K7c|K7d|P0|P1|P1b|P1c|P2|P3|P4|P5|C1|C2|C3|C4|H1a|H1b|H1c|F1|L1|Q1|O1>\n", argv[0]);
		return 2;
	}
	g_scn = argv[1];
	g_admin = connect_admin();
	guard();
	log_init();

	must(g_admin, "DROP DATABASE IF EXISTS rt");
	must(g_admin, "CREATE DATABASE rt");
	must(g_admin, "CREATE TABLE rt.m (id INT AUTO_INCREMENT PRIMARY KEY, tag VARCHAR(32) NOT NULL) ENGINE=InnoDB");
	must(g_admin, "CREATE TABLE rt.h (id INT PRIMARY KEY, v INT NOT NULL) ENGINE=InnoDB");
	for (const char* t : { "item", "quest", "guild_member", "player" })
		must(g_admin, std::string("TRUNCATE TABLE player.") + t);
	must(g_admin, "TRUNCATE TABLE account.account");

	const std::string s = g_scn;
	if (s == "R1") R1(); else if (s == "R2") R2(); else if (s == "R3") R3(); else if (s == "R4") R4(); else if (s == "R5") R5();
	else if (s == "G1") G1(); else if (s == "G2") G2();
	else if (s == "K7a") K7a(); else if (s == "K7b") K7b(); else if (s == "K7c") K7c(); else if (s == "K7d") K7d();
	else if (s == "P0") P0(); else if (s == "P1") P1(); else if (s == "P2") P2();
	else if (s == "P1b") P1b(); else if (s == "P1c") P1c(); else if (s == "P3") P3(); else if (s == "P4") P4(); else if (s == "P5") P5();
#ifdef HAVE_2A
	else if (s == "F1") F1(); else if (s == "L1") L1(); else if (s == "Q1") Q1(); else if (s == "O1") O1();
	else if (s == "M1") M1(); else if (s == "L2") L2(); else if (s == "G3") G3();
#endif
	else if (s == "D1") D1(); else if (s == "D2") D2(); else if (s == "D3") D3(); else if (s == "D4") D4(); else if (s == "D5") D5(); else if (s == "D6") D6();
	else if (s == "D7") D7(); else if (s == "D8") D8();
	else if (s == "C1") C1(); else if (s == "C2") C2(); else if (s == "C3") C3(); else if (s == "C4") C4();
	else if (s == "H1a") H1a(); else if (s == "H1b") H1b(); else if (s == "H1c") H1c();
	else die("unknown scenario", nullptr);

	ms(1200);
	if (g_admin)
		mysql_close(g_admin);
	return 0;
}
