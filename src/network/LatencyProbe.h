#pragma once

#include <chrono>
#include <optional>

namespace Network
{
	// Sends a latency:probe at a fixed interval and times the server's
	// acknowledgement. Call once per main-loop pass with the frame delta.
	void UpdateLatencyProbe(float a_deltaSeconds);

	// Latest measured round trip. Empty while disconnected, before the first
	// acknowledgement, or after a probe timed out. Safe to call from any thread.
	[[nodiscard]] std::optional<std::chrono::milliseconds> GetLatency();

	// The local wall clock translated to the server clock, in milliseconds since
	// the Unix epoch. Peers compare timestamps on this shared clock, so their own
	// wall clocks do not need to agree. Empty while disconnected or before the
	// first acknowledgement of this connection. Safe to call from any thread.
	[[nodiscard]] std::optional<double> GetServerTimeMilliseconds();
}
