// TEST TOOL ONLY (no runtime code). A-19 account-level safebox activation: the state machine, the gold hold and the
// login status classification of server-src/src/game/safebox_activation.h, tested as is (the game uses the same header).
//
//   c++ -std=c++20 -I server-src/src -o test-activation-logic tools/safebox/test-activation-logic.cpp && ./test-activation-logic
#include "game/safebox_activation.h"

#include <cstdio>
#include <string>

namespace sa = safebox_activation;

namespace
{
	int g_pass = 0, g_fail = 0;

	void check(bool ok, const std::string& what)
	{
		(ok ? g_pass : g_fail)++;
		printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
	}

	constexpr int64_t GOLD_MAX = 2000000000;	// common/length.h
	constexpr int FEE = 500;

	sa::TState Inactive()
	{
		sa::TState s;
		s.iState = sa::STATE_INACTIVE;
		return s;
	}

	// The CHARACTER flow around the header: request takes the fee from the usable gold, a refund gives it back
	struct Player
	{
		int64_t gold;
		sa::TState s;

		int Request(uint32_t id)
		{
			const int r = sa::Request(s, gold, FEE, id);
			if (r == sa::REQUEST_SENT)
				gold -= FEE;
			return r;
		}

		sa::TSettlement Settle(uint32_t id, bool failed, bool created)
		{
			const sa::TSettlement r = sa::Settle(s, id, failed, created);
			gold += r.iRefund;
			return r;
		}

		int64_t Saved() const { return sa::SavedGold(gold, s); }
	};
}

int main()
{
	// --- login status: (size or NULL, has SAFEBOX item)
	{
		const sa::TStatus rowItem = sa::ParseStatus(false, "1", "1");
		check(sa::StateFromStatus(rowItem) == sa::STATE_ACTIVE && !sa::NeedsRow(rowItem), "status row+item -> ACTIVE, no ensure");
		const sa::TStatus row = sa::ParseStatus(false, "3", "0");
		check(sa::StateFromStatus(row) == sa::STATE_ACTIVE && !sa::NeedsRow(row) && row.iSize == 3, "status row/no item -> ACTIVE (size read)");
		const sa::TStatus item = sa::ParseStatus(false, nullptr, "1");
		check(sa::StateFromStatus(item) == sa::STATE_ACTIVE && sa::NeedsRow(item), "status no row/item -> ACTIVE + ensure");
		const sa::TStatus none = sa::ParseStatus(false, nullptr, "0");
		check(sa::StateFromStatus(none) == sa::STATE_INACTIVE && !sa::NeedsRow(none), "status no row/no item -> INACTIVE");
		const sa::TStatus failed = sa::ParseStatus(true, nullptr, nullptr);
		check(sa::StateFromStatus(failed) == sa::STATE_UNKNOWN && !sa::NeedsRow(failed), "status query error -> UNKNOWN");
		const sa::TStatus missing = sa::ParseStatus(false, "1", nullptr);
		check(sa::StateFromStatus(missing) == sa::STATE_UNKNOWN, "status without the item column -> UNKNOWN");
	}

	// --- request preconditions
	{
		Player p{ 499, Inactive() };
		check(p.Request(1) == sa::REQUEST_NOT_ENOUGH_GOLD && p.gold == 499 && p.s.iState == sa::STATE_INACTIVE && p.s.iHold == 0,
			"gold 499: not enough, nothing held, still INACTIVE");
	}
	{
		Player p{ 500, Inactive() };
		check(p.Request(1) == sa::REQUEST_SENT && p.gold == 0 && p.s.iHold == 500 && p.s.iState == sa::STATE_PENDING,
			"gold 500: sent, usable gold 0, 500 held, PENDING");
		check(p.Saved() == 500, "gold 500: a save during PENDING writes 500 (hold is still the player's)");
	}
	{
		Player p{ 1234, Inactive() };
		check(p.Request(1) == sa::REQUEST_SENT && p.gold == 734 && p.Saved() == 1234, "gold 1234: sent, usable 734, saved 1234");
		check(p.Request(2) == sa::REQUEST_BUSY && p.gold == 734 && p.s.iHold == 500 && p.s.dwRequestID == 1,
			"duplicate request while PENDING: busy, nothing more held");
	}
	{
		sa::TState s;	// UNKNOWN
		check(sa::Request(s, 100000, FEE, 1) == sa::REQUEST_NOT_INACTIVE && s.iHold == 0, "UNKNOWN: no request (never charged as INACTIVE)");
		s.iState = sa::STATE_ACTIVE;
		check(sa::Request(s, 100000, FEE, 1) == sa::REQUEST_NOT_INACTIVE && s.iHold == 0, "ACTIVE: no request (no second fee)");
		sa::TState i = Inactive();
		check(sa::Request(i, 100000, 0, 1) == sa::REQUEST_INVALID && sa::Request(i, 100000, FEE, 0) == sa::REQUEST_INVALID
			&& i.iState == sa::STATE_INACTIVE, "fee 0 or request id 0: invalid");
	}

	// --- settlement
	{
		Player p{ 1000, Inactive() };
		p.Request(7);
		const sa::TSettlement r = p.Settle(7, false, true);
		check(r.bMatched && r.bCharged && r.iRefund == 0 && p.gold == 500 && p.Saved() == 500 && p.s.iState == sa::STATE_ACTIVE,
			"CREATED: fee charged once, ACTIVE, saved gold 500");
		check(!p.Settle(7, false, true).bMatched && p.gold == 500, "same result twice: second one ignored");
		check(p.Request(8) == sa::REQUEST_NOT_INACTIVE && p.gold == 500, "after CREATED: no second fee");
	}
	{
		Player p{ 1000, Inactive() };
		p.Request(7);
		const sa::TSettlement r = p.Settle(7, false, false);
		check(r.bMatched && !r.bCharged && r.iRefund == 500 && p.gold == 1000 && p.Saved() == 1000 && p.s.iState == sa::STATE_ACTIVE,
			"ALREADY: hold returned, ACTIVE, no fee");
	}
	{
		Player p{ 1000, Inactive() };
		p.Request(7);
		const sa::TSettlement r = p.Settle(7, true, false);
		check(r.bMatched && !r.bCharged && r.iRefund == 500 && p.gold == 1000 && p.s.iState == sa::STATE_UNKNOWN,
			"FAILED: hold returned, UNKNOWN (asked again, never assumed INACTIVE)");
	}
	{
		Player p{ 1000, Inactive() };
		p.Request(7);
		const sa::TSettlement r = p.Settle(6, false, true);
		check(!r.bMatched && p.gold == 500 && p.s.iHold == 500 && p.s.iState == sa::STATE_PENDING && p.Saved() == 1000,
			"stale request id: nothing changed, still held");
		check(!p.Settle(0, false, true).bMatched, "request id 0: ignored");
	}
	{
		// The session ends before the result: the last save already wrote the hold as the player's gold
		Player p{ 1000, Inactive() };
		p.Request(7);
		check(p.Saved() == 1000, "logout/warp while PENDING: saved gold 1000, the fee is not lost");
	}

	// --- no-fee ensure
	{
		sa::TState s = Inactive();
		check(sa::Ensure(s, 3, false) && s.iState == sa::STATE_PENDING && s.bKind == sa::KIND_ENSURE && s.iHold == 0,
			"legacy ensure from INACTIVE: PENDING, nothing held");
		const sa::TSettlement r = sa::Settle(s, 3, false, true);
		check(r.bMatched && !r.bCharged && r.iRefund == 0 && s.iState == sa::STATE_ACTIVE, "legacy ensure CREATED: ACTIVE, no fee");
		sa::TState f = Inactive();
		sa::Ensure(f, 4, false);
		const sa::TSettlement rf = sa::Settle(f, 4, true, false);
		check(rf.bMatched && rf.iRefund == 0 && f.iState == sa::STATE_UNKNOWN, "legacy ensure FAILED: UNKNOWN, no gold change");
	}
	{
		sa::TState s;
		s.iState = sa::STATE_ACTIVE;
		check(sa::Ensure(s, 5, true) && s.iState == sa::STATE_ACTIVE, "rowless-items ensure at login: stays ACTIVE");
		sa::Settle(s, 5, true, false);
		check(s.iState == sa::STATE_ACTIVE && s.dwRequestID == 0, "rowless-items ensure FAILED: still ACTIVE (items are the evidence)");
		sa::TState i = Inactive();
		check(!sa::Ensure(i, 6, true), "background ensure on INACTIVE: refused");
		sa::TState busy = Inactive();
		Player p{ 1000, busy };
		p.Request(9);
		check(!sa::Ensure(p.s, 10, false) && p.s.dwRequestID == 9, "ensure while a paid request is pending: refused");
	}

	// --- GOLD_MAX: the hold keeps room for its refund
	{
		Player p{ GOLD_MAX - 1, Inactive() };
		p.Request(11);
		check(p.gold == GOLD_MAX - 501 && p.Saved() == GOLD_MAX - 1, "near GOLD_MAX: saved gold stays below GOLD_MAX");
		check(!sa::GainFits(p.gold, 1, p.s, GOLD_MAX), "near GOLD_MAX while PENDING: a gain that would leave no room for the hold is refused");
		check(sa::GainFits(p.gold, 0, p.s, GOLD_MAX), "near GOLD_MAX while PENDING: no gain fits");
		const sa::TSettlement r = p.Settle(11, false, false);
		check(r.iRefund == 500 && p.gold == GOLD_MAX - 1 && p.gold < GOLD_MAX, "near GOLD_MAX: ALREADY refund lands at GOLD_MAX - 1");
		sa::TState none = Inactive();
		check(sa::GainFits(GOLD_MAX - 2, 1, none, GOLD_MAX) && !sa::GainFits(GOLD_MAX - 1, 1, none, GOLD_MAX),
			"without a hold: the old limit (gold + amount < GOLD_MAX)");
	}

	printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
