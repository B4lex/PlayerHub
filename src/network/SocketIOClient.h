#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/EquipmentState.h"
#include "protocol/PlayerState.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Network
{
    struct RemotePlayerTransform
    {
        std::string playerID;
        PlayerTransform transform;
        // Server-clock milliseconds when the source emitted this checkpoint.
        // Empty when the source had no clock estimate yet.
        std::optional<double> sentAtServerMilliseconds;
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
        std::vector<std::string> disconnectedPlayerIDs;
        // Connection state when these changes were drained. Remote player IDs
        // are the server's per-connection sids, so while disconnected, or once
        // the generation advances, every previously known remote player is gone.
        // Receive history is reset with each connection change, so the changes
        // above always belong to this generation.
        std::uint64_t connectionGeneration = 0;
        bool connected = false;
    };

    // Starts the Socket.IO client. Safe to call more than once.
    void Initialize();

    [[nodiscard]] bool IsConnected();

    // Advances every time the default namespace opens, including automatic
    // reconnects. The server forgets per-connection state, so latest-wins
    // publishers republish whenever this changes.
    [[nodiscard]] std::uint64_t GetConnectionGeneration();

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

    // Emits latency:probe. a_onAcknowledged runs on the transport thread when the
    // server acknowledges it, with the server clock in milliseconds when the ack
    // carried one; it never runs if the connection drops first.
    void EmitLatencyProbe(std::function<void(std::optional<double> a_serverTimeMilliseconds)> a_onAcknowledged);

    // Transfers network-thread updates to the caller. The authoritative
    // main-loop frame hook is the only consumer.
    [[nodiscard]] RemotePlayerChanges DrainRemotePlayerChanges();
}
