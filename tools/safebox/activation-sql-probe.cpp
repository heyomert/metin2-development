// TEST/PROBE TOOL ONLY (no runtime code). A-19 account-level safebox activation against MariaDB, through CAsyncSQL
// ReturnQuery (the worker, as db and game use it), with the exact statement and result classification of
// server-src/src/db/SafeboxActivation.h and the exact login status query and parsing of game/safebox_activation.h.
// Runs only against the temporary MariaDB started by activation-sql-probe.sh (datadir prefix + per-run token guard).
// Same order as db/stdafx.h: the base types, then common/length.h and common/tables.h (through
// db/SafeboxActivation.h), then libsql/AsyncSQL.h, which #defines QUERY_MAX_LEN that length.h declares as an enum value
#include "libthecore/stdafx.h"
#include "db/SafeboxActivation.h"
#include "game/safebox_activation.h"
#include "libsql/AsyncSQL.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

// game/db define this in their version.cpp; the probe has no build identity
const char* M2BuildFields() { return " build=probe build_dirty=0 build_src=probe"; }

namespace
{
	MYSQL* g_admin = nullptr;
	int g_pass = 0, g_fail = 0;

	[[noreturn]] void die(const std::string& what, MYSQL* h)
	{
		printf("FATAL %s: %s\n", what.c_str(), h ? mysql_error(h) : "");
		fflush(stdout);
		_exit(3);
	}

	void must(MYSQL* h, const std::string& q)
	{
		if (mysql_real_query(h, q.c_str(), q.size()))
			die(q, h);
		if (MYSQL_RES* r = mysql_store_result(h))
			mysql_free_result(r);
	}

	std::string rows_of(MYSQL* h, const std::string& q)
	{
		if (mysql_real_query(h, q.c_str(), q.size()))
			die(q, h);
		std::string out;
		if (MYSQL_RES* r = mysql_store_result(h))
		{
			const unsigned n = mysql_num_fields(r);
			while (MYSQL_ROW row = mysql_fetch_row(r))
			{
				out += out.empty() ? "" : " ";
				for (unsigned i = 0; i < n; ++i)
					out += (i ? ":" : "") + std::string(row[i] ? row[i] : "NULL");
			}
			mysql_free_result(r);
		}
		return out.empty() ? "(none)" : out;
	}

	void ms(int n) { std::this_thread::sleep_for(std::chrono::milliseconds(n)); }

	void check(bool ok, const std::string& what)
	{
		(ok ? g_pass : g_fail)++;
		printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
	}

	void guard()
	{
		const char* token = getenv("RT_TOKEN");
		if (!token || strlen(token) < 32)
			die("guard: RT_TOKEN missing; run through activation-sql-probe.sh", nullptr);
		const std::string dd = rows_of(g_admin, "SELECT @@datadir");
		if (dd.rfind("/var/tmp/m2sbprobe-", 0) != 0)
			die("guard: refusing to run against datadir " + dd, nullptr);
		if (rows_of(g_admin, "SELECT token FROM m2sqlrt_guard.token") != token)
			die("guard: run token does not match", nullptr);
	}

	long long count_lines(const char* file, const char* needle)
	{
		std::ifstream in(file);
		std::string line;
		long long n = 0;
		while (std::getline(in, line))
			if (line.find(needle) != std::string::npos)
				++n;
		return n;
	}

	struct Probe : public CAsyncSQL
	{
	};

	void start(Probe& sql, const char* user, const char* pw)
	{
		if (!sql.Setup("127.0.0.1", user, pw, "player", "latin1", false, atoi(getenv("RT_PORT"))))
			die("asyncsql setup", nullptr);
		for (int i = 0; i < 100 && !sql.IsConnected(); ++i)
			ms(50);
		if (!sql.IsConnected())
			die("asyncsql connect", nullptr);
	}

	std::unique_ptr<SQLMsg> run(Probe& sql, const char* q)
	{
		sql.ReturnQuery(q, nullptr);
		std::unique_ptr<SQLMsg> r;
		for (int i = 0; i < 500 && !sql.PopResult(r); ++i)
			ms(10);
		if (!r)
			die("no result from the worker", nullptr);
		return r;
	}

	// The db path: SafeboxActivateQuery -> ReturnQuery -> SafeboxActivateOutcome
	uint8_t activate(Probe& sql, uint32_t acc, bool* pbUnexpected)
	{
		char q[512];
		SafeboxActivateQuery(q, sizeof(q), "", acc);
		auto r = run(sql, q);
		return SafeboxActivateOutcome(r->uiSQLErrno, r->Get() ? r->Get()->uiAffectedRows : 0, pbUnexpected);
	}

	// The game path: SAFEBOX_ACTIVATION_STATUS_QUERY -> ReturnQuery -> the parsing of game/db.cpp QID_SAFEBOX_STATUS
	safebox_activation::TStatus status(Probe& sql, uint32_t acc)
	{
		char q[512];
		snprintf(q, sizeof(q), SAFEBOX_ACTIVATION_STATUS_QUERY, "", acc, "", acc);
		auto r = run(sql, q);
		SQLResult* res = r->Get();
		const bool bFailed = r->uiSQLErrno != 0 || !res || !res->pSQLResult || res->uiNumRows != 1;
		MYSQL_ROW row = bFailed ? nullptr : mysql_fetch_row(res->pSQLResult);
		return safebox_activation::ParseStatus(bFailed || !row, row ? row[0] : nullptr, row ? row[1] : nullptr);
	}

	std::string row_of(uint32_t acc)
	{
		return rows_of(g_admin, "SELECT account_id, size, IF(password = '', 'default', password) FROM player.safebox WHERE account_id = "
			+ std::to_string(acc));
	}
}

int main()
{
	g_admin = mysql_init(nullptr);
	if (!mysql_real_connect(g_admin, nullptr, "root", nullptr, nullptr, 0, getenv("RT_SOCK"), 0))
		die("admin connect", g_admin);
	guard();
	log_init();

	must(g_admin, "TRUNCATE TABLE player.safebox");
	must(g_admin, "TRUNCATE TABLE player.item");
	// fixtures (synthetic ids): row size 3; row size 0; row with a password; rowless account with a SAFEBOX item;
	// an INVENTORY item whose owner (a pid) has the same number as an account
	must(g_admin, "INSERT INTO player.safebox (account_id, size) VALUES (1002, 3), (1003, 0)");
	must(g_admin, "INSERT INTO player.safebox (account_id, size, password) VALUES (1004, 1, 'pw1234')");
	must(g_admin, "INSERT INTO player.item (id, owner_id, window, pos, count, vnum) VALUES (1, 2001, 'SAFEBOX', 0, 1, 8006)");
	must(g_admin, "INSERT INTO player.item (id, owner_id, window, pos, count, vnum) VALUES (2, 2002, 'INVENTORY', 0, 1, 8006)");
	must(g_admin, "INSERT INTO player.safebox (account_id, size) VALUES (2003, 1)");
	must(g_admin, "INSERT INTO player.item (id, owner_id, window, pos, count, vnum) VALUES (3, 2003, 'SAFEBOX', 0, 1, 8006)");

	{
		Probe sql;
		start(sql, "rt", getenv("RT_PW"));
		bool u = false;

		check(activate(sql, 1001, &u) == SAFEBOX_ACTIVATE_CREATED && !u && row_of(1001) == "1001:1:default", "new row -> CREATED, size 1, default password");
		check(activate(sql, 1001, &u) == SAFEBOX_ACTIVATE_ALREADY && !u && row_of(1001) == "1001:1:default", "same request again -> ALREADY, row unchanged");
		check(activate(sql, 1002, &u) == SAFEBOX_ACTIVATE_ALREADY && !u && row_of(1002) == "1002:3:default", "existing size 3 -> ALREADY, size stays 3");
		check(activate(sql, 1003, &u) == SAFEBOX_ACTIVATE_ALREADY && !u && row_of(1003) == "1003:1:default", "existing size 0 -> ALREADY, size raised to 1");
		check(activate(sql, 1004, &u) == SAFEBOX_ACTIVATE_ALREADY && !u && row_of(1004) == "1004:1:pw1234", "existing password -> ALREADY, password kept");

		namespace sa = safebox_activation;
		const sa::TStatus a = status(sql, 2003);
		check(sa::StateFromStatus(a) == sa::STATE_ACTIVE && !sa::NeedsRow(a), "status row+item -> ACTIVE");
		const sa::TStatus b = status(sql, 1002);
		check(sa::StateFromStatus(b) == sa::STATE_ACTIVE && !sa::NeedsRow(b) && b.iSize == 3, "status row/no item -> ACTIVE, size 3");
		const sa::TStatus c = status(sql, 2001);
		check(sa::StateFromStatus(c) == sa::STATE_ACTIVE && sa::NeedsRow(c), "status no row/item -> ACTIVE + ensure");
		const sa::TStatus d = status(sql, 2002);
		check(sa::StateFromStatus(d) == sa::STATE_INACTIVE, "status no row, item only as pid in INVENTORY -> INACTIVE");
		check(sa::StateFromStatus(status(sql, 9999)) == sa::STATE_INACTIVE, "status no row/no item -> INACTIVE");

		// The no-fee ensure for the rowless account: same statement, creates the row, the item stays
		check(activate(sql, 2001, &u) == SAFEBOX_ACTIVATE_CREATED && row_of(2001) == "2001:1:default"
			&& rows_of(g_admin, "SELECT owner_id, window FROM player.item WHERE id = 1") == "2001:SAFEBOX", "rowless ensure -> CREATED, SAFEBOX item untouched");
		sql.Quit();
	}
	{
		// Permanent failure: same statement, an account without INSERT/UPDATE privilege
		Probe ro;
		start(ro, "rt_ro", getenv("RT_RO_PW"));
		bool u = false;
		check(activate(ro, 1005, &u) == SAFEBOX_ACTIVATE_FAILED && row_of(1005) == "(none)", "permanent SQL error -> FAILED, no row");
		ro.Quit();
	}
	{
		// Status query error: an account that can read safebox but not item
		Probe nr;
		start(nr, "rt_noitem", getenv("RT_NOITEM_PW"));
		check(safebox_activation::StateFromStatus(status(nr, 1001)) == safebox_activation::STATE_UNKNOWN, "status query error -> UNKNOWN");
		nr.Quit();
	}

	ms(300);
	check(count_lines("syserr.log", "errno=1062") == 0, "no 1062 in the run");
	printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
	mysql_close(g_admin);
	return g_fail ? 1 : 0;
}
