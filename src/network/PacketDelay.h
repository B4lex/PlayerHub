#pragma once

#include <chrono>
#include <optional>

namespace Network
{
	// Main-loop thread. Records one remote checkpoint as it is applied: the time
	// from its emit on the source client to now, both on the server clock.
	// Ignored while this client has no server clock estimate.
	void RecordRemotePacketDelay(double a_sentAtServerMilliseconds);

	// Main-loop thread, once per pass with the frame delta. Publishes the mean
	// delay of the samples recorded over the last reporting window.
	void UpdatePacketDelay(float a_deltaSeconds);

	// Mean remote packet delay of the last complete window. Empty while
	// disconnected or when no stamped remote checkpoint was applied in that
	// window. Safe to call from any thread.
	[[nodiscard]] std::optional<std::chrono::milliseconds> GetRemotePacketDelay();
}
