#include "network/ConnectionMonitor.h"
#include "config/PluginConfig.h"
#include "network/ServerClient.h"
#include "runtime/FrameTime.h"
#include "runtime/GameSession.h"
#include "runtime/Profiling.h"
#include "runtime/VanillaIntro.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <string_view>
#include <utility>

namespace Network
{
    namespace
    {
        constexpr float kProbeIntervalSeconds = 1.0F;
        // The transport notices a closed connection, but not a server that stops
        // answering while the socket stays open.
        constexpr float kProbeTimeoutSeconds = 5.0F;
        constexpr float kReconnectDelaySeconds = 1.0F;
        constexpr float kMaximumReconnectDelaySeconds = 15.0F;
        constexpr float kReconnectAttemptTimeoutSeconds = 20.0F;
        constexpr unsigned kMaximumBackoffDoublings = 4;
        constexpr float kReconnectJitterFraction = 0.5F;
        constexpr std::int64_t kNoLatency = -1;

        enum class Phase
        {
            kIdle,
            kClosing,
            kConnected,
            kWaitingToReconnect,
            kReconnecting,
        };

        std::atomic_int64_t latencyMilliseconds{ kNoLatency };
        std::atomic_uint64_t outstandingProbeID{ 0 };
        std::atomic_bool connectionEnabled{ false };
        std::atomic_bool displayNameSet{ false };
        std::atomic_bool endpointSet{ true };

        std::mutex refusalLock;
        std::optional<Refusal> refusal;

        // Main-loop thread only.
        Phase phase = Phase::kIdle;
        float phaseSeconds = 0.0F;
        unsigned reconnectAttempts = 0;
        float reconnectDelaySeconds = 0.0F;
        std::uint64_t lastProbeID = 0;
        std::uint64_t probeConnectionGeneration = 0;
        float secondsSinceProbe = kProbeIntervalSeconds;
        std::optional<std::uint64_t> requirementsConfigRevision;
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

        void SetRefusal(std::optional<Refusal> a_refusal)
        {
            const std::scoped_lock lock{ refusalLock };
            refusal = std::move(a_refusal);
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

        void FollowRequirements()
        {
            const auto revision = Config::GetRevision();
            if (requirementsConfigRevision == revision)
            {
                return;
            }
            requirementsConfigRevision = revision;
            const auto config = Config::Get();
            const bool nameSet = !config->connection.displayName.empty();
            const bool hasEndpoint = !Config::GetServerEndpoint(config->connection).empty();
            if (displayNameSet.exchange(nameSet, std::memory_order_acq_rel) != nameSet)
            {
                logs::info("Display name {}", nameSet ? "set" : "cleared; not connecting without one");
                if (!nameSet)
                {
                    // Applying one again must not connect by itself.
                    connectionEnabled.store(false, std::memory_order_release);
                }
            }
            if (endpointSet.exchange(hasEndpoint, std::memory_order_acq_rel) != hasEndpoint)
            {
                logs::info("Server endpoint {}",
                    hasEndpoint ? "available" : "missing: the custom server has no URL; not opening a connection without one");
            }
        }

        void FollowGameStart(bool a_inGame)
        {
            if (a_inGame == std::exchange(observedInGame, a_inGame))
            {
                return;
            }
            SetRefusal(std::nullopt);
            if (!a_inGame)
            {
                return;
            }
            const bool autoConnect = Config::Get()->connection.autoConnect;
            const bool nameSet = displayNameSet.load(std::memory_order_acquire);
            const bool hasEndpoint = endpointSet.load(std::memory_order_acquire);
            const bool connect = autoConnect && nameSet && hasEndpoint;
            connectionEnabled.store(connect, std::memory_order_release);
            const auto* next = "waiting for the player to connect";
            if (connect)
            {
                next = "connecting automatically";
            }
            else if (autoConnect && !nameSet)
            {
                next = "waiting for the player to enter a display name and connect";
            }
            else if (autoConnect)
            {
                next = "waiting for the player to enter a custom server endpoint and connect";
            }
            logs::info("Game started; {}", next);
        }

        bool IsConnectionWanted()
        {
            return GameSession::IsInGame() && !VanillaIntro::IsPlaying()
                && connectionEnabled.load(std::memory_order_acquire) && displayNameSet.load(std::memory_order_acquire)
                && (endpointSet.load(std::memory_order_acquire) || IsConnected());
        }

        void Close(std::string_view a_reason)
        {
            if (phase == Phase::kIdle)
            {
                return;
            }
            if (phase != Phase::kClosing)
            {
                logs::info("Closing the server connection: reason={}", a_reason);
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
            if (!BeginReconnect())
            {
                return;
            }
            reconnectAttempts = 0;
            ForgetLatency();
            EnterPhase(Phase::kReconnecting);
            logs::info("Connecting to the server");
        }

        void UpdateDisconnected(float a_deltaSeconds)
        {
            if (auto refused = GetRefusal())
            {
                logs::error("Not reconnecting: the server refused the connection ({})", GetRefusalName(refused->reason));
                SetRefusal(std::move(refused));
                connectionEnabled.store(false, std::memory_order_release);
                Close("the server refused the connection");
                return;
            }
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
        const auto deltaSeconds = FrameTime::Clamp(a_deltaSeconds);
        const bool inGame = GameSession::IsInGame();
        const bool introPlaying = VanillaIntro::IsPlaying();
        FollowRequirements();
        FollowGameStart(inGame && !introPlaying);
        if (!inGame)
        {
            Close("left the game");
            return;
        }
        if (introPlaying)
        {
            Close("the vanilla intro is playing");
            return;
        }
        if (!displayNameSet.load(std::memory_order_acquire))
        {
            Close("no display name is set");
            return;
        }
        if (!endpointSet.load(std::memory_order_acquire) && !IsConnected())
        {
            // Entering one later must not connect by itself.
            connectionEnabled.store(false, std::memory_order_release);
            Close("no custom server endpoint is set");
            return;
        }
        if (!connectionEnabled.load(std::memory_order_acquire))
        {
            Close("player disconnected");
            return;
        }
        if (phase == Phase::kIdle || phase == Phase::kClosing)
        {
            Open();
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
        if (a_enabled)
        {
            SetRefusal(std::nullopt);
        }
        if (connectionEnabled.exchange(a_enabled, std::memory_order_acq_rel) != a_enabled)
        {
            logs::info("Player {} the server", a_enabled ? "connected to" : "disconnected from");
        }
    }

    ConnectionState GetConnectionState()
    {
        if (!IsConnectionWanted())
        {
            return IsConnected() || IsTransportOpen() ? ConnectionState::kDisconnecting
                                                      : ConnectionState::kDisconnected;
        }
        return IsConnected() ? ConnectionState::kConnected : ConnectionState::kConnecting;
    }

    std::optional<Refusal> GetConnectionRefusal()
    {
        const std::scoped_lock lock{ refusalLock };
        return refusal;
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
