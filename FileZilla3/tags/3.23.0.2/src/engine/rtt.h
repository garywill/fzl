#ifndef FZ_RTT_HEADER
#define FZ_RTT_HEADER

#include "socket.h"

class CLatencyMeasurement final
{
public:
	CLatencyMeasurement();

	// Returns false if measurement cannot be started due to
	// a measurement already running
	bool Start();

	// Returns fals if there was no measurement running
	bool Stop();

	// In ms, returns -1 if no data is available.
	int GetLatency() const;

	void Reset();

protected:
	fz::monotonic_clock m_start;

	int64_t m_summed_latency{};
	int m_measurements{};

	mutable fz::mutex m_sync{false};
};

#endif
