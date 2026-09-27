#include "network/ConnectionMonitor.h"
#include "config/PluginConfig.h"
#include "network/SocketIOClient.h"
#include "runtime/GameSession.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <random>
#include <utility>

namespace Network
{
    namespace
    {
        constexpr float kProbeIntervalSeconds = 1.0F;
        // Socket.IO with Engine.IO v4 never notices a silent server on its own,
        // and never fails a pending ack, so an unanswered probe means the
        // connection is dead.
        constexpr float kProbeTimeoutSeconds = 5.0F;
        constexpr float kReconnectDelaySeconds = 1.0F;
        constexpr float kMaximumReconnectDelaySeconds = 15.0F;
        // An attempt whose handshake stalls without failing is torn down and retried.
        constexpr float kReconnectAttemptTimeoutSeconds = 20.0F;
        constexpr float kMaximumFrameTime = 0.1F;
        constexpr unsigned kMaximumBackoffDoublings = 4;
        // Spreads the retries of clients that lost the same server together, so a
        // restart does not meet them all at once.
        constexpr float kReconnectJitterFraction = 0.5F;
        constexpr std::int64_t kNoLatency = -1;

        enum class Phase
        {
            // No connection is wanted, and the client is closed.
            kIdle,
            // No connection is wanted; waiting for a previous worker to finish
            // before the client can be closed.
            kClosing,
            kConnected,
            kWaitingToReconnect,
            kReconnecting,
        };

        std::atomic_int64_t latencyMilliseconds{ kNoLatency };
        std::atomic_uint64_t outstandingProbeID{ 0 };
        std::atomic_bool connectionEnabled{ false };

        // Main-loop thread only.
        Phase phase = Phase::kIdle;
        float phaseSeconds = 0.0F;
        unsigned reconnectAttempts = 0;
        float reconnectDelaySeconds = 0.0F;
        std::uint64_t lastProbeID = 0;
        std::uint64_t probeConnectionGeneration = 0;
        float secondsSinceProbe = kProbeIntervalSeconds;
        std::uint64_t observedConfigRevision = 0;
        bool endpointChangePending = false;
        bool observedInGame = false;

        void ForgetLatency()
        {
            outstandingProbeID.store(0, std::memory_order_release);
            latencyMilliseconds.store(kNoLatency, std::memory_order_release);
        }

        void SendProbe()
        {
            const auto probeID = ++lastProbeID;
            const auto sentAt = std::chrono::steady_clock::now();
            secondsSinceProbe = 0.0F;
            outstandingProbeID.store(probeID, std::memory_order_release);

            EmitLatencyProbe([probeID, sentAt] {
                auto expectedProbeID = probeID;
                if (!outstandingProbeID.compare_exchange_strong(
                        expectedProbeID, 0, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    return;
                }

                const auto roundTrip = std::chrono::steady_clock::now() - sentAt;
                latencyMilliseconds.store(std::chrono::duration_cast<std::chrono::milliseconds>(roundTrip).count(),
                    std::memory_order_release);
            });
        }

        void EnterPhase(Phase a_phase)
        {
            phase = a_phase;
            phaseSeconds = 0.0F;
        }

        float ChooseReconnectDelaySeconds()
        {
            static std::mt19937 generator{ std::random_device{}() };
            std::uniform_real_distribution<float> jitter{ 1.0F - kReconnectJitterFraction,
                1.0F + kReconnectJitterFraction };

            const auto doublings = (std::min)(reconnectAttempts, kMaximumBackoffDoublings);
            const auto delay = kReconnectDelaySeconds * static_cast<float>(1U << doublings);
            return (std::min)(delay, kMaximumReconnectDelaySeconds) * jitter(generator);
        }

        void WaitToReconnect()
        {
            reconnectDelaySeconds = ChooseReconnectDelaySeconds();
            EnterPhase(Phase::kWaitingToReconnect);
        }

        void UpdateConnected(float a_deltaSeconds)
        {
            if (phase != Phase::kConnected)
            {
                logs::info("Server connection established after {} reconnect attempts: connectionGeneration={}",
                    reconnectAttempts, static_cast<unsigned long long>(GetConnectionGeneration()));
                EnterPhase(Phase::kConnected);
                reconnectAttempts = 0;
            }

            const auto generation = GetConnectionGeneration();
            if (generation != probeConnectionGeneration)
            {
                probeConnectionGeneration = generation;
                ForgetLatency();
                SendProbe();
                return;
            }

            secondsSinceProbe += a_deltaSeconds;

            const auto outstanding = outstandingProbeID.load(std::memory_order_acquire);
            if (outstanding != 0)
            {
                if (secondsSinceProbe < kProbeTimeoutSeconds)
                {
                    return;
                }

                logs::warn("Latency probe {} was not acknowledged within {:.0f} seconds; abandoning the connection",
                    static_cast<unsigned long long>(outstanding), kProbeTimeoutSeconds);
                ForgetLatency();
                AbandonConnection();
                return;
            }

            if (secondsSinceProbe >= kProbeIntervalSeconds)
            {
                SendProbe();
            }
        }

        // Returns whether the connection is being moved to a changed endpoint. The
        // move skips the reconnect delay and waits only for a previous attempt's
        // worker to finish.
        bool FollowEndpointChange()
        {
            const auto revision = Config::GetRevision();
            if (revision != observedConfigRevision)
            {
                observedConfigRevision = revision;
                endpointChangePending = endpointChangePending || IsEndpointOutdated();
            }
            if (!endpointChangePending)
            {
                return false;
            }

            if (IsConnected())
            {
                AbandonConnection();
            }
            if (!BeginReconnect())
            {
                return true;
            }
            endpointChangePending = false;
            ForgetLatency();
            reconnectAttempts = 0;
            EnterPhase(Phase::kReconnecting);
            logs::info("Server endpoint changed; reconnecting");
            return true;
        }

        // Each game starts from [Network] AutoConnect.
        void FollowGameStart(bool a_inGame)
        {
            if (a_inGame == std::exchange(observedInGame, a_inGame) || !a_inGame)
            {
                return;
            }
            const bool autoConnect = Config::Get()->network.autoConnect;
            connectionEnabled.store(autoConnect, std::memory_order_release);
            logs::info(
                "Game started; {}", autoConnect ? "connecting automatically" : "waiting for the player to connect");
        }

        void Close()
        {
            if (phase == Phase::kIdle)
            {
                return;
            }
            if (phase != Phase::kClosing)
            {
                logs::info("Closing the server connection: reason={}",
                    connectionEnabled.load(std::memory_order_acquire) ? "left the game" : "player disconnected");
                ForgetLatency();
                EnterPhase(Phase::kClosing);
            }
            if (BeginClose())
            {
                EnterPhase(Phase::kIdle);
            }
        }

        void Open()
        {
            // A previous worker may still be closing the client; retried next pass.
            if (!BeginReconnect())
            {
                return;
            }
            observedConfigRevision = Config::GetRevision();
            endpointChangePending = false;
            reconnectAttempts = 0;
            ForgetLatency();
            EnterPhase(Phase::kReconnecting);
            logs::info("Connecting to the server");
        }

        void UpdateDisconnected(float a_deltaSeconds)
        {
            if (phase == Phase::kConnected)
            {
                ForgetLatency();
                WaitToReconnect();
                logs::warn("Server connection lost; reconnecting in {:.1f} seconds", reconnectDelaySeconds);
            }

            phaseSeconds += a_deltaSeconds;

            if (phase == Phase::kWaitingToReconnect)
            {
                if (phaseSeconds >= reconnectDelaySeconds && BeginReconnect())
                {
                    ++reconnectAttempts;
                    logs::info("Reconnecting to the server: attempt={}", reconnectAttempts);
                    EnterPhase(Phase::kReconnecting);
                }
            }
            else if (DidReconnectAttemptFail() || phaseSeconds >= kReconnectAttemptTimeoutSeconds)
            {
                WaitToReconnect();
                logs::warn("Reconnect attempt {} did not succeed; next attempt in {:.1f} seconds", reconnectAttempts,
                    reconnectDelaySeconds);
            }
        }
    }

    void UpdateConnectionMonitor(float a_deltaSeconds)
    {
        PROFILE_FUNCTION();
        const auto deltaSeconds = std::clamp(a_deltaSeconds, 0.0F, kMaximumFrameTime);
        const bool inGame = GameSession::IsInGame();
        FollowGameStart(inGame);
        if (!inGame || !connectionEnabled.load(std::memory_order_acquire))
        {
            Close();
            return;
        }
        if (phase == Phase::kIdle || phase == Phase::kClosing)
        {
            Open();
            return;
        }
        if (FollowEndpointChange())
        {
            return;
        }
        if (IsConnected())
        {
            UpdateConnected(deltaSeconds);
        }
        else
        {
            UpdateDisconnected(deltaSeconds);
        }
    }

    void SetConnectionEnabled(bool a_enabled)
    {
        if (connectionEnabled.exchange(a_enabled, std::memory_order_acq_rel) != a_enabled)
        {
            logs::info("Player {} the server", a_enabled ? "connected to" : "disconnected from");
        }
    }

    ConnectionState GetConnectionState()
    {
        if (!GameSession::IsInGame() || !connectionEnabled.load(std::memory_order_acquire))
        {
            return IsConnected() || IsTransportOpen() ? ConnectionState::kDisconnecting
                                                      : ConnectionState::kDisconnected;
        }
        return IsConnected() ? ConnectionState::kConnected : ConnectionState::kConnecting;
    }

    std::optional<std::chrono::milliseconds> GetLatency()
    {
        const auto latency = latencyMilliseconds.load(std::memory_order_acquire);
        if (latency == kNoLatency || !IsConnected())
        {
            return std::nullopt;
        }
        return std::chrono::milliseconds{ latency };
    }
}
