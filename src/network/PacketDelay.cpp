#include "network/PacketDelay.h"
#include "network/LatencyProbe.h"
#include "network/SocketIOClient.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace Network
{
	namespace
	{
		// Checkpoints arrive every 50 ms, so a window holds many samples and the
		// displayed value changes at a readable rate.
		constexpr float kReportIntervalSeconds = 1.0F;
		constexpr float kMaximumFrameTime = 0.1F;
		constexpr std::int64_t kNoDelay = -1;

		// Written by the main loop, read by the UI.
		std::atomic_int64_t delayMilliseconds{ kNoDelay };

		// Main-loop thread only.
		double windowDelaySum = 0.0;
		std::uint64_t windowSampleCount = 0;
		float secondsSinceReport = 0.0F;

		void ResetWindow()
		{
			windowDelaySum = 0.0;
			windowSampleCount = 0;
			secondsSinceReport = 0.0F;
		}
	}

	void RecordRemotePacketDelay(double a_sentAtServerMilliseconds)
	{
		const auto now = GetServerTimeMilliseconds();
		if (!now) {
			return;
		}

		const auto delay = *now - a_sentAtServerMilliseconds;
		if (!std::isfinite(delay)) {
			return;
		}
		// Each client's clock estimate is off by up to half its path asymmetry,
		// which can make a fast delivery look like it arrived before it was sent.
		windowDelaySum += (std::max)(delay, 0.0);
		++windowSampleCount;
	}

	void UpdatePacketDelay(float a_deltaSeconds)
	{
		if (!IsConnected()) {
			ResetWindow();
			delayMilliseconds.store(kNoDelay, std::memory_order_release);
			return;
		}

		secondsSinceReport += std::clamp(a_deltaSeconds, 0.0F, kMaximumFrameTime);
		if (secondsSinceReport < kReportIntervalSeconds) {
			return;
		}

		const auto mean = windowSampleCount == 0 ?
			kNoDelay :
			std::llround(windowDelaySum / static_cast<double>(windowSampleCount));
		delayMilliseconds.store(mean, std::memory_order_release);
		ResetWindow();
	}

	std::optional<std::chrono::milliseconds> GetRemotePacketDelay()
	{
		const auto delay = delayMilliseconds.load(std::memory_order_acquire);
		if (delay == kNoDelay || !IsConnected()) {
			return std::nullopt;
		}
		return std::chrono::milliseconds{ delay };
	}
}
