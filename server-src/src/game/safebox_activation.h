#pragma once

#include <cstdint>
#include <cstdlib>

// Account-level safebox activation (A-19, docs/engineering/safebox-activation.md): the per-character state machine and
// its gold hold. No game types here, so tools/safebox/test-activation-logic.cpp tests exactly this code.

// Login status of the account (DBManager::ReturnQuery format; account id twice): the safebox row's size, NULL without a
// row, and whether the account has items in the SAFEBOX window (owner_id is the account id there, a pid elsewhere)
#define SAFEBOX_ACTIVATION_STATUS_QUERY \
	"SELECT (SELECT size FROM safebox%s WHERE account_id = %u), " \
	"EXISTS(SELECT 1 FROM item%s WHERE owner_id = %u AND window = 'SAFEBOX')"

namespace safebox_activation
{
	// Values are the quest API (game.get_safebox_activation)
	enum EState : int
	{
		STATE_UNKNOWN = -1,		// status not answered yet or the query failed: never treated as INACTIVE (no fee path)
		STATE_INACTIVE = 0,		// no row and no SAFEBOX item
		STATE_ACTIVE = 1,		// the account's row, or (legacy) SAFEBOX items without a row
		STATE_PENDING = 2,		// this character's request is in flight
	};

	// game.request_safebox_activation
	enum ERequest : int
	{
		REQUEST_SENT = 0,
		REQUEST_NOT_INACTIVE = 1,
		REQUEST_NOT_ENOUGH_GOLD = 2,
		REQUEST_BUSY = 3,
		REQUEST_INVALID = 4,
	};

	enum EKind : uint8_t
	{
		KIND_NONE,
		KIND_PAID,		// the fee is held until the result: CREATED keeps it, ALREADY and FAILED give it back
		KIND_ENSURE,	// no fee: an account that is already active by legacy evidence gets its row
	};

	struct TState
	{
		int			iState = STATE_UNKNOWN;
		bool		bQueryInFlight = false;
		uint32_t	dwRequestID = 0;	// outstanding request, 0 = none
		uint8_t		bKind = KIND_NONE;
		int			iHold = 0;			// taken from the usable gold, still the player's in every save until settled
	};

	struct TStatus
	{
		bool	bFailed = true;
		bool	bHasRow = false;
		int		iSize = 0;
		bool	bHasItem = false;
	};

	// One row of SAFEBOX_ACTIVATION_STATUS_QUERY; bFailed: the query failed or did not return exactly one row
	inline TStatus ParseStatus(bool bFailed, const char* szSize, const char* szHasItem)
	{
		TStatus s;
		s.bFailed = bFailed || !szHasItem;
		if (s.bFailed)
			return s;
		s.bHasRow = szSize != nullptr;
		s.iSize = szSize ? atoi(szSize) : 0;
		s.bHasItem = atoi(szHasItem) != 0;
		return s;
	}

	inline int StateFromStatus(const TStatus& s)
	{
		if (s.bFailed)
			return STATE_UNKNOWN;
		return (s.bHasRow || s.bHasItem) ? STATE_ACTIVE : STATE_INACTIVE;
	}

	// A legacy account (SAFEBOX items, no row) is active; its row is created without a fee
	inline bool NeedsRow(const TStatus& s)
	{
		return !s.bFailed && !s.bHasRow && s.bHasItem;
	}

	// Paid request. On REQUEST_SENT the caller takes iFee from the usable gold (it is now s.iHold) and sends the request.
	inline int Request(TState& s, int64_t llGold, int iFee, uint32_t dwRequestID)
	{
		if (iFee <= 0 || dwRequestID == 0)
			return REQUEST_INVALID;

		if (s.dwRequestID != 0 || s.iState == STATE_PENDING)
			return REQUEST_BUSY;

		if (s.iState != STATE_INACTIVE)
			return REQUEST_NOT_INACTIVE;

		if (llGold < iFee)
			return REQUEST_NOT_ENOUGH_GOLD;

		s.iState = STATE_PENDING;
		s.dwRequestID = dwRequestID;
		s.bKind = KIND_PAID;
		s.iHold = iFee;
		return REQUEST_SENT;
	}

	// Request without a fee. bBackground: an ACTIVE legacy account gets its row and stays ACTIVE whatever the result;
	// otherwise only from INACTIVE (a character that paid per character before A-19), which becomes PENDING.
	inline bool Ensure(TState& s, uint32_t dwRequestID, bool bBackground)
	{
		if (dwRequestID == 0 || s.dwRequestID != 0)
			return false;

		if (bBackground)
		{
			if (s.iState != STATE_ACTIVE)
				return false;
		}
		else
		{
			if (s.iState != STATE_INACTIVE)
				return false;

			s.iState = STATE_PENDING;
		}

		s.dwRequestID = dwRequestID;
		s.bKind = KIND_ENSURE;
		return true;
	}

	struct TSettlement
	{
		bool	bMatched = false;	// false: stale or foreign result, this character is not changed
		bool	bCharged = false;	// the held fee became the payment (CREATED of a paid request)
		int		iRefund = 0;		// gold to give back to the usable gold
	};

	// Result of the outstanding request: bCreated = the row was inserted now; bFailed = the statement did not apply
	inline TSettlement Settle(TState& s, uint32_t dwRequestID, bool bFailed, bool bCreated)
	{
		TSettlement r;

		if (dwRequestID == 0 || dwRequestID != s.dwRequestID)
			return r;

		r.bMatched = true;

		if (s.bKind == KIND_PAID)
		{
			if (!bFailed && bCreated)
				r.bCharged = true;
			else
				r.iRefund = s.iHold;
		}

		// A failed statement may still have been applied (ambiguous result): ask again instead of assuming INACTIVE
		if (s.iState == STATE_PENDING)
			s.iState = bFailed ? STATE_UNKNOWN : STATE_ACTIVE;

		s.dwRequestID = 0;
		s.bKind = KIND_NONE;
		s.iHold = 0;
		return r;
	}

	// The gold a save writes: the hold is still the player's until the fee is settled
	inline int64_t SavedGold(int64_t llGold, const TState& s)
	{
		return llGold + s.iHold;
	}

	// A gold gain must leave room for the hold, so that a refund can never overflow
	inline bool GainFits(int64_t llGold, int64_t llAmount, const TState& s, int64_t llGoldMax)
	{
		return llGold + llAmount + s.iHold < llGoldMax;
	}
}
