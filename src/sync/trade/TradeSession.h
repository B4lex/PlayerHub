#pragma once

#include "protocol/ChatMessage.h"
#include "protocol/TradeState.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

class TradeSession
{
public:
    enum class Phase : std::uint8_t
    {
        kIdle,
        kRequesting,
        kInviting,
        kInvited,
        kOpen
    };

    enum class Blocker : std::uint8_t
    {
        kNone,
        kNotOpen,
        kEmpty,
        kSending,
        kAwaitingReport,
        kAccepted
    };

    struct Commit
    {
        std::vector<Multiplayer::TradeItem> given;
        std::vector<Multiplayer::TradeItem> received;
        // The peer's offer indices of `received`, for resolving them again.
        std::vector<std::uint8_t> receivedIndices;
    };

    enum class Outcome : std::uint8_t
    {
        kIgnored,
        kInviteSent,
        kInvited,
        kOpened,
        kPeerOffer,
        kReport,
        kPeerAccepted,
        kCommitted,
        kClosed
    };

    struct EventResult
    {
        Outcome outcome{ Outcome::kIgnored };
        Multiplayer::TradeCloseReason reason{ Multiplayer::TradeCloseReason::kCancelled };
        // The phase before a close.
        Phase closedPhase{ Phase::kIdle };
        std::optional<Commit> commit;
    };

    static constexpr float kRequestTimeoutSeconds = 10.0F;
    static constexpr float kInviteSeconds = 30.0F;
    // The server expires an invitation; this only ends one whose close was lost.
    static constexpr float kInviteGraceSeconds = 5.0F;
    static constexpr float kOfferQuietSeconds = 0.2F;

    [[nodiscard]] Phase GetPhase() const noexcept { return phase; }
    [[nodiscard]] Multiplayer::TradeID GetID() const noexcept { return id; }
    [[nodiscard]] const Multiplayer::RosterEntry& GetPeer() const noexcept { return peer; }
    [[nodiscard]] float GetWaited() const noexcept { return waited; }
    [[nodiscard]] bool IsActive() const noexcept { return phase != Phase::kIdle; }

    [[nodiscard]] const std::vector<Multiplayer::TradeItem>& GetOwnItems() const noexcept { return ownItems; }
    [[nodiscard]] const std::vector<Multiplayer::TradeItem>& GetPeerItems() const noexcept { return peerItems; }
    [[nodiscard]] std::uint64_t GetPeerRevision() const noexcept { return peerRevision; }
    // The peer's report on the shown own offer, once it covers it.
    [[nodiscard]] const std::vector<Multiplayer::TradeSkip>* FindOwnSkips() const;
    [[nodiscard]] const std::vector<Multiplayer::TradeSkip>& GetPeerSkips() const noexcept { return peerSkips; }
    [[nodiscard]] bool IsOwnAccepted() const noexcept { return ownAccepted; }
    [[nodiscard]] bool IsPeerAccepted() const noexcept { return peerAccepted; }
    [[nodiscard]] Blocker GetAcceptBlocker() const;

    bool BeginRequest(const Multiplayer::RosterEntry& a_peer);
    EventResult OnEvent(const Multiplayer::TradeEvent& a_event);
    // Ends the trade locally, as a close from the server would.
    void Reset();

    // Returns whether the offer changed.
    bool SetOwnItems(std::vector<Multiplayer::TradeItem> a_items);
    // Returns the report to send on the peer's current offer.
    [[nodiscard]] Multiplayer::TradeReport ReportPeerOffer(std::vector<Multiplayer::TradeSkip> a_skips);
    [[nodiscard]] std::optional<Multiplayer::TradeAcceptance> Accept();

    // Advances timers; returns the offer to publish once edits are quiet, or a timed-out
    // phase to end.
    struct Tick
    {
        std::optional<Multiplayer::TradeOffer> offer;
        bool timedOut{ false };
    };
    [[nodiscard]] Tick Advance(float a_seconds);

private:
    EventResult Close(Multiplayer::TradeCloseReason a_reason);

    Phase phase{ Phase::kIdle };
    Multiplayer::TradeID id{ 0 };
    Multiplayer::RosterEntry peer;
    float waited{ 0.0F };

    std::vector<Multiplayer::TradeItem> ownItems;
    bool ownDirty{ false };
    float sinceEdit{ 0.0F };
    std::uint64_t ownRevision{ 0 };
    // Every sent revision until the trade ends: a commit may name one the server took
    // before a newer one arrived.
    std::map<std::uint64_t, std::vector<Multiplayer::TradeItem>> sentOffers;
    std::uint64_t ownReportRevision{ 0 };
    std::vector<Multiplayer::TradeSkip> ownSkips;

    std::uint64_t peerRevision{ 0 };
    std::vector<Multiplayer::TradeItem> peerItems;
    std::vector<Multiplayer::TradeSkip> peerSkips;
    std::uint64_t reportedPeerRevision{ 0 };

    bool ownAccepted{ false };
    bool peerAccepted{ false };
};
