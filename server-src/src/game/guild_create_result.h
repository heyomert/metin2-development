#pragma once

#include <cstdint>

// Guild creation (A-17 g11, docs/engineering/db-step2a-directquery-audit.md): whether the INSERT INTO guild created the
// guild. No game or libsql types here, so tools/guild/test-guild-create-logic.cpp tests exactly this code.
//
// Before, CGuild::CGuild never read the result: a failed INSERT gave guild_id 0 and the grade rows, the db member row
// (guild 0, leader), the mark slot and the GUILD_LOAD broadcast were still created. guild_member.pid is UNIQUE, so the
// leader could not join or found any guild afterwards, and a later creation took the fee without adding the leader
// (reproduced on the test VM, docs/worklog/2026-10-11-guild-create-a17.md).

namespace guild_create
{
	// bApplied: the statement executed without error (ESQLResult::APPLIED); uiInsertID: the AUTO_INCREMENT id it created.
	// A failed or AMBIGUOUS statement has no id (DB step 2a), so nothing may be built on it.
	inline bool InsertCreatedGuild(bool bApplied, uint32_t uiInsertID)
	{
		return bApplied && uiInsertID != 0;
	}
}
