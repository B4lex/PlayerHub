#pragma once

#include "protocol/ChatMessage.h"
#include "protocol/TradeState.h"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
        kPluginQuery,
        kPluginReply,
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
        // The peer's queried plugins for kPluginQuery; the plugins the peer lacks for kPluginReply.
        std::vector<std::string> plugins;
    };

    static constexpr float kRequestTimeoutSeconds = 10.0F;
    static constexpr float kInviteSeconds = 30.0F;
    // The server expires an invitation; this only ends one whose close was lost.
    static constexpr float kInviteGraceSeconds = 5.0F;
    // Edits are sent together this long after the first one not yet sent.
    static constexpr float kOfferBatchSeconds = 0.5F;
    // Rows wait this long for the peer's plugin reply, then the offer's report checks them.
    static constexpr float kPluginCheckSeconds = 5.0F;

    [[nodiscard]] Phase GetPhase() const noexcept { return phase; }
    [[nodiscard]] Multiplayer::TradeID GetID() const noexcept { return id; }
    [[nodiscard]] const Multiplayer::PlayerIdentity& GetPeer() const noexcept { return peer; }
    [[nodiscard]] float GetWaited() const noexcept { return waited; }
    [[nodiscard]] bool IsActive() const noexcept { return phase != Phase::kIdle; }

    [[nodiscard]] const std::vector<Multiplayer::TradeItem>& GetPeerItems() const noexcept { return peerItems; }
    [[nodiscard]] std::uint64_t GetPeerRevision() const noexcept { return peerRevision; }
    // The peer's report on the shown own offer, once it covers it.
    [[nodiscard]] const std::vector<Multiplayer::TradeSkip>* FindOwnSkips() const;
    // The own revision the peer's latest report covers, or null before one.
    [[nodiscard]] const std::vector<Multiplayer::TradeItem>* FindReportedOffer() const;
    [[nodiscard]] const std::vector<Multiplayer::TradeSkip>& GetReportedSkips() const noexcept { return ownSkips; }
    [[nodiscard]] bool IsOwnAccepted() const noexcept { return ownAccepted; }
    [[nodiscard]] bool IsPeerAccepted() const noexcept { return peerAccepted; }
    [[nodiscard]] Blocker GetAcceptBlocker() const;
    // Whether the plugin is in an unanswered own query that has not yet waited kPluginCheckSeconds.
    [[nodiscard]] bool IsCheckingPlugin(std::string_view a_plugin) const;

    bool BeginRequest(const Multiplayer::PlayerIdentity& a_peer);
    EventResult OnEvent(const Multiplayer::TradeEvent& a_event);
    // Ends the trade locally, as a close from the server would.
    void Reset();

    // Returns whether the offer changed.
    bool SetOwnItems(std::vector<Multiplayer::TradeItem> a_items);
    // Returns the report to send on the peer's current offer.
    [[nodiscard]] Multiplayer::TradeReport ReportPeerOffer(std::vector<Multiplayer::TradeSkip> a_skips);
    [[nodiscard]] std::optional<Multiplayer::TradeAcceptance> Accept();
    // Returns the queries to send for the plugins not asked about yet in this trade.
    [[nodiscard]] std::vector<Multiplayer::TradePluginQuery> QueryPlugins(std::span<const std::string> a_plugins);

    // Advances timers; returns the offer to publish once its batch is due, a timed-out phase
    // to end, or the plugin check running out.
    struct Tick
    {
        std::optional<Multiplayer::TradeOffer> offer;
        bool timedOut{ false };
        bool pluginCheckExpired{ false };
    };
    [[nodiscard]] Tick Advance(float a_seconds);

private:
    EventResult Close(Multiplayer::TradeCloseReason a_reason);

    Phase phase{ Phase::kIdle };
    Multiplayer::TradeID id{ 0 };
    Multiplayer::PlayerIdentity peer;
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

    std::vector<std::string> askedPlugins;
    // Unanswered own queries, oldest first: the server relays the replies in order.
    std::deque<std::vector<std::string>> pluginQueries;
    float pluginWait{ 0.0F };
    bool pluginCheckExpired{ false };

    bool ownAccepted{ false };
    bool peerAccepted{ false };
};
