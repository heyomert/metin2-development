#pragma once

#include "AsyncSQL.h"

#include <cstdlib>

// The first column of the first row of a statement that answers with exactly one row (SELECT COUNT(*), AVG(...), ...).
// Returns false when the statement failed or no row came back: the caller must then not use a made-up value
// (DB step 2a, A-17: a failed DirectQuery has no result set, and mysql_fetch_row(NULL) returns NULL).
// A NULL column is a real answer (AVG over no rows) and reads as 0, as str_to_number left it before.
inline bool SQLReadFirstInt(SQLMsg* msg, long long& out)
{
	out = 0;
	if (!msg || msg->uiSQLErrno != 0)
		return false;

	SQLResult* res = msg->Get();
	if (!res || !res->pSQLResult)
		return false;

	MYSQL_ROW row = mysql_fetch_row(res->pSQLResult);
	if (!row)
		return false;

	if (row[0] && row[0][0])
		out = std::strtoll(row[0], nullptr, 10);
	return true;
}
