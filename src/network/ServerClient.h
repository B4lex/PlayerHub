#pragma once

#include "network/Refusal.h"
#include "protocol/AppearanceState.h"
#include "protocol/ChatMessage.h"
#include "protocol/EquipmentState.h"
#include "protocol/PeerSlot.h"
#include "protocol/PlayerState.h"
#include "protocol/Session.h"

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
        PlayerTransform transform;
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
        std::vector<RemotePlayerAppearance> appearances;
        std::vector<RemotePlayerEquipment> equipment;
        std::vector<AnimationAction> animationActions;
        std::vector<Multiplayer::PlayerInspection> inspections;
        std::vector<Multiplayer::PeerSlot> exitedPlayerIDs;
        std::vector<Multiplayer::PeerSlot> disconnectedPlayerIDs;
        std::uint64_t connectionGeneration = 0;
        bool connected = false;
    };

    void Initialize();

    [[nodiscard]] bool IsConnected();

    [[nodiscard]] bool IsTransportOpen();

    [[nodiscard]] std::uint64_t GetConnectionGeneration();

    void PublishLocalWorldEnter();
    void PublishLocalWorldExit();

    void PublishLocalPlayerTransform(const PlayerTransform& a_transform);

    void PublishLocalAppearance(const Multiplayer::PlayerAppearance& a_appearance);

    void PublishLocalEquipment(const Multiplayer::PlayerEquipment& a_equipment);

    void PublishLocalAttack(const Attack& a_attack, std::uint64_t a_graphEpoch, std::uint64_t a_checkpointSequence);

    [[nodiscard]] bool PublishChatMessage(Multiplayer::ChatChannel a_channel, Multiplayer::PeerSlot a_recipient,
        std::string_view a_text, std::function<void(std::string_view)> a_onRejected);

    void PublishVanish(std::string_view a_server, Multiplayer::SessionID a_session, bool a_vanished);

    [[nodiscard]] bool RequestInspection(Multiplayer::PeerSlot a_playerID);

    [[nodiscard]] std::string GetServerAddress();

    [[nodiscard]] std::string_view DescribeServer(std::string_view a_server);

    void EmitLatencyProbe(std::function<void()> a_onAcknowledged);

    void AbandonConnection();

    [[nodiscard]] bool BeginReconnect();

    [[nodiscard]] bool BeginClose();

    [[nodiscard]] bool IsEndpointOutdated();

    [[nodiscard]] bool DidReconnectAttemptFail();

    [[nodiscard]] std::optional<Refusal> GetRefusal();

    [[nodiscard]] RemotePlayerChanges DrainRemotePlayerChanges();

    [[nodiscard]] std::vector<Multiplayer::ChatMessage> DrainChatMessages();

    void CopyNearbyPlayers(std::vector<Multiplayer::PeerSlot>& a_players);

    [[nodiscard]] bool CopyRosterIfChanged(std::uint64_t& a_revision, Multiplayer::Roster& a_roster);
}
