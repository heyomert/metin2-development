// TEST TOOL ONLY (no runtime code). A-17 g18/g19 pc.change_name: the decision of server-src/src/game/change_name_result.h,
// tested as is (the game uses the same header). The quest consumes the item only on RET_CHANGED, so every failure must
// map to RET_DB_ERROR and only a certainly applied one-row UPDATE to RET_CHANGED.
//
//   c++ -std=c++20 -I server-src/src -o test-change-name-logic tools/change-name/test-change-name-logic.cpp && ./test-change-name-logic
#include "game/change_name_result.h"

#include <cstdio>
#include <string>

namespace cn = change_name;

namespace
{
	int g_pass = 0, g_fail = 0;

	void check(bool ok, const std::string& what)
	{
		(ok ? g_pass : g_fail)++;
		printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
	}
}

int main()
{
	// Quest API values must not move: change_name.quest compares the numbers
	check(cn::RET_NOT_RELOGGED == 0 && cn::RET_NO_STRING == 1 && cn::RET_INVALID_NAME == 2 && cn::RET_NAME_IN_USE == 3 &&
			cn::RET_CHANGED == 4 && cn::RET_NOT_SUPPORTED == 5 && cn::RET_DB_ERROR == 6, "quest return values 0..6 unchanged");
	check(cn::CHECK_PASSED < 0, "CHECK_PASSED is not a quest value");

	// Name check (SELECT COUNT(*))
	check(cn::AfterNameCheck(true, 0) == cn::CHECK_PASSED, "check: answered 0 -> go on");
	check(cn::AfterNameCheck(true, 1) == cn::RET_NAME_IN_USE, "check: answered 1 -> name in use (3)");
	check(cn::AfterNameCheck(true, 2) == cn::RET_NAME_IN_USE, "check: answered 2 (existing duplicates) -> name in use");
	check(cn::AfterNameCheck(false, 0) == cn::RET_DB_ERROR, "check: failed -> db error (6), not 'name free' (g18)");
	check(cn::AfterNameCheck(false, 1) == cn::RET_DB_ERROR, "check: failed, stale count ignored -> db error");

	// UPDATE
	check(cn::AfterUpdate(true, 1) == cn::RET_CHANGED, "update: applied, 1 row -> changed (4)");
	check(cn::AfterUpdate(false, 0) == cn::RET_DB_ERROR, "update: not delivered / permanent / ambiguous -> db error (g19)");
	check(cn::AfterUpdate(false, 1) == cn::RET_DB_ERROR, "update: failed with a leftover row count -> db error");
	check(cn::AfterUpdate(true, 0) == cn::RET_DB_ERROR, "update: applied but no row changed -> db error");
	check(cn::AfterUpdate(true, 2) == cn::RET_DB_ERROR, "update: applied, more than one row -> db error");

	// Only RET_CHANGED makes the quest consume the item: no failure combination reaches it
	int iConsumingFailures = 0;
	for (int bApplied = 0; bApplied <= 1; ++bApplied)
		for (uint32_t uiRows = 0; uiRows <= 3; ++uiRows)
			if (cn::AfterUpdate(bApplied != 0, uiRows) == cn::RET_CHANGED && !(bApplied && uiRows == 1))
				++iConsumingFailures;
	check(iConsumingFailures == 0, "update: no other (applied, rows) combination returns changed");

	printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
