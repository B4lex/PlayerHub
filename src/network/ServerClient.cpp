#include "network/ServerClient.h"
#include "config/PluginConfig.h"
#include "network/PlayerFilterStore.h"
#include "network/SessionStore.h"
#include "network/TrafficMeter.h"
#include "network/WireCodec.h"
#include "protocol/Quantization.h"
#include "runtime/Profiling.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <deque>
#include <exception>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Network
{
    namespace
    {
        using Multiplayer::PeerSlot;

        constexpr std::uint16_t kProtocolMismatchCloseCode = 4001;
        constexpr std::uint16_t kDisplayNameTakenCloseCode = 4004;
        constexpr std::uint16_t kDisplayNameInvalidCloseCode = 4005;
        constexpr std::uint16_t kSessionReplacedCloseCode = 4006;
        constexpr std::uint16_t kServerFullCloseCode = 4007;
        constexpr std::uint16_t kTooManyNewSessionsCloseCode = 4008;
        constexpr int kHandshakeTimeoutSeconds = 10;

        std::atomic_bool connected{ false };
        std::atomic_bool transportOpen{ false };
        std::atomic_uint64_t connectionGeneration{ 0 };
        std::atomic_bool reconnectWorkerRunning{ false };
        std::atomic_bool attemptFailed{ false };
        std::atomic_bool closeRequested{ false };

        std::mutex remoteChangesLock;
        std::unordered_map<PeerSlot, RemotePlayerTransform> pendingTransforms;
        std::unordered_map<PeerSlot, std::uint64_t> latestReceivedSequences;
        std::unordered_map<PeerSlot, CheckpointContext> peerContexts;
        std::unordered_map<PeerSlot, RemotePlayerAppearance> pendingAppearances;
        std::unordered_map<PeerSlot, std::uint64_t> latestReceivedAppearanceRevisions;
        std::unordered_map<PeerSlot, RemotePlayerEquipment> pendingEquipment;
        std::unordered_map<PeerSlot, std::uint64_t> latestReceivedEquipmentRevisions;
        std::deque<AnimationAction> pendingAnimationActions;
        std::unordered_map<PeerSlot, std::uint64_t> latestReceivedActionSequences;
        std::unordered_map<PeerSlot, Multiplayer::PlayerInspection> pendingInspections;
        constexpr std::size_t kMaximumPendingTradeEvents = 256;
        std::deque<Multiplayer::TradeEvent> pendingTradeEvents;
        bool tradeEventsLost = false;
        std::unordered_set<PeerSlot> pendingExits;
        std::unordered_set<PeerSlot> pendingDisconnects;
        PeerSlot localSlot = 0;

        constexpr std::size_t kMaximumPendingChatMessages = 128;
        std::mutex chatMessagesLock;
        std::deque<Multiplayer::ChatMessage> pendingChatMessages;

        std::mutex rosterLock;
        Multiplayer::Roster roster;
        std::atomic_uint64_t rosterRevision{ 0 };
        std::vector<PeerSlot> localPlayers;

        std::mutex requestsLock;
        std::uint32_t lastRequestID = 0;
        std::unordered_map<std::uint32_t, std::function<void(const Wire::Result&)>> pendingRequests;

        std::mutex publishLock;
        std::optional<CheckpointContext> sentContext;
        std::uint64_t localActionSequence = 0;

        std::mutex attemptLock;
        std::string attemptEndpoint;
        std::string attemptDisplayName;
        std::optional<Refusal> refusal;
        std::string welcomedServer;

        std::mutex vanishLock;
        std::string helloServer;

        std::mutex socketLock;
        std::shared_ptr<ix::WebSocket> socket;

        std::string GetAttemptEndpoint()
        {
            const std::scoped_lock lock{ attemptLock };
            return attemptEndpoint;
        }

        std::string ToWebSocketUrl(std::string a_endpoint)
        {
            if (a_endpoint.starts_with("https://"))
            {
                a_endpoint.replace(0, 5, "wss");
            }
            else if (a_endpoint.starts_with("http://"))
            {
                a_endpoint.replace(0, 4, "ws");
            }
            const auto authority = a_endpoint.find("://");
            const auto path = authority == std::string::npos ? std::string::npos : a_endpoint.find('/', authority + 3);
            if (path == std::string::npos)
            {
                return a_endpoint + "/ws";
            }
            if (a_endpoint.size() == path + 1)
            {
                return a_endpoint + "ws";
            }
            return a_endpoint;
        }

        std::shared_ptr<ix::WebSocket> GetSocket()
        {
            const std::scoped_lock lock{ socketLock };
            return socket;
        }

        void Send(const std::string& a_message)
        {
            if (a_message.empty())
            {
                logs::error("Could not encode a message for the server");
                return;
            }
            if (const auto current = GetSocket())
            {
                const auto info = current->sendBinary(a_message);
                if (info.success)
                {
                    CountSentBytes(info.wireSize);
                }
            }
        }

        void SendHello(ix::WebSocket& a_socket)
        {
            const auto config = Config::Get();
            const auto server = ToWebSocketUrl(GetAttemptEndpoint());
            const auto token = SessionStore::Find(server);
            {
                const std::scoped_lock lock{ attemptLock };
                attemptDisplayName = config->connection.displayName;
            }
            const std::scoped_lock lock{ vanishLock };
            helloServer = server;
            std::vector<Multiplayer::SessionID> vanished;
            PlayerFilterStore::CopyVanished(server, vanished);
            if (vanished.size() > Multiplayer::kMaximumVanishedSessions)
            {
                logs::warn("Vanished {} players on {}; the server accepts {}, so the rest are shown", vanished.size(),
                    DescribeServer(server), Multiplayer::kMaximumVanishedSessions);
                vanished.resize(Multiplayer::kMaximumVanishedSessions);
            }
            const auto hello
                = Wire::EncodeHello(config->advanced.viewRadius, config->connection.displayName, token, vanished);
            if (hello.empty())
            {
                logs::error("Could not encode the hello for the server");
                return;
            }
            logs::info("Sending hello: displayName=\"{}\" session={} vanished={}", config->connection.displayName,
                token ? "kept" : "none, asking for a new one", vanished.size());
            if (const auto info = a_socket.sendBinary(hello); info.success)
            {
                CountSentBytes(info.wireSize);
            }
        }

        std::optional<Refusal> MakeRefusal(std::uint16_t a_closeCode, const std::string& a_closeReason)
        {
            std::string displayName;
            {
                const std::scoped_lock lock{ attemptLock };
                displayName = attemptDisplayName;
            }
            switch (a_closeCode)
            {
                case kProtocolMismatchCloseCode:
                    return Refusal{ RefusalReason::kProtocolMismatch,
                        std::format("The server runs another protocol version ({}).", a_closeReason) };
                case kDisplayNameTakenCloseCode:
                    return Refusal{ RefusalReason::kDisplayNameTaken,
                        "The display name is already taken by another player." };
                case kDisplayNameInvalidCloseCode:
                    return Refusal{ RefusalReason::kDisplayNameInvalid,
                        "The server does not accept the display name." };
                case kSessionReplacedCloseCode:
                    return Refusal{ RefusalReason::kSessionReplaced,
                        "Your session connected from another game, which closed this connection. Connect again "
                        "to take it back." };
                case kServerFullCloseCode:
                    return Refusal{ RefusalReason::kServerFull, "The server is full." };
                case kTooManyNewSessionsCloseCode:
                    return Refusal{ RefusalReason::kTooManyNewSessions,
                        "Too many new players joined this server from your IP address recently." };
                default:
                    return std::nullopt;
            }
        }

        void ClearRequests()
        {
            const std::scoped_lock lock{ requestsLock };
            pendingRequests.clear();
        }

        std::uint32_t RegisterRequest(std::function<void(const Wire::Result&)> a_onResult)
        {
            const std::scoped_lock lock{ requestsLock };
            if (++lastRequestID == 0)
            {
                ++lastRequestID;
            }
            pendingRequests.insert_or_assign(lastRequestID, std::move(a_onResult));
            return lastRequestID;
        }

        void ForgetRemotePlayersLocked()
        {
            pendingTransforms.clear();
            latestReceivedSequences.clear();
            peerContexts.clear();
            pendingAppearances.clear();
            latestReceivedAppearanceRevisions.clear();
            pendingEquipment.clear();
            latestReceivedEquipmentRevisions.clear();
            pendingAnimationActions.clear();
            latestReceivedActionSequences.clear();
            pendingInspections.clear();
            pendingTradeEvents.clear();
            tradeEventsLost = false;
            pendingExits.clear();
            pendingDisconnects.clear();
        }

        void ResetRoster(PeerSlot a_localSlot)
        {
            const std::scoped_lock lock{ rosterLock };
            roster.localPlayerID = a_localSlot;
            roster.players.clear();
            localPlayers.clear();
            rosterRevision.fetch_add(1, std::memory_order_acq_rel);
        }

        void MarkDisconnected()
        {
            {
                std::scoped_lock lock{ remoteChangesLock };
                connected.store(false, std::memory_order_release);
                attemptFailed.store(true, std::memory_order_release);
                ForgetRemotePlayersLocked();
            }
            ResetRoster(0);
            ClearRequests();
        }

        void ForgetRemoteWorldStateLocked(PeerSlot a_slot)
        {
            pendingTransforms.erase(a_slot);
            latestReceivedSequences.erase(a_slot);
            peerContexts.erase(a_slot);
            latestReceivedActionSequences.erase(a_slot);
            std::erase_if(pendingAnimationActions,
                [a_slot](const AnimationAction& a_action) { return a_action.playerID == a_slot; });
        }

        void AcceptCheckpointLocked(
            PeerSlot a_slot, const QuantizedCheckpoint& a_checkpoint, std::chrono::steady_clock::time_point a_received)
        {
            const auto context = peerContexts.find(a_slot);
            if (context == peerContexts.end())
            {
                logs::debug("Ignored a checkpoint of peer {} before its PeerEnter", a_slot);
                return;
            }
            const auto latest = latestReceivedSequences.find(a_slot);
            if (latest != latestReceivedSequences.end() && a_checkpoint.sequence <= latest->second)
            {
                logs::debug("Ignored stale checkpoint: player={} sequence={} newestSequence={}", a_slot,
                    static_cast<unsigned long long>(a_checkpoint.sequence),
                    static_cast<unsigned long long>(latest->second));
                return;
            }
            latestReceivedSequences.insert_or_assign(a_slot, a_checkpoint.sequence);
            pendingTransforms.insert_or_assign(a_slot,
                RemotePlayerTransform{ .playerID = a_slot,
                    .transform = Dequantize(a_checkpoint, context->second),
                    .received = a_received });
        }

        void AcceptAppearanceLocked(PeerSlot a_slot, Multiplayer::PlayerAppearance&& a_appearance)
        {
            const auto latest = latestReceivedAppearanceRevisions.find(a_slot);
            if (latest != latestReceivedAppearanceRevisions.end() && a_appearance.revision <= latest->second)
            {
                return;
            }
            latestReceivedAppearanceRevisions.insert_or_assign(a_slot, a_appearance.revision);
            pendingAppearances.insert_or_assign(
                a_slot, RemotePlayerAppearance{ .playerID = a_slot, .appearance = std::move(a_appearance) });
        }

        void AcceptEquipmentLocked(PeerSlot a_slot, Multiplayer::PlayerEquipment&& a_equipment)
        {
            const auto latest = latestReceivedEquipmentRevisions.find(a_slot);
            if (latest != latestReceivedEquipmentRevisions.end() && a_equipment.revision <= latest->second)
            {
                return;
            }
            latestReceivedEquipmentRevisions.insert_or_assign(a_slot, a_equipment.revision);
            pendingEquipment.insert_or_assign(
                a_slot, RemotePlayerEquipment{ .playerID = a_slot, .equipment = std::move(a_equipment) });
        }

        void OnWelcome(const Wire::Welcome& a_welcome)
        {
            // Kept even when the connection is closing: the session exists on the server and
            // holds its display name either way.
            auto server = ToWebSocketUrl(GetAttemptEndpoint());
            if (a_welcome.issuedToken)
            {
                SessionStore::Save(server, *a_welcome.issuedToken);
            }
            {
                const std::scoped_lock lock{ attemptLock };
                welcomedServer = std::move(server);
            }
            unsigned long long generation;
            {
                std::scoped_lock lock{ remoteChangesLock };
                if (closeRequested.load(std::memory_order_acquire))
                {
                    logs::info("Server welcomed a connection that is closing; ignoring it");
                    return;
                }
                ForgetRemotePlayersLocked();
                localSlot = a_welcome.slot;
                generation = connectionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
                connected.store(true, std::memory_order_release);
            }
            ResetRoster(a_welcome.slot);
            {
                const std::scoped_lock lock{ publishLock };
                sentContext.reset();
                localActionSequence = 0;
            }
            logs::info(
                "Server welcomed this client as slot {}: session={} ({}) displayName=\"{}\" connectionGeneration={}",
                a_welcome.slot, static_cast<unsigned long long>(a_welcome.sessionID),
                a_welcome.issuedToken ? "new" : "resumed", a_welcome.displayName, generation);
        }

        void OnResult(const Wire::Result& a_result)
        {
            std::function<void(const Wire::Result&)> callback;
            {
                const std::scoped_lock lock{ requestsLock };
                const auto request = pendingRequests.find(a_result.requestID);
                if (request == pendingRequests.end())
                {
                    return;
                }
                callback = std::move(request->second);
                pendingRequests.erase(request);
            }
            callback(a_result);
        }

        void OnPeerStates(const Wire::PeerStates& a_states)
        {
            const auto received = std::chrono::steady_clock::now();
            std::scoped_lock lock{ remoteChangesLock };
            for (const auto& entry : a_states.entries)
            {
                if (entry.slot != localSlot)
                {
                    AcceptCheckpointLocked(entry.slot, entry.checkpoint, received);
                }
            }
        }

        void OnPeerEnter(Wire::PeerEnter&& a_enter)
        {
            logs::debug("Peer {} entered: cell={:08X} worldspace={:08X} sequence={} appearance={} equipment={}",
                a_enter.slot, a_enter.context.cellID, a_enter.context.worldspaceID,
                static_cast<unsigned long long>(a_enter.state.sequence), a_enter.appearance.has_value(),
                a_enter.equipment.has_value());
            std::scoped_lock lock{ remoteChangesLock };
            pendingDisconnects.erase(a_enter.slot);
            ForgetRemoteWorldStateLocked(a_enter.slot);
            if (a_enter.appearance)
            {
                AcceptAppearanceLocked(a_enter.slot, std::move(*a_enter.appearance));
            }
            if (a_enter.equipment)
            {
                AcceptEquipmentLocked(a_enter.slot, std::move(*a_enter.equipment));
            }
            peerContexts.insert_or_assign(a_enter.slot, a_enter.context);
            AcceptCheckpointLocked(a_enter.slot, a_enter.state, std::chrono::steady_clock::now());
        }

        void OnPeerContext(const Wire::PeerContext& a_context)
        {
            std::scoped_lock lock{ remoteChangesLock };
            const auto previous = peerContexts.find(a_context.slot);
            if (previous == peerContexts.end())
            {
                logs::debug("Ignored the context of peer {} before its PeerEnter", a_context.slot);
                return;
            }
            if (a_context.context.graphEpoch < previous->second.graphEpoch)
            {
                logs::warn("Ignored peer context with a falling graph epoch: player={} epoch={} newestEpoch={}",
                    a_context.slot, static_cast<unsigned long long>(a_context.context.graphEpoch),
                    static_cast<unsigned long long>(previous->second.graphEpoch));
                return;
            }
            previous->second = a_context.context;
        }

        void OnPeerLeave(const Wire::PeerLeave& a_leave)
        {
            logs::debug("Peer {} left: reason={}", a_leave.slot, Wire::GetLeaveReasonName(a_leave.reason));
            std::scoped_lock lock{ remoteChangesLock };
            ForgetRemoteWorldStateLocked(a_leave.slot);
            if (a_leave.reason == Wire::LeaveReason::kDisconnected)
            {
                pendingAppearances.erase(a_leave.slot);
                latestReceivedAppearanceRevisions.erase(a_leave.slot);
                pendingEquipment.erase(a_leave.slot);
                latestReceivedEquipmentRevisions.erase(a_leave.slot);
                pendingExits.erase(a_leave.slot);
                pendingDisconnects.insert(a_leave.slot);
                return;
            }
            // Unlike a disconnect, a later message does not cancel the leave: the main-loop
            // pass must drop the old actor before a new checkpoint.
            pendingExits.insert(a_leave.slot);
        }

        void OnPeerAction(AnimationAction&& a_action)
        {
            std::scoped_lock lock{ remoteChangesLock };
            const auto latest = latestReceivedActionSequences.find(a_action.playerID);
            if (latest != latestReceivedActionSequences.end())
            {
                if (a_action.actionSequence <= latest->second)
                {
                    logs::debug("Animation sync ignored stale action: player={} actionSequence={} newestActionSequence={}",
                        a_action.playerID, static_cast<unsigned long long>(a_action.actionSequence),
                        static_cast<unsigned long long>(latest->second));
                    return;
                }
                if (a_action.actionSequence != latest->second + 1)
                {
                    logs::warn("Animation sync rejected action sequence gap: player={} expected={} received={}",
                        a_action.playerID, static_cast<unsigned long long>(latest->second + 1),
                        static_cast<unsigned long long>(a_action.actionSequence));
                    return;
                }
            }

            latestReceivedActionSequences.insert_or_assign(a_action.playerID, a_action.actionSequence);
            logs::debug(
                "Animation sync received attack: player={} action={} power={} direction={} actionSequence={} "
                "graphEpoch={} checkpointSequence={}",
                a_action.playerID, GetAttackActionName(a_action.attack.action), a_action.attack.power,
                GetPowerAttackDirectionName(a_action.attack.direction),
                static_cast<unsigned long long>(a_action.actionSequence),
                static_cast<unsigned long long>(a_action.graphEpoch),
                static_cast<unsigned long long>(a_action.checkpointSequence));
            pendingAnimationActions.push_back(std::move(a_action));
        }

        void OnChatMessage(Multiplayer::ChatMessage&& a_message)
        {
            // Compared here, since the slot belongs to the connection the message arrived on.
            a_message.outgoing = a_message.sender.playerID == localSlot;
            std::scoped_lock lock{ chatMessagesLock };
            if (pendingChatMessages.size() >= kMaximumPendingChatMessages)
            {
                pendingChatMessages.pop_front();
            }
            pendingChatMessages.push_back(std::move(a_message));
        }

        void OnTradeEvent(Multiplayer::TradeEvent&& a_event)
        {
            std::scoped_lock lock{ remoteChangesLock };
            if (auto* invite = std::get_if<Multiplayer::TradeInvite>(&a_event))
            {
                // Compared here, since the slot belongs to the connection the message arrived on.
                invite->outgoing = invite->sender.playerID == localSlot;
            }
            if (pendingTradeEvents.size() >= kMaximumPendingTradeEvents)
            {
                // A trade's messages only make sense in order, so the trade ends instead.
                logs::warn("Dropped {} queued trade messages", pendingTradeEvents.size());
                pendingTradeEvents.clear();
                tradeEventsLost = true;
            }
            pendingTradeEvents.push_back(std::move(a_event));
        }

        void OnRosterChange(Wire::RosterChange&& a_change)
        {
            logs::debug("Roster changed: joined={} left={}", a_change.joined.size(), a_change.left);
            const std::scoped_lock lock{ rosterLock };
            if (a_change.left != 0)
            {
                std::erase_if(roster.players,
                    [&](const Multiplayer::RosterEntry& a_player) { return a_player.playerID == a_change.left; });
                std::erase(localPlayers, a_change.left);
            }
            for (auto& joined : a_change.joined)
            {
                const auto known = std::ranges::find(roster.players, joined.playerID, &Multiplayer::RosterEntry::playerID);
                if (known != roster.players.end())
                {
                    known->name = std::move(joined.name);
                }
                else
                {
                    roster.players.push_back(std::move(joined));
                }
            }
            rosterRevision.fetch_add(1, std::memory_order_acq_rel);
        }

        void OnLocalPlayersChange(const Wire::LocalPlayersChange& a_change)
        {
            logs::debug("Local players changed: joined={} left={}", a_change.joined.size(), a_change.left.size());
            const std::scoped_lock lock{ rosterLock };
            std::erase_if(localPlayers, [&](PeerSlot a_slot) { return std::ranges::contains(a_change.left, a_slot); });
            for (const auto slot : a_change.joined)
            {
                if (const auto at = std::ranges::lower_bound(localPlayers, slot); at == localPlayers.end() || *at != slot)
                {
                    localPlayers.insert(at, slot);
                }
            }
        }

        void Dispatch(Wire::ServerMessage&& a_message)
        {
            if (const auto* welcome = std::get_if<Wire::Welcome>(&a_message))
            {
                OnWelcome(*welcome);
                return;
            }
            if (!connected.load(std::memory_order_acquire))
            {
                return;
            }
            std::visit(
                [](auto&& a_value) {
                    using T = std::decay_t<decltype(a_value)>;
                    if constexpr (std::is_same_v<T, Wire::Result>)
                    {
                        OnResult(a_value);
                    }
                    else if constexpr (std::is_same_v<T, Wire::PeerStates>)
                    {
                        OnPeerStates(a_value);
                    }
                    else if constexpr (std::is_same_v<T, Wire::PeerEnter>)
                    {
                        OnPeerEnter(std::move(a_value));
                    }
                    else if constexpr (std::is_same_v<T, Wire::PeerContext>)
                    {
                        OnPeerContext(a_value);
                    }
                    else if constexpr (std::is_same_v<T, Wire::PeerLeave>)
                    {
                        OnPeerLeave(a_value);
                    }
                    else if constexpr (std::is_same_v<T, AnimationAction>)
                    {
                        OnPeerAction(std::move(a_value));
                    }
                    else if constexpr (std::is_same_v<T, Wire::PeerAppearance>)
                    {
                        std::scoped_lock lock{ remoteChangesLock };
                        AcceptAppearanceLocked(a_value.slot, std::move(a_value.appearance));
                    }
                    else if constexpr (std::is_same_v<T, Wire::PeerEquipment>)
                    {
                        std::scoped_lock lock{ remoteChangesLock };
                        AcceptEquipmentLocked(a_value.slot, std::move(a_value.equipment));
                    }
                    else if constexpr (std::is_same_v<T, Multiplayer::ChatMessage>)
                    {
                        OnChatMessage(std::move(a_value));
                    }
                    else if constexpr (std::is_same_v<T, Wire::RosterChange>)
                    {
                        OnRosterChange(std::move(a_value));
                    }
                    else if constexpr (std::is_same_v<T, Wire::LocalPlayersChange>)
                    {
                        OnLocalPlayersChange(a_value);
                    }
                    else if constexpr (std::is_same_v<T, Multiplayer::PlayerInspection>)
                    {
                        std::scoped_lock lock{ remoteChangesLock };
                        pendingInspections.insert_or_assign(a_value.playerID, std::move(a_value));
                    }
                    else if constexpr (std::is_same_v<T, Multiplayer::TradeEvent>)
                    {
                        OnTradeEvent(std::move(a_value));
                    }
                },
                std::move(a_message));
        }

        void OnSocketMessage(const ix::WebSocketMessagePtr& a_message)
        {
            switch (a_message->type)
            {
                case ix::WebSocketMessageType::Open:
                {
                    PROFILE_THREAD_NAME("Server connection");
                    transportOpen.store(true, std::memory_order_release);
                    logs::info("WebSocket connected.");
                    if (const auto current = GetSocket())
                    {
                        SendHello(*current);
                    }
                    break;
                }
                case ix::WebSocketMessageType::Message:
                {
                    PROFILE_SCOPE("Server message");
                    CountReceivedBytes(a_message->wireSize);
                    if (!a_message->binary)
                    {
                        logs::warn("Ignored a text message from the server");
                        break;
                    }
                    // One decoder per transport thread; the socket has one.
                    static thread_local Wire::Decoder decoder;
                    const auto& data = a_message->str;
                    auto decoded = decoder.Decode(std::as_bytes(std::span{ data.data(), data.size() }));
                    if (!decoded)
                    {
                        logs::warn("Ignored a malformed server message: {}", decoded.error());
                        break;
                    }
                    Dispatch(std::move(*decoded));
                    break;
                }
                case ix::WebSocketMessageType::Close:
                {
                    transportOpen.store(false, std::memory_order_release);
                    const auto& close = a_message->closeInfo;
                    if (auto refused = MakeRefusal(close.code, close.reason))
                    {
                        logs::error("Server refused this client: code={} reason={}", close.code, close.reason);
                        const std::scoped_lock lock{ attemptLock };
                        refusal = std::move(refused);
                    }
                    MarkDisconnected();
                    logs::info("WebSocket closed: code={} reason={} remote={}; forgetting remote players", close.code,
                        close.reason, close.remote);
                    break;
                }
                case ix::WebSocketMessageType::Error:
                    transportOpen.store(false, std::memory_order_release);
                    MarkDisconnected();
                    logs::warn("Could not connect to {}: {} (http status {})", DescribeServer(GetAttemptEndpoint()),
                        a_message->errorInfo.reason, a_message->errorInfo.http_status);
                    break;
                default:
                    break;
            }
        }

        void StopSocket()
        {
            {
                const std::scoped_lock lock{ vanishLock };
                helloServer.clear();
            }
            std::shared_ptr<ix::WebSocket> old;
            {
                const std::scoped_lock lock{ socketLock };
                old = std::exchange(socket, nullptr);
            }
            if (old)
            {
                old->stop();
            }
            transportOpen.store(false, std::memory_order_release);
        }

        void OpenSocket()
        {
            std::string endpoint{ Config::GetServerEndpoint(Config::Get()->connection) };
            {
                const std::scoped_lock lock{ attemptLock };
                attemptEndpoint = endpoint;
            }
            auto next = std::make_shared<ix::WebSocket>();
            next->setUrl(ToWebSocketUrl(endpoint));
            // ConnectionMonitor owns every retry.
            next->disableAutomaticReconnection();
            next->setPerMessageDeflateOptions(ix::WebSocketPerMessageDeflateOptions(false));
            next->setHandshakeTimeout(kHandshakeTimeoutSeconds);
            // IXWebSocket answers the server's pings on its own thread.
            next->setPingInterval(0);
            next->setOnMessageCallback(OnSocketMessage);
            {
                const std::scoped_lock lock{ socketLock };
                socket = next;
            }
            logs::info("Opening WebSocket connection.");
            next->start();
        }
    }

    void Initialize()
    {
        static std::once_flag initializeOnce;
        std::call_once(initializeOnce, [] {
            if (!ix::initNetSystem())
            {
                logs::error("Could not initialize the network system");
            }
        });
    }

    bool IsConnected()
    {
        return connected.load(std::memory_order_acquire);
    }

    bool IsTransportOpen()
    {
        return transportOpen.load(std::memory_order_acquire);
    }

    std::uint64_t GetConnectionGeneration()
    {
        return connectionGeneration.load(std::memory_order_acquire);
    }

    void PublishLocalWorldEnter()
    {
        if (!IsConnected())
        {
            return;
        }
        {
            const std::scoped_lock lock{ publishLock };
            sentContext.reset();
        }
        Send(Wire::EncodeEnter());
    }

    void PublishLocalWorldExit()
    {
        if (IsConnected())
        {
            Send(Wire::EncodeExit());
        }
    }

    void PublishLocalPlayerTransform(const PlayerTransform& a_transform)
    {
        PROFILE_FUNCTION();
        if (!IsConnected())
        {
            return;
        }
        const auto context = GetCheckpointContext(a_transform);
        bool contextChanged = false;
        {
            const std::scoped_lock lock{ publishLock };
            if (sentContext != context)
            {
                sentContext = context;
                contextChanged = true;
            }
        }
        if (contextChanged)
        {
            logs::debug("Publishing context: cell={:08X} worldspace={:08X} graphEpoch={} furniture={}:{:06X}",
                context.cellID, context.worldspaceID, static_cast<unsigned long long>(context.graphEpoch),
                context.furniture.plugin, context.furniture.localFormID);
            Send(Wire::EncodeContext(context));
        }
        Send(Wire::EncodeCheckpoint(Quantize(a_transform)));
    }

    void PublishLocalAppearance(const Multiplayer::PlayerAppearance& a_appearance)
    {
        PROFILE_FUNCTION();
        if (IsConnected() && Multiplayer::IsValidAppearance(a_appearance))
        {
            Send(Wire::EncodeAppearance(a_appearance));
        }
    }

    void PublishLocalEquipment(const Multiplayer::PlayerEquipment& a_equipment)
    {
        PROFILE_FUNCTION();
        if (IsConnected() && Multiplayer::IsValidEquipment(a_equipment))
        {
            Send(Wire::EncodeEquipment(a_equipment));
        }
    }

    void PublishLocalAttack(const Attack& a_attack, std::uint64_t a_graphEpoch, std::uint64_t a_checkpointSequence)
    {
        PROFILE_FUNCTION();
        if (!IsConnected() || a_graphEpoch == 0 || a_checkpointSequence == 0)
        {
            return;
        }
        std::uint64_t actionSequence;
        {
            const std::scoped_lock lock{ publishLock };
            actionSequence = ++localActionSequence;
        }
        logs::debug(
            "Animation sync publishing attack: action={} power={} direction={} actionSequence={} graphEpoch={} "
            "checkpointSequence={}",
            GetAttackActionName(a_attack.action), a_attack.power, GetPowerAttackDirectionName(a_attack.direction),
            static_cast<unsigned long long>(actionSequence), static_cast<unsigned long long>(a_graphEpoch),
            static_cast<unsigned long long>(a_checkpointSequence));
        Send(Wire::EncodeAction(actionSequence, a_graphEpoch, a_checkpointSequence, a_attack));
    }

    bool PublishChatMessage(Multiplayer::ChatChannel a_channel, PeerSlot a_recipient, std::string_view a_text,
        std::function<void(std::string_view)> a_onRejected)
    {
        if (!IsConnected())
        {
            return false;
        }
        const auto requestID = RegisterRequest([onRejected = std::move(a_onRejected)](const Wire::Result& a_result) {
            if (a_result.ok)
            {
                return;
            }
            logs::info("Server rejected chat message: {}", a_result.error.empty() ? "<no reason>" : a_result.error);
            onRejected(a_result.error);
        });
        Send(Wire::EncodeChat(requestID, a_channel, a_recipient, a_text));
        return true;
    }

    void PublishVanish(std::string_view a_server, Multiplayer::SessionID a_session, bool a_vanished)
    {
        const std::scoped_lock lock{ vanishLock };
        if (!helloServer.empty() && helloServer == a_server)
        {
            Send(Wire::EncodeVanish(a_session, a_vanished));
        }
    }

    bool RequestInspection(PeerSlot a_playerID)
    {
        if (!IsConnected())
        {
            return false;
        }
        Send(Wire::EncodeInspect(a_playerID));
        return true;
    }

    bool PublishTradeRequest(PeerSlot a_playerID)
    {
        if (!IsConnected())
        {
            return false;
        }
        Send(Wire::EncodeTradeRequest(a_playerID));
        return true;
    }

    void PublishTradeAnswer(Multiplayer::TradeID a_trade, bool a_accept)
    {
        if (IsConnected())
        {
            Send(Wire::EncodeTradeAnswer(a_trade, a_accept));
        }
    }

    void PublishTradeOffer(const Multiplayer::TradeOffer& a_offer)
    {
        if (IsConnected())
        {
            Send(Wire::EncodeTradeOffer(a_offer));
        }
    }

    void PublishTradeReport(const Multiplayer::TradeReport& a_report)
    {
        if (IsConnected())
        {
            Send(Wire::EncodeTradeReport(a_report));
        }
    }

    void PublishTradeAccept(const Multiplayer::TradeAcceptance& a_acceptance)
    {
        if (IsConnected())
        {
            Send(Wire::EncodeTradeAccept(a_acceptance));
        }
    }

    void PublishTradeCancel(Multiplayer::TradeID a_trade, Multiplayer::TradeCloseReason a_reason)
    {
        if (IsConnected())
        {
            Send(Wire::EncodeTradeCancel(a_trade, a_reason));
        }
    }

    std::string GetServerAddress()
    {
        {
            const std::scoped_lock lock{ attemptLock };
            if (!welcomedServer.empty())
            {
                return welcomedServer;
            }
        }
        const auto config = Config::Get();
        const auto endpoint = Config::GetServerEndpoint(config->connection);
        return endpoint.empty() ? std::string{} : ToWebSocketUrl(std::string{ endpoint });
    }

    std::string_view DescribeServer(std::string_view a_server)
    {
        static const auto official = ToWebSocketUrl(std::string{ Config::GetDefaultServerEndpoint() });
        return a_server == Config::GetDefaultServerEndpoint() || a_server == official ? "the official server"
                                                                                      : a_server;
    }

    void EmitLatencyProbe(std::function<void()> a_onAcknowledged)
    {
        if (!IsConnected())
        {
            return;
        }
        const auto requestID
            = RegisterRequest([onAcknowledged = std::move(a_onAcknowledged)](const Wire::Result&) { onAcknowledged(); });
        Send(Wire::EncodePing(requestID));
    }

    void AbandonConnection()
    {
        logs::warn("Abandoning the connection to {}; forgetting remote players", DescribeServer(GetAttemptEndpoint()));
        MarkDisconnected();
    }

    bool BeginReconnect()
    {
        if (reconnectWorkerRunning.exchange(true, std::memory_order_acq_rel))
        {
            return false;
        }
        closeRequested.store(false, std::memory_order_release);
        {
            const std::scoped_lock lock{ attemptLock };
            refusal.reset();
        }

        // Stopping a socket joins its thread and may wait on its close handshake.
        std::thread([] {
            try
            {
                StopSocket();
                attemptFailed.store(false, std::memory_order_release);
                OpenSocket();
            }
            catch (const std::exception& a_exception)
            {
                logs::error("Reconnect attempt threw: {}", a_exception.what());
                attemptFailed.store(true, std::memory_order_release);
            }
            reconnectWorkerRunning.store(false, std::memory_order_release);
        }).detach();
        return true;
    }

    bool BeginClose()
    {
        closeRequested.store(true, std::memory_order_release);
        MarkDisconnected();
        if (reconnectWorkerRunning.exchange(true, std::memory_order_acq_rel))
        {
            return false;
        }

        std::thread([] {
            try
            {
                StopSocket();
                logs::info("Server connection closed on request");
            }
            catch (const std::exception& a_exception)
            {
                logs::error("Close threw: {}", a_exception.what());
            }
            reconnectWorkerRunning.store(false, std::memory_order_release);
        }).detach();
        return true;
    }

    bool DidReconnectAttemptFail()
    {
        return !reconnectWorkerRunning.load(std::memory_order_acquire) && attemptFailed.load(std::memory_order_acquire);
    }

    std::optional<Refusal> GetRefusal()
    {
        const std::scoped_lock lock{ attemptLock };
        return refusal;
    }

    RemotePlayerChanges DrainRemotePlayerChanges()
    {
        PROFILE_FUNCTION();
        RemotePlayerChanges changes;
        std::scoped_lock lock{ remoteChangesLock };

        // An abandoned transport may still deliver messages until it is replaced.
        if (!connected.load(std::memory_order_acquire))
        {
            ForgetRemotePlayersLocked();
        }

        changes.transforms.reserve(pendingTransforms.size());
        for (auto& entry : pendingTransforms)
        {
            changes.transforms.push_back(std::move(entry.second));
        }
        pendingTransforms.clear();

        changes.appearances.reserve(pendingAppearances.size());
        for (auto& entry : pendingAppearances)
        {
            changes.appearances.push_back(std::move(entry.second));
        }
        pendingAppearances.clear();

        changes.equipment.reserve(pendingEquipment.size());
        for (auto& entry : pendingEquipment)
        {
            changes.equipment.push_back(std::move(entry.second));
        }
        pendingEquipment.clear();

        changes.animationActions.reserve(pendingAnimationActions.size());
        while (!pendingAnimationActions.empty())
        {
            changes.animationActions.push_back(std::move(pendingAnimationActions.front()));
            pendingAnimationActions.pop_front();
        }

        changes.inspections.reserve(pendingInspections.size());
        for (auto& entry : pendingInspections)
        {
            changes.inspections.push_back(std::move(entry.second));
        }
        pendingInspections.clear();

        changes.tradeEvents.reserve(pendingTradeEvents.size());
        for (auto& event : pendingTradeEvents)
        {
            changes.tradeEvents.push_back(std::move(event));
        }
        pendingTradeEvents.clear();
        changes.tradeEventsLost = std::exchange(tradeEventsLost, false);

        changes.exitedPlayerIDs.assign(pendingExits.begin(), pendingExits.end());
        pendingExits.clear();
        changes.disconnectedPlayerIDs.assign(pendingDisconnects.begin(), pendingDisconnects.end());
        pendingDisconnects.clear();

        changes.connectionGeneration = connectionGeneration.load(std::memory_order_acquire);
        changes.connected = connected.load(std::memory_order_acquire);
        return changes;
    }

    std::vector<Multiplayer::ChatMessage> DrainChatMessages()
    {
        std::vector<Multiplayer::ChatMessage> messages;
        std::scoped_lock lock{ chatMessagesLock };
        messages.reserve(pendingChatMessages.size());
        for (auto& message : pendingChatMessages)
        {
            messages.push_back(std::move(message));
        }
        pendingChatMessages.clear();
        return messages;
    }

    void CopyNearbyPlayers(std::vector<PeerSlot>& a_players)
    {
        const std::scoped_lock lock{ rosterLock };
        a_players.insert(a_players.end(), localPlayers.begin(), localPlayers.end());
    }

    bool CopyRosterIfChanged(std::uint64_t& a_revision, Multiplayer::Roster& a_roster)
    {
        if (rosterRevision.load(std::memory_order_acquire) == a_revision)
        {
            return false;
        }
        const std::scoped_lock lock{ rosterLock };
        a_revision = rosterRevision.load(std::memory_order_acquire);
        a_roster = roster;
        return true;
    }
}
