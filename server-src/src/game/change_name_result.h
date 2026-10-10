#pragma once

#include <cstdint>

// pc.change_name (A-17 g18/g19, docs/engineering/db-step2a-directquery-audit.md): what the name check and the UPDATE
// mean for the quest. No game or libsql types here, so tools/change-name/test-change-name-logic.cpp tests exactly this code.
//
// The quest consumes the item and starts the cooldown only on RET_CHANGED, so every database failure must end as
// RET_DB_ERROR and the side effects (messenger list, change_name log, SetNewName) run only after RET_CHANGED.
// Before, a failed check read as "name free" and a failed UPDATE was never read: the item was consumed and the name
// stayed the same (reproduced on the test VM, docs/worklog/2026-10-10-change-name-a17.md).

namespace change_name
{
	// Values are the quest API (pc.change_name, change_name.quest)
	enum EReturn : int
	{
		RET_NOT_RELOGGED = 0,	// a new name is already set; the player has not logged in again
		RET_NO_STRING = 1,
		RET_INVALID_NAME = 2,
		RET_NAME_IN_USE = 3,
		RET_CHANGED = 4,
		RET_NOT_SUPPORTED = 5,
		RET_DB_ERROR = 6,		// nothing was changed, consumed or logged as a change; the player may try again later
	};

	// AfterNameCheck: the name is free, go on to the UPDATE (not a quest value)
	constexpr int CHECK_PASSED = -1;

	// bRead: the SELECT COUNT(*) answered with a row (SQLReadFirstInt). A failed check is not "name free".
	inline int AfterNameCheck(bool bRead, long long llCount)
	{
		if (!bRead)
			return RET_DB_ERROR;

		return llCount != 0 ? RET_NAME_IN_USE : CHECK_PASSED;
	}

	// bApplied: the UPDATE executed without error (ESQLResult::APPLIED). Only a statement that certainly changed this one
	// row is a change. AMBIGUOUS (sent, answer lost) may have been applied but is still RET_DB_ERROR: the item and the
	// cooldown are kept, so the accepted worst case is one free rename, never a lost item.
	inline int AfterUpdate(bool bApplied, uint32_t uiAffectedRows)
	{
		return (bApplied && uiAffectedRows == 1) ? RET_CHANGED : RET_DB_ERROR;
	}
}
