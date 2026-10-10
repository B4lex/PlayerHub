#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/ChatMessage.h"
#include "protocol/EquipmentState.h"
#include "protocol/PeerSlot.h"
#include "protocol/PlayerState.h"
#include "protocol/Quantization.h"
#include "protocol/Session.h"
#include "protocol/TradeState.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Network::Wire
{
    [[nodiscard]] std::string EncodeHello(std::uint32_t a_viewRadius, std::string_view a_displayName,
        const std::optional<Multiplayer::SessionToken>& a_token, std::span<const Multiplayer::SessionID> a_vanished);
    [[nodiscard]] std::string EncodeEnter();
    [[nodiscard]] std::string EncodeExit();
    [[nodiscard]] std::string EncodeContext(const CheckpointContext& a_context);
    [[nodiscard]] std::string EncodeCheckpoint(const QuantizedCheckpoint& a_checkpoint);
    [[nodiscard]] std::string EncodeAction(
        std::uint64_t a_actionSequence, std::uint64_t a_graphEpoch, std::uint64_t a_checkpointSequence, const Attack& a_attack);
    [[nodiscard]] std::string EncodeAppearance(const Multiplayer::PlayerAppearance& a_appearance);
    [[nodiscard]] std::string EncodeEquipment(const Multiplayer::PlayerEquipment& a_equipment);
    [[nodiscard]] std::string EncodeChat(std::uint32_t a_requestID, Multiplayer::ChatChannel a_channel,
        Multiplayer::PeerSlot a_recipient, std::string_view a_text);
    [[nodiscard]] std::string EncodePing(std::uint32_t a_requestID);
    [[nodiscard]] std::string EncodeVanish(Multiplayer::SessionID a_session, bool a_vanished);
    [[nodiscard]] std::string EncodeInspect(Multiplayer::PeerSlot a_slot);
    [[nodiscard]] std::string EncodeTradeRequest(Multiplayer::PeerSlot a_slot);
    [[nodiscard]] std::string EncodeTradeAnswer(Multiplayer::TradeID a_trade, bool a_accept);
    [[nodiscard]] std::string EncodeTradeOffer(const Multiplayer::TradeOffer& a_offer);
    [[nodiscard]] std::string EncodeTradeReport(const Multiplayer::TradeReport& a_report);
    [[nodiscard]] std::string EncodeTradeAccept(const Multiplayer::TradeAcceptance& a_acceptance);
    [[nodiscard]] std::string EncodeTradeCancel(Multiplayer::TradeID a_trade, Multiplayer::TradeCloseReason a_reason);

    struct Welcome
    {
        Multiplayer::PeerSlot slot{ 0 };
        Multiplayer::SessionID sessionID{ 0 };
        std::string displayName;
        std::optional<Multiplayer::SessionToken> issuedToken;
    };

    struct Result
    {
        std::uint32_t requestID{ 0 };
        bool ok{ false };
        std::string error;
    };

    struct PeerEntry
    {
        Multiplayer::PeerSlot slot{ 0 };
        QuantizedCheckpoint checkpoint;
    };

    struct PeerStates
    {
        std::vector<PeerEntry> entries;
    };

    struct PeerEnter
    {
        Multiplayer::PeerSlot slot{ 0 };
        std::optional<Multiplayer::PlayerAppearance> appearance;
        std::optional<Multiplayer::PlayerEquipment> equipment;
        CheckpointContext context;
        QuantizedCheckpoint state;
    };

    struct PeerContext
    {
        Multiplayer::PeerSlot slot{ 0 };
        CheckpointContext context;
    };

    enum class LeaveReason : std::uint8_t
    {
        kOutOfRange,
        kExcluded,
        kExited,
        kDisconnected,
        kViewerUnplaced
    };

    [[nodiscard]] constexpr const char* GetLeaveReasonName(LeaveReason a_reason) noexcept
    {
        switch (a_reason)
        {
            case LeaveReason::kOutOfRange:
                return "out of range";
            case LeaveReason::kExcluded:
                return "excluded";
            case LeaveReason::kExited:
                return "exited";
            case LeaveReason::kDisconnected:
                return "disconnected";
            case LeaveReason::kViewerUnplaced:
                return "viewer unplaced";
        }
        return "unknown";
    }

    struct PeerLeave
    {
        Multiplayer::PeerSlot slot{ 0 };
        LeaveReason reason{ LeaveReason::kExited };
    };

    struct PeerAppearance
    {
        Multiplayer::PeerSlot slot{ 0 };
        Multiplayer::PlayerAppearance appearance;
    };

    struct PeerEquipment
    {
        Multiplayer::PeerSlot slot{ 0 };
        Multiplayer::PlayerEquipment equipment;
    };

    struct RosterChange
    {
        std::vector<Multiplayer::RosterEntry> joined;
        Multiplayer::PeerSlot left{ 0 };
    };

    struct LocalPlayersChange
    {
        std::vector<Multiplayer::PeerSlot> joined;
        std::vector<Multiplayer::PeerSlot> left;
    };

    using ServerMessage = std::variant<Welcome, Result, PeerStates, PeerEnter, PeerContext, PeerLeave, AnimationAction,
        PeerAppearance, PeerEquipment, Multiplayer::ChatMessage, RosterChange, LocalPlayersChange,
        Multiplayer::PlayerInspection, Multiplayer::TradeEvent>;

    class Decoder
    {
    public:
        Decoder();
        ~Decoder();
        Decoder(const Decoder&) = delete;
        Decoder& operator=(const Decoder&) = delete;

        [[nodiscard]] std::expected<ServerMessage, std::string> Decode(std::span<const std::byte> a_data);

    private:
        struct Buffer;
        std::unique_ptr<Buffer> buffer;
    };

    inline constexpr std::size_t kMaximumPeerStatesEntries = 4096;
    inline constexpr std::size_t kMaximumRosterEntries = 4096;
}
