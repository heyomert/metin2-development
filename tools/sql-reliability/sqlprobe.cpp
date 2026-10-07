// TEST/PROBE TOOL ONLY (no runtime code).
// Analysis probes for the AsyncSQL fix design (DB step 2, docs/engineering/db-step2-asyncsql-fix.md). Unlike sqlrt
// (the frozen baseline) these answer design questions with the REAL table definitions (server/sql/player.sql: item and
// quest InnoDB, player and guild_member Aria) and the real statement shapes of db/Cache.cpp, db/ClientManager*.cpp.
// Runs only against the temporary MariaDB started by probe.sh; same fail-closed guard as sqlrt.
//
//   sqlprobe <R1|R2|R3|R4|R5|G1|G2|K7a|K7b|K7c|K7d|P0|P1|P1b|P1c|P2|P3|P4|P5|C1|C2|C3|C4|H1a|H1b|H1c>
//   env (probe.sh): RT_PORT, RT_PW, RT_SOCK, RT_TOKEN, RT_START (shell command that starts the temporary server)
#include "libsql/AsyncSQL.h"

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

	std::string errnos()
	{
		std::string s;
		for (int e : { 1205, 1213, 1927, 1053, 2002, 2006, 2013, 2014 })
			if (const long long n = syserr(("errno: " + std::to_string(e) + ")").c_str()))
				s += "e" + std::to_string(e) + "=" + std::to_string(n) + " ";
		return s + "retrying=" + std::to_string(syserr("AsyncSQL: retrying")) + " reconnected="
			+ std::to_string(syserr("was reconnected")) + " locale_fail=" + std::to_string(syserr("cannot set locale"));
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
		ms(500);
		sql.AsyncQuery(mark("g1-after").c_str()); // wakes a stuck head, if any
		ms(2500);
		printf("%s down: %s | after restart: before=%lld during=%lld after=%lld %s\n", g_scn, during.c_str(),
			marker("g1-before"), marker("g1-during"), marker("g1-after"), errnos().c_str());
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
		const long long fails = syserr("query failed"), retries = syserr("AsyncSQL: retrying");
		setCond(false);
		sql.Quit();
		static const char* names[] = { "", "access_denied", "unknown_database", "too_many_connections", "user_connection_limit" };
		printf("%s case=%s raw rc=%d errno=%u | asyncsql_today: failures_logged_in_3s=%lld retrying_lines=%lld\n", g_scn,
			names[which], rc, e, fails, retries);
	}
	void C1() { conn_case(1); }
	void C2() { conn_case(2); }
	void C3() { conn_case(3); }
	void C4() { conn_case(4); }

	void H1a() { h1(false, false); }
	void H1b() { h1(true, false); }
	void H1c() { h1(true, true); }
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <R1|R2|R3|R4|R5|G1|G2|K7a|K7b|K7c|K7d|P0|P1|P1b|P1c|P2|P3|P4|P5|C1|C2|C3|C4|H1a|H1b|H1c>\n", argv[0]);
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
	else if (s == "C1") C1(); else if (s == "C2") C2(); else if (s == "C3") C3(); else if (s == "C4") C4();
	else if (s == "H1a") H1a(); else if (s == "H1b") H1b(); else if (s == "H1c") H1c();
	else die("unknown scenario", nullptr);

	ms(1200);
	if (g_admin)
		mysql_close(g_admin);
	return 0;
}
