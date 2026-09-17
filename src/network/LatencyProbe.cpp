#include "network/LatencyProbe.h"
#include "network/SocketIOClient.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Network
{
	namespace
	{
		constexpr float kProbeIntervalSeconds = 1.0F;
		// Socket.IO never fails a pending ack, for example when the transport
		// drops, so an unanswered probe is abandoned after this long.
		constexpr float kProbeTimeoutSeconds = 5.0F;
		constexpr float kMaximumFrameTime = 0.1F;
		constexpr std::int64_t kNoLatency = -1;

		// Written by the transport thread's ack callback, read by the UI.
		std::atomic_int64_t latencyMilliseconds{ kNoLatency };
		// Server clock minus local system clock, or NaN while unknown. Written by
		// the ack callback, read by publishers and the delay tracker.
		std::atomic<double> serverClockOffsetMilliseconds{
			std::numeric_limits<double>::quiet_NaN()
		};
		// The probe whose acknowledgement is still expected, or 0 for none.
		// Acks of abandoned probes lose the compare-exchange and are ignored.
		std::atomic_uint64_t outstandingProbeID{ 0 };

		// Main-loop thread only.
		std::uint64_t lastProbeID = 0;
		std::uint64_t probeConnectionGeneration = 0;
		float secondsSinceProbe = kProbeIntervalSeconds;

		double GetSystemTimeMilliseconds(std::chrono::system_clock::time_point a_time)
		{
			return std::chrono::duration<double, std::milli>(a_time.time_since_epoch()).count();
		}

		void ForgetLatency()
		{
			outstandingProbeID.store(0, std::memory_order_release);
			latencyMilliseconds.store(kNoLatency, std::memory_order_release);
			serverClockOffsetMilliseconds.store(
				std::numeric_limits<double>::quiet_NaN(),
				std::memory_order_release);
		}

		void SendProbe()
		{
			const auto probeID = ++lastProbeID;
			const auto sentAt = std::chrono::steady_clock::now();
			const auto sentAtSystem = std::chrono::system_clock::now();
			secondsSinceProbe = 0.0F;
			outstandingProbeID.store(probeID, std::memory_order_release);

			EmitLatencyProbe([probeID, sentAt, sentAtSystem](std::optional<double> a_serverTime) {
				auto expectedProbeID = probeID;
				if (!outstandingProbeID.compare_exchange_strong(
						expectedProbeID,
						0,
						std::memory_order_acq_rel,
						std::memory_order_acquire)) {
					return;
				}

				// Wall-clock round trip: frame rate does not affect the result.
				const auto roundTrip = std::chrono::steady_clock::now() - sentAt;
				latencyMilliseconds.store(
					std::chrono::duration_cast<std::chrono::milliseconds>(roundTrip).count(),
					std::memory_order_release);

				// Assume the server read its clock halfway through the round trip.
				// The error is at most half of any asymmetry between the two legs.
				if (a_serverTime) {
					const auto midpoint = GetSystemTimeMilliseconds(sentAtSystem) +
					                      std::chrono::duration<double, std::milli>(roundTrip).count() / 2.0;
					serverClockOffsetMilliseconds.store(
						*a_serverTime - midpoint,
						std::memory_order_release);
				}
			});
		}
	}

	void UpdateLatencyProbe(float a_deltaSeconds)
	{
		if (!IsConnected()) {
			ForgetLatency();
			secondsSinceProbe = kProbeIntervalSeconds;
			return;
		}

		// The server keeps no probe state, but a reconnect can strand the
		// outstanding ack and the previous sample describes another connection.
		const auto generation = GetConnectionGeneration();
		if (generation != probeConnectionGeneration) {
			probeConnectionGeneration = generation;
			ForgetLatency();
			SendProbe();
			return;
		}

		secondsSinceProbe += std::clamp(a_deltaSeconds, 0.0F, kMaximumFrameTime);

		const auto outstanding = outstandingProbeID.load(std::memory_order_acquire);
		if (outstanding != 0) {
			if (secondsSinceProbe < kProbeTimeoutSeconds) {
				return;
			}

			auto expectedProbeID = outstanding;
			if (outstandingProbeID.compare_exchange_strong(
					expectedProbeID,
					0,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				latencyMilliseconds.store(kNoLatency, std::memory_order_release);
				logs::warn(
					"Latency probe {} was not acknowledged within {:.0f} seconds",
					static_cast<unsigned long long>(outstanding),
					kProbeTimeoutSeconds);
			}
		}

		if (secondsSinceProbe >= kProbeIntervalSeconds) {
			SendProbe();
		}
	}

	std::optional<std::chrono::milliseconds> GetLatency()
	{
		const auto latency = latencyMilliseconds.load(std::memory_order_acquire);
		if (latency == kNoLatency || !IsConnected()) {
			return std::nullopt;
		}
		return std::chrono::milliseconds{ latency };
	}

	std::optional<double> GetServerTimeMilliseconds()
	{
		const auto offset = serverClockOffsetMilliseconds.load(std::memory_order_acquire);
		if (std::isnan(offset) || !IsConnected()) {
			return std::nullopt;
		}
		return GetSystemTimeMilliseconds(std::chrono::system_clock::now()) + offset;
	}
}
