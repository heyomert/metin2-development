#ifndef __INC_METIN2_DB_DBMETRICS_H__
#define __INC_METIN2_DB_DBMETRICS_H__

#include "sql_metrics.h"

#include <cstdint>

// db process SQL telemetry: log/sql_YYYY-MM-DD.log (docs/monitoring.md -> "SQL (sql_*.log)").
// Own queue and worker (metrics_writer), so it never stalls the db loop and never shares the syslog/syserr pool.
// Everything except the writer's worker runs on the db main thread.
class CDBMetrics : public singleton<CDBMetrics>
{
	public:
		CDBMetrics();
		virtual ~CDBMetrics();

		void	Initialize();	// after every CDBManager::Connect; never throws, leaves the lines off on failure
		void	Window();		// every 10 s from CClientManager::Process
		void	Shutdown();		// after CDBManager::Quit (counts what was left unexecuted), before log_destroy

		// Every query result handed to CClientManager::AnalyzeQueryResult, before anything else looks at it
		void	NoteResult(int iType, unsigned int uiFinalErrno);

	private:
		enum ESave
		{
			SAVE_PLAYER,
			SAVE_ITEM,
			DESTROY_ITEM,
			SAVE_QUEST,
			SAVE_SAFEBOX,
			AWARD_TAKEN,
			SAVE_MAX,
		};

		void	Emit(bool bFinal);

		bool					m_bEnabled;
		metrics_writer			m_writer;
		sql_metrics_reporter	m_sql;

		uint64_t	m_aullOk[SAVE_MAX];
		uint64_t	m_aullErr[SAVE_MAX];
};

#endif
