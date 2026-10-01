#pragma once

#include "network/Refusal.h"
#include "protocol/AppearanceState.h"
#include "protocol/ChatMessage.h"
#include "protocol/EquipmentState.h"
#include "protocol/PeerSlot.h"
#include "protocol/PlayerState.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Network
{
    struct RemotePlayerTransform
    {
        Multiplayer::PeerSlot playerID{ 0 };
        // Merged with the peer's context at the time it arrived.
        PlayerTransform transform;
        // When the transport thread decoded it, for the playout clock.
        std::chrono::steady_clock::time_point received{};
    };

    struct RemotePlayerAppearance
    {
        Multiplayer::PeerSlot playerID{ 0 };
        Multiplayer::PlayerAppearance appearance;
    };

    struct RemotePlayerEquipment
    {
        Multiplayer::PeerSlot playerID{ 0 };
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
        // Peers this client no longer receives: out of range, excluded, or out
        // of the world. Their checkpoint and actions are forgotten, while
        // appearance and equipment remain valid. Checkpoints and actions above
        // for the same player arrived after the leave.
        std::vector<Multiplayer::PeerSlot> exitedPlayerIDs;
        // Peers whose connection closed: forget everything.
        std::vector<Multiplayer::PeerSlot> disconnectedPlayerIDs;
        // Connection state when these changes were drained. Slots are
        // per-connection, so while disconnected, or once the generation
        // advances, every previously known remote player is gone. Receive
        // history is reset with each connection change, so the changes above
        // always belong to this generation.
        std::uint64_t connectionGeneration = 0;
        bool connected = false;
    };

    // Prepares the client without connecting. Safe to call more than once. The
    // client never connects or retries by itself; ConnectionMonitor opens every
    // connection through BeginReconnect.
    void Initialize();

    // Whether the server welcomed this connection. Every hello presents the
    // session token kept for the server (SessionStore) and [General] DisplayName,
    // and the welcome names the session it resumed or issued.
    [[nodiscard]] bool IsConnected();

    // Whether the WebSocket is open. It outlasts IsConnected while a close is
    // in progress. Safe to call from any thread.
    [[nodiscard]] bool IsTransportOpen();

    // Advances every time the server welcomes a connection. The server forgets
    // per-connection state, so latest-wins publishers republish whenever this
    // changes.
    [[nodiscard]] std::uint64_t GetConnectionGeneration();

    // The server accepts context and checkpoints only between Enter and Exit,
    // and forgets the checkpoint and action stream on exit. Both are ordered
    // with the checkpoint stream; safe to call from any thread.
    void PublishLocalWorldEnter();
    void PublishLocalWorldExit();

    // Called from the game thread. Sends the checkpoint's context first when it
    // differs from the last one sent since entering the world, then the
    // quantized checkpoint.
    void PublishLocalPlayerTransform(const PlayerTransform& a_transform);

    // Publishes the latest-wins appearance state. The caller owns revisions.
    void PublishLocalAppearance(const Multiplayer::PlayerAppearance& a_appearance);

    // Publishes the latest-wins equipped items. The caller owns revisions.
    void PublishLocalEquipment(const Multiplayer::PlayerEquipment& a_equipment);

    // Publishes one item in the ordered semantic action stream: the attack's hand
    // and, for a power attack, its direction. Graph event names never cross it.
    void PublishLocalAttack(const Attack& a_attack, std::uint64_t a_graphEpoch, std::uint64_t a_checkpointSequence);

    // Sends a chat line with already trimmed, valid text; a whisper's text starts
    // with the recipient's name. Returns false without sending while
    // disconnected. a_onRejected runs on the transport thread with the server's
    // reason if it refuses the message; accepted messages come back through
    // DrainChatMessages like everyone else's.
    [[nodiscard]] bool PublishChatMessage(Multiplayer::ChatChannel a_channel, std::string_view a_text,
        std::function<void(std::string_view)> a_onRejected);

    // Sends a Ping. a_onAcknowledged runs on the transport thread when the
    // server answers it; it never runs if the connection drops first.
    void EmitLatencyProbe(std::function<void()> a_onAcknowledged);

    // Declares the current connection dead because the server stopped
    // answering. Behaves as if the transport had reported the loss: the client
    // is disconnected and every remote player is forgotten. Main-loop thread.
    void AbandonConnection();

    // Closes whatever is left of the old transport and opens a new one on a
    // worker thread, whether the loss was ours, the server's, or a failed
    // attempt. Returns false while a previous BeginReconnect or BeginClose is
    // still working. Clears a refusal.
    [[nodiscard]] bool BeginReconnect();

    // Reports the client disconnected at once, forgetting every remote player,
    // and closes the connection or pending attempt on a worker thread. Until the
    // next BeginReconnect, a connection that still opens is not reported as
    // connected. Returns false while a previous BeginReconnect or BeginClose is
    // still working; the caller retries. Main-loop thread.
    [[nodiscard]] bool BeginClose();

    // Whether the latest attempt went to an endpoint other than the configured
    // one. False before the first attempt.
    [[nodiscard]] bool IsEndpointOutdated();

    // Whether the latest attempt from BeginReconnect has been handed to the
    // transport and then failed or closed before the server welcomed it.
    [[nodiscard]] bool DidReconnectAttemptFail();

    // Why the server refused the latest connection for good; retrying cannot
    // help until what the reason names changes. Empty otherwise. Safe to call
    // from any thread.
    [[nodiscard]] std::optional<Refusal> GetRefusal();

    // Transfers network-thread updates to the caller. The authoritative
    // main-loop frame hook is the only consumer.
    [[nodiscard]] RemotePlayerChanges DrainRemotePlayerChanges();

    // Transfers received chat messages, oldest first. Unlike player state, they
    // outlive the connection they arrived on. Safe to call from any thread.
    [[nodiscard]] std::vector<Multiplayer::ChatMessage> DrainChatMessages();
}
