#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/ChatMessage.h"
#include "protocol/EquipmentState.h"
#include "protocol/PeerSlot.h"
#include "protocol/PlayerState.h"
#include "protocol/Quantization.h"
#include "protocol/Session.h"

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

// Protobuf encoding of the wire messages (proto/skyrimmp/v1) to and from the
// plain values in src/protocol. Decoding validates every field as the server
// does, so nothing invalid reaches the queues.
namespace Network::Wire
{
    // Without a token the server issues a new session under a_nickname.
    // Otherwise the token's session is resumed and takes a_nickname.
    [[nodiscard]] std::string EncodeHello(std::uint32_t a_viewRadius, std::string_view a_nickname,
        const std::optional<Multiplayer::SessionToken>& a_token);
    [[nodiscard]] std::string EncodeEnter();
    [[nodiscard]] std::string EncodeExit();
    [[nodiscard]] std::string EncodeContext(const CheckpointContext& a_context);
    [[nodiscard]] std::string EncodeCheckpoint(const QuantizedCheckpoint& a_checkpoint);
    [[nodiscard]] std::string EncodeAction(
        std::uint64_t a_actionSequence, std::uint64_t a_graphEpoch, std::uint64_t a_checkpointSequence, const Attack& a_attack);
    // Empty when the value cannot be encoded, which validation prevents.
    [[nodiscard]] std::string EncodeAppearance(const Multiplayer::PlayerAppearance& a_appearance);
    [[nodiscard]] std::string EncodeEquipment(const Multiplayer::PlayerEquipment& a_equipment);
    [[nodiscard]] std::string EncodeChat(
        std::uint32_t a_requestID, Multiplayer::ChatChannel a_channel, std::string_view a_text);
    [[nodiscard]] std::string EncodePing(std::uint32_t a_requestID);
    [[nodiscard]] std::string EncodeSetNickname(std::uint32_t a_requestID, std::string_view a_nickname);

    struct Welcome
    {
        Multiplayer::PeerSlot slot{ 0 };
        Multiplayer::SessionID sessionID{ 0 };
        // The session's nickname, as peers see it.
        std::string nickname;
        // Only when the server issued the session for this connection.
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
        // Present only when newer than what this client holds.
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

    using ServerMessage = std::variant<Welcome, Result, PeerStates, PeerEnter, PeerContext, PeerLeave, AnimationAction,
        PeerAppearance, PeerEquipment, Multiplayer::ChatMessage>;

    // Decodes server messages with one reused buffer; the generated message
    // struct is large. Not thread-safe: one per transport thread.
    class Decoder
    {
    public:
        Decoder();
        ~Decoder();
        Decoder(const Decoder&) = delete;
        Decoder& operator=(const Decoder&) = delete;

        // The message, or what was rejected in it.
        [[nodiscard]] std::expected<ServerMessage, std::string> Decode(std::span<const std::byte> a_data);

    private:
        struct Buffer;
        std::unique_ptr<Buffer> buffer;
    };

    // Bounds the work one untrusted batch can cause.
    inline constexpr std::size_t kMaximumPeerStatesEntries = 4096;
}
