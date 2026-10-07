// CLIENT_MULTI_STATEMENTS dependency test (DB step 2a, docs/engineering/db-step2-asyncsql-fix.md). AsyncSQL connects
// with CLIENT_MULTI_STATEMENTS; this runs the statement shapes the game sends, with the flag on and off, against the
// throwaway MariaDB of run.sh (never the game database), using the same send/read calls as CAsyncSQL::Attempt.
//
//   c++ -std=c++20 -O2 -I<build>/vendor/mariadb-connector-c-3.4.5/include \
//       -I<server-src>/vendor/mariadb-connector-c-3.4.5/include -o multistmt multistmt.cpp \
//       <build>/lib/libmariadbclient.a -lssl -lcrypto -lpthread -lm
//   ./multistmt <socket> <user> <password> <db>
//
// Per case: rc of send/read, errno, rows affected, and what mysql_next_result reports afterwards (the second
// statement's error is only visible there, which AsyncSQL does not check).
#include <mysql.h>

#include <cstdio>
#include <cstring>

namespace
{
	struct Case
	{
		const char* name;
		const char* sql;
	};

	// questlua_pc.cpp:2726 shape: INSERT ... SELECT ... FROM DUAL WHERE NOT EXISTS (...) with a trailing " ;"
	const Case CASES[] = {
		{ "plain_insert", "INSERT INTO ms (v) VALUES (1)" },
		{ "trailing_semicolon", "INSERT INTO ms (v) select 2 from DUAL where not exists (select v from ms where v = 2) ;" },
		{ "two_statements", "INSERT INTO ms (v) VALUES (3); INSERT INTO ms (v) VALUES (4)" },
		{ "second_fails", "INSERT INTO ms (v) VALUES (5); INSERT INTO no_such_table (v) VALUES (6)" },
	};

	void Run(const char* sock, const char* user, const char* pass, const char* db, bool multi)
	{
		MYSQL m;
		mysql_init(&m);
		if (!mysql_real_connect(&m, nullptr, user, pass, db, 0, sock, multi ? CLIENT_MULTI_STATEMENTS : 0))
		{
			std::printf("multi=%d connect failed errno=%u\n", multi ? 1 : 0, mysql_errno(&m));
			return;
		}
		mysql_query(&m, "DROP TABLE IF EXISTS ms");
		mysql_query(&m, "CREATE TABLE ms (v INT) ENGINE=InnoDB");

		for (const Case& c : CASES)
		{
			int sendRc = mysql_send_query(&m, c.sql, static_cast<unsigned long>(std::strlen(c.sql)));
			int readRc = sendRc ? -1 : mysql_read_query_result(&m);
			unsigned firstErr = mysql_errno(&m);
			long long affected = readRc == 0 ? static_cast<long long>(mysql_affected_rows(&m)) : -1;

			// What AsyncSQL's DrainResults/Store loop would walk through
			int nextRc = 0, more = 0;
			unsigned nextErr = 0;
			if (readRc == 0)
			{
				if (MYSQL_RES* r = mysql_store_result(&m))
					mysql_free_result(r);
				while ((nextRc = mysql_next_result(&m)) == 0)
				{
					++more;
					if (MYSQL_RES* r = mysql_store_result(&m))
						mysql_free_result(r);
				}
				if (nextRc > 0)
					nextErr = mysql_errno(&m);
			}
			std::printf("multi=%d case=%s send_rc=%d read_rc=%d errno=%u affected=%lld more_results=%d next_rc=%d next_errno=%u\n",
				multi ? 1 : 0, c.name, sendRc, readRc, firstErr, affected, more, nextRc, nextErr);
		}

		if (mysql_query(&m, "SELECT GROUP_CONCAT(v ORDER BY v) FROM ms") == 0)
		{
			MYSQL_RES* r = mysql_store_result(&m);
			MYSQL_ROW row = r ? mysql_fetch_row(r) : nullptr;
			std::printf("multi=%d table_rows=%s\n", multi ? 1 : 0, row && row[0] ? row[0] : "(none)");
			if (r)
				mysql_free_result(r);
		}
		mysql_query(&m, "DROP TABLE IF EXISTS ms");
		mysql_close(&m);
	}
}

int main(int argc, char** argv)
{
	if (argc < 5)
	{
		std::fprintf(stderr, "usage: %s <socket> <user> <password> <db>\n", argv[0]);
		return 2;
	}
	Run(argv[1], argv[2], argv[3], argv[4], true);
	Run(argv[1], argv[2], argv[3], argv[4], false);
	return 0;
}
