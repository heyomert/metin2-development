#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

namespace spdlog
{
	class async_logger;
	namespace details { class thread_pool; }
}

class metrics_daily_sink;

// Periodic server health line (docs/monitoring.md). Accumulators are touched only by the game thread; writing
// happens on a private spdlog worker with discard_new, so a metrics problem drops lines instead of stalling the
// game loop, and the global syslog/syserr pool is never shared.
class CServerMetrics : public singleton<CServerMetrics>
{
	public:
		using Clock = std::chrono::steady_clock;

		CServerMetrics();
		virtual ~CServerMetrics();

		void	Initialize();	// after config_init; never throws, leaves metrics off on failure
		void	Shutdown();		// before thecore_destroy; idempotent

		bool	IsEnabled() const { return m_bEnabled; }

		// Game thread only; callers skip these when !IsEnabled(). Sections share timestamps (the end of one section
		// is the start of the next) because a clock read is not free: ~11.6 us per steady_clock::now() on the
		// VirtualBox test VM (ACPI-fast timecounter), see docs/monitoring.md.
		void	BeginIteration(int iPassedPulses);				// right after thecore_idle() returned pulses; emits one line per window
		void	Mark();											// restart the section clock without attributing the gap
		void	MarkEvents(int iEventCount);					// after event_process()
		void	MarkHeartbeat();								// end of heartbeat()
		void	MarkCharacterUpdate();							// after CHARACTER_MANAGER::Update()
		void	MarkIO();										// after db_clientdesc->Update() and io_loop()
		void	OnBytesWrittenReset(int iCurrentBytesWritten);	// right before current_bytes_written is zeroed
		void	EndIteration(int iCurrentBytesWritten);			// end of idle()

	private:
		void	ResetWindow(Clock::time_point now);
		void	AddBytesWritten(int iCurrentBytesWritten);
		void	Emit(Clock::time_point now);

	private:
		bool	m_bEnabled;

		std::shared_ptr<spdlog::details::thread_pool>	m_pool;
		std::shared_ptr<metrics_daily_sink>				m_sink;
		std::shared_ptr<spdlog::async_logger>			m_logger;

		Clock::time_point	m_startTime;
		Clock::time_point	m_windowStart;
		Clock::time_point	m_iterationStart;
		Clock::time_point	m_stamp;

		Clock::duration		m_event;
		Clock::duration		m_heartbeat;
		Clock::duration		m_character;
		Clock::duration		m_io;
		Clock::duration		m_work;
		Clock::duration		m_workMax;
		Clock::duration		m_gapMax;

		uint64_t	m_iterations;
		uint64_t	m_pulses;
		uint64_t	m_latePulses;
		uint64_t	m_lateIterations;
		uint64_t	m_maxLatePulses;
		uint64_t	m_events;
		uint64_t	m_sentBytes;
		int64_t		m_bytesSeen;
};
