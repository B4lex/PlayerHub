#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/ChatMessage.h"
#include "protocol/EquipmentState.h"
#include "protocol/PlayerState.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Network
{
    struct RemotePlayerTransform
    {
        std::string playerID;
        PlayerTransform transform;
    };

    struct RemotePlayerAppearance
    {
        std::string playerID;
        Multiplayer::PlayerAppearance appearance;
    };

    struct RemotePlayerEquipment
    {
        std::string playerID;
        Multiplayer::PlayerEquipment equipment;
    };

    struct RemotePlayerChanges
    {
        std::vector<RemotePlayerTransform> transforms;
        // Only the newest pending appearance revision per player.
        std::vector<RemotePlayerAppearance> appearances;
        // Only the newest pending equipment revision per player.
        std::vector<RemotePlayerEquipment> equipment;
        std::vector<AnimationAction> animationActions;
        std::vector<RemoteMountTransition> mountTransitions;
        // Players that left the world. Their checkpoint and actions are
        // forgotten, while appearance and equipment remain valid. Checkpoints and
        // actions above for the same player arrived after the exit.
        std::vector<std::string> exitedPlayerIDs;
        std::vector<std::string> disconnectedPlayerIDs;
        // Connection state when these changes were drained. Remote player IDs
        // are the server's per-connection sids, so while disconnected, or once
        // the generation advances, every previously known remote player is gone.
        // Receive history is reset with each connection change, so the changes
        // above always belong to this generation.
        std::uint64_t connectionGeneration = 0;
        bool connected = false;
    };

    // Prepares the Socket.IO client without connecting. Safe to call more than
    // once. The client never connects or retries by itself; ConnectionMonitor
    // opens every connection through BeginReconnect.
    void Initialize();

    [[nodiscard]] bool IsConnected();

    // Whether the Engine.IO transport is open. It outlasts IsConnected while a
    // close is in progress. Safe to call from any thread.
    [[nodiscard]] bool IsTransportOpen();

    // Advances every time the default namespace opens, including automatic
    // reconnects. The server forgets per-connection state, so latest-wins
    // publishers republish whenever this changes.
    [[nodiscard]] std::uint64_t GetConnectionGeneration();

    // The server accepts checkpoints only between player:enter and player:exit,
    // and forgets the checkpoint and action stream on exit. Both are ordered
    // with the checkpoint stream; safe to call from any thread.
    void PublishLocalWorldEnter();
    void PublishLocalWorldExit();

    // Called from the game thread. socket.io-client-cpp queues the emit for its
    // transport thread; no Skyrim objects cross the network boundary.
    void PublishLocalPlayerTransform(const PlayerTransform& a_transform);

    // Publishes the latest-wins appearance state. The caller owns revisions.
    void PublishLocalAppearance(const Multiplayer::PlayerAppearance& a_appearance);

    // Publishes the latest-wins equipped items. The caller owns revisions.
    void PublishLocalEquipment(const Multiplayer::PlayerEquipment& a_equipment);

    // Publishes one item in the ordered semantic action stream: the attack's hand
    // and, for a power attack, its direction. Graph event names never cross it.
    void PublishLocalAttack(const Attack& a_attack, std::uint64_t a_graphEpoch, std::uint64_t a_checkpointSequence);

    // Publishes the start of an animated mount or dismount, anchored to the
    // checkpoint published in the same pass.
    void PublishLocalMountTransition(
        MountTransition a_transition, std::uint64_t a_graphEpoch, std::uint64_t a_checkpointSequence);

    // Emits chat:send with already trimmed, valid text. Returns false without
    // sending while disconnected. a_onRejected runs on the transport thread with
    // the server's reason if it refuses the message; accepted messages come back
    // through DrainChatMessages like everyone else's.
    [[nodiscard]] bool PublishChatMessage(
        std::string_view a_text, std::function<void(std::string_view)> a_onRejected);

    // Emits latency:probe. a_onAcknowledged runs on the transport thread when the
    // server acknowledges it; it never runs if the connection drops first.
    void EmitLatencyProbe(std::function<void()> a_onAcknowledged);

    // Declares the current connection dead because the server stopped
    // answering. Behaves as if the transport had reported the loss: the client
    // is disconnected and every remote player is forgotten. Main-loop thread.
    void AbandonConnection();

    // Closes whatever is left of the old transport and opens a new one on a
    // worker thread, whether the loss was ours, the server's, or a failed
    // attempt. Returns false while a previous BeginReconnect or BeginClose is
    // still working.
    [[nodiscard]] bool BeginReconnect();

    // Reports the client disconnected at once, forgetting every remote player,
    // and closes the connection or pending attempt on a worker thread. Until the
    // next BeginReconnect, a namespace that still opens is not reported as
    // connected. Returns false while a previous BeginReconnect or BeginClose is
    // still working; the caller retries. Main-loop thread.
    [[nodiscard]] bool BeginClose();

    // Whether the latest attempt went to an endpoint other than the configured
    // one. False before the first attempt.
    [[nodiscard]] bool IsEndpointOutdated();

    // Whether the latest attempt from BeginReconnect has been
    // handed to the transport and then failed or closed before the namespace
    // opened.
    [[nodiscard]] bool DidReconnectAttemptFail();

    // Transfers network-thread updates to the caller. The authoritative
    // main-loop frame hook is the only consumer.
    [[nodiscard]] RemotePlayerChanges DrainRemotePlayerChanges();

    // Transfers received chat messages, oldest first. Unlike player state, they
    // outlive the connection they arrived on. Safe to call from any thread.
    [[nodiscard]] std::vector<Multiplayer::ChatMessage> DrainChatMessages();
}
