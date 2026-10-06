#include "stdafx.h"
#include "DBMetrics.h"
#include "DBManager.h"
#include "QID.h"

#ifdef OS_WINDOWS
#include <process.h>
#else
#include <unistd.h>
#endif

extern int g_iMetricsEnable;

namespace
{
	const int METRICS_KEEP_DAYS = 14;
	const size_t METRICS_QUEUE_SIZE = 64;

	const char* const SLOT_NAMES[SQL_MAX_NUM] = { "player", "account", "common", "hotbackup" };
}

CDBMetrics::CDBMetrics() : m_bEnabled(false)
{
	for (int i = 0; i < SAVE_MAX; ++i)
		m_aullOk[i] = m_aullErr[i] = 0;
}

CDBMetrics::~CDBMetrics()
{
	m_writer.Stop();
}

void CDBMetrics::Initialize()
{
	if (!g_iMetricsEnable)
	{
		sys_log(0, "METRICS: disabled (METRICS_ENABLE: 0)");
		return;
	}

	// Slots that were not connected have no objects and are skipped (Add ignores null)
	CDBManager& rkDB = CDBManager::instance();
	for (int iSlot = 0; iSlot < SQL_MAX_NUM; ++iSlot)
	{
		m_sql.Add("db", SLOT_NAMES[iSlot], "main", rkDB.GetSQLForStats(iSlot, CDBManager::SQL_ROLE_MAIN));
		m_sql.Add("db", SLOT_NAMES[iSlot], "async", rkDB.GetSQLForStats(iSlot, CDBManager::SQL_ROLE_ASYNC));
		m_sql.Add("db", SLOT_NAMES[iSlot], "direct", rkDB.GetSQLForStats(iSlot, CDBManager::SQL_ROLE_DIRECT));
	}

	if (!m_writer.Start("log", "sql", METRICS_KEEP_DAYS, METRICS_QUEUE_SIZE))
	{
		sys_err("METRICS: initialization failed, SQL lines disabled");
		return;
	}

	m_sql.Start(sql_metrics_reporter::Clock::now());
	m_bEnabled = true;
	sys_log(0, "METRICS: SQL lines every 10 s to log/sql_YYYY-MM-DD.log, kept %d days", METRICS_KEEP_DAYS);
}

void CDBMetrics::NoteResult(int iType, unsigned int uiFinalErrno)
{
	int i;
	switch (iType)
	{
		case QID_PLAYER_SAVE:		i = SAVE_PLAYER; break;
		case QID_ITEM_SAVE:			i = SAVE_ITEM; break;
		case QID_ITEM_DESTROY:		i = DESTROY_ITEM; break;
		case QID_QUEST_SAVE:		i = SAVE_QUEST; break;
		case QID_SAFEBOX_SAVE:		i = SAVE_SAFEBOX; break;
		case QID_ITEM_AWARD_TAKEN:	i = AWARD_TAKEN; break;
		default:					return;
	}

	++(uiFinalErrno ? m_aullErr[i] : m_aullOk[i]);
}

void CDBMetrics::Window()
{
	if (m_bEnabled)
		Emit(false);
}

void CDBMetrics::Shutdown()
{
	if (!m_bEnabled)
		return;

	Emit(true);
	m_bEnabled = false;
	m_writer.Stop();
}

void CDBMetrics::Emit(bool bFinal)
{
	// Δ of this window: SAVE results that reached AnalyzeQueryResult. Results of the cache flush at shutdown are
	// never popped (the main loop has ended), so they are not in here; they only show on the connection lines.
	char extra[512];
	snprintf(extra, sizeof(extra),
		" save_player_ok=%llu save_player_err=%llu save_item_ok=%llu save_item_err=%llu"
		" destroy_item_ok=%llu destroy_item_err=%llu save_quest_ok=%llu save_quest_err=%llu"
		" save_safebox_ok=%llu save_safebox_err=%llu award_taken_ok=%llu award_taken_err=%llu",
		(unsigned long long) m_aullOk[SAVE_PLAYER], (unsigned long long) m_aullErr[SAVE_PLAYER],
		(unsigned long long) m_aullOk[SAVE_ITEM], (unsigned long long) m_aullErr[SAVE_ITEM],
		(unsigned long long) m_aullOk[DESTROY_ITEM], (unsigned long long) m_aullErr[DESTROY_ITEM],
		(unsigned long long) m_aullOk[SAVE_QUEST], (unsigned long long) m_aullErr[SAVE_QUEST],
		(unsigned long long) m_aullOk[SAVE_SAFEBOX], (unsigned long long) m_aullErr[SAVE_SAFEBOX],
		(unsigned long long) m_aullOk[AWARD_TAKEN], (unsigned long long) m_aullErr[AWARD_TAKEN]);

	for (int i = 0; i < SAVE_MAX; ++i)
		m_aullOk[i] = m_aullErr[i] = 0;

	// db quits its SQL connections before this final window, so the copy-queue leftovers are counted
#ifdef OS_WINDOWS
	const long lPid = (long) _getpid();
#else
	const long lPid = (long) getpid();
#endif
	m_sql.Window(sql_metrics_reporter::Clock::now(), "db", lPid, m_writer, extra, bFinal, bFinal);
}
