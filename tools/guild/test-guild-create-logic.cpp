// TEST TOOL ONLY (no runtime code). A-17 g11 guild creation: the decision of server-src/src/game/guild_create_result.h,
// tested as is (the game uses the same header). Only an applied INSERT with a real AUTO_INCREMENT id may create a guild;
// every other combination must leave guild_id 0 so that no side effect starts and no fee is taken.
//
//   c++ -std=c++20 -I server-src/src -o test-guild-create-logic tools/guild/test-guild-create-logic.cpp && ./test-guild-create-logic
#include "game/guild_create_result.h"

#include <cstdio>
#include <string>

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
	using guild_create::InsertCreatedGuild;

	check(InsertCreatedGuild(true, 3), "applied with id 3 -> guild created");
	check(InsertCreatedGuild(true, 0xFFFFFFFFu), "applied with the largest id -> guild created");
	check(!InsertCreatedGuild(true, 0), "applied but id 0 -> no guild (id 0 is never a row)");
	check(!InsertCreatedGuild(false, 0), "failed (not delivered / permanent / ambiguous), id 0 -> no guild (g11)");
	check(!InsertCreatedGuild(false, 2), "failed with a leftover id -> no guild (before 2a: the previous INSERT's id)");

	// No failed statement may create a guild, whatever id is left in the result
	int iCreatedOnFailure = 0;
	for (uint32_t uiID : { 0u, 1u, 2u, 1000u, 0xFFFFFFFFu })
		if (InsertCreatedGuild(false, uiID))
			++iCreatedOnFailure;
	check(iCreatedOnFailure == 0, "failed statement never creates a guild for any id");

	printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
