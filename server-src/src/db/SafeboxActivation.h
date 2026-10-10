#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "common/tables.h"

// Account-level safebox activation (A-19, docs/engineering/safebox-activation.md): the statement and how its result
// is read. Header-only so tools/safebox/activation-sql-probe.cpp runs exactly this against MariaDB.
//
// Idempotent: a new row gets size 1; an existing row keeps its size (GREATEST: never smaller) and its password.
// No IGNORE: every other error stays an error.
inline int SafeboxActivateQuery(char* szQuery, size_t size, const char* c_pszPostfix, uint32_t dwAccountID)
{
	return snprintf(szQuery, size,
			"INSERT INTO safebox%s (account_id, size) VALUES(%u, 1) ON DUPLICATE KEY UPDATE size = GREATEST(size, 1)",
			c_pszPostfix, dwAccountID);
}

// Applied with 1 affected row: inserted now. 0 (row unchanged) or 2 (existing row raised to size 1): the row was
// there. Any other count cannot come from this statement; it is read as ALREADY (the row exists, no fee is taken)
// and *pbUnexpected is set for the caller to log.
inline uint8_t SafeboxActivateOutcome(unsigned int uiSQLErrno, uint32_t uiAffectedRows, bool* pbUnexpected)
{
	*pbUnexpected = false;

	if (uiSQLErrno != 0)
		return SAFEBOX_ACTIVATE_FAILED;

	if (uiAffectedRows == 1)
		return SAFEBOX_ACTIVATE_CREATED;

	if (uiAffectedRows != 0 && uiAffectedRows != 2)
		*pbUnexpected = true;

	return SAFEBOX_ACTIVATE_ALREADY;
}

inline const char* SafeboxActivateOutcomeName(uint8_t bResult)
{
	switch (bResult)
	{
		case SAFEBOX_ACTIVATE_CREATED:	return "created";
		case SAFEBOX_ACTIVATE_ALREADY:	return "already";
		case SAFEBOX_ACTIVATE_FAILED:	return "failed";
		default:						return "invalid";
	}
}
