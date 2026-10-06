#include "sync/trade/TradeSession.h"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>

namespace
{
    using Multiplayer::TradeCloseReason;

    // A commit names a revision at most a round trip old, which a quiet edit
    // interval of 0.2 s keeps well inside this many.
    constexpr std::size_t kKeptOfferRevisions = 16;

    bool IsSkipped(const std::vector<Multiplayer::TradeSkip>& a_skips, std::size_t a_index)
    {
        return std::ranges::any_of(a_skips, [a_index](const auto& a_skip) { return a_skip.index == a_index; });
    }
}

const std::vector<Multiplayer::TradeSkip>* TradeSession::FindOwnSkips() const
{
    if (ownDirty)
    {
        return nullptr;
    }
    if (ownRevision == 0)
    {
        static const std::vector<Multiplayer::TradeSkip> none;
        return &none;
    }
    return ownReportRevision == ownRevision ? &ownSkips : nullptr;
}

TradeSession::Blocker TradeSession::GetAcceptBlocker() const
{
    if (phase != Phase::kOpen)
    {
        return Blocker::kNotOpen;
    }
    if (ownAccepted)
    {
        return Blocker::kAccepted;
    }
    if (ownItems.empty() && peerItems.empty())
    {
        return Blocker::kEmpty;
    }
    if (ownDirty)
    {
        return Blocker::kSending;
    }
    if (!FindOwnSkips() || reportedPeerRevision != peerRevision)
    {
        return Blocker::kAwaitingReport;
    }
    return Blocker::kNone;
}

bool TradeSession::BeginRequest(const Multiplayer::RosterEntry& a_peer)
{
    if (phase != Phase::kIdle)
    {
        return false;
    }
    Reset();
    phase = Phase::kRequesting;
    peer = a_peer;
    return true;
}

void TradeSession::Reset()
{
    *this = TradeSession{};
}

TradeSession::EventResult TradeSession::Close(TradeCloseReason a_reason)
{
    EventResult result{ .outcome = Outcome::kClosed, .reason = a_reason, .closedPhase = phase };
    const auto closedPeer = std::move(peer);
    Reset();
    // Kept for the caller's notice.
    peer = closedPeer;
    return result;
}

TradeSession::EventResult TradeSession::OnEvent(const Multiplayer::TradeEvent& a_event)
{
    return std::visit(
        [this](const auto& a_value) -> EventResult {
            using T = std::decay_t<decltype(a_value)>;
            if constexpr (std::is_same_v<T, Multiplayer::TradeInvite>)
            {
                if (a_value.outgoing)
                {
                    if (phase != Phase::kRequesting || a_value.recipient.playerID != peer.playerID)
                    {
                        return {};
                    }
                    phase = Phase::kInviting;
                    id = a_value.id;
                    waited = 0.0F;
                    return { .outcome = Outcome::kInviteSent };
                }
                // A request of the local player's that crossed this invitation is refused
                // by the server, and that refusal is then ignored.
                if (phase != Phase::kIdle && phase != Phase::kRequesting)
                {
                    return {};
                }
                Reset();
                phase = Phase::kInvited;
                id = a_value.id;
                peer = Multiplayer::RosterEntry{ .playerID = a_value.sender.playerID,
                    .name = a_value.sender.name,
                    .session = a_value.sender.session };
                return { .outcome = Outcome::kInvited };
            }
            else if constexpr (std::is_same_v<T, Multiplayer::TradeClose>)
            {
                if (a_value.id == 0 ? phase != Phase::kRequesting : a_value.id != id || phase == Phase::kIdle)
                {
                    return {};
                }
                return Close(a_value.reason);
            }
            else
            {
                if (a_value.id != id || id == 0)
                {
                    return {};
                }
                if constexpr (std::is_same_v<T, Multiplayer::TradeOpen>)
                {
                    if (phase != Phase::kInviting && phase != Phase::kInvited)
                    {
                        return {};
                    }
                    phase = Phase::kOpen;
                    return { .outcome = Outcome::kOpened };
                }
                if (phase != Phase::kOpen)
                {
                    return {};
                }
                if constexpr (std::is_same_v<T, Multiplayer::TradeOffer>)
                {
                    if (a_value.revision <= peerRevision)
                    {
                        return {};
                    }
                    peerRevision = a_value.revision;
                    peerItems = a_value.items;
                    peerSkips.clear();
                    ownAccepted = false;
                    peerAccepted = false;
                    return { .outcome = Outcome::kPeerOffer };
                }
                else if constexpr (std::is_same_v<T, Multiplayer::TradeReport>)
                {
                    if (!sentOffers.contains(a_value.revision))
                    {
                        return {};
                    }
                    ownReportRevision = a_value.revision;
                    ownSkips = a_value.skipped;
                    return { .outcome = Outcome::kReport };
                }
                else if constexpr (std::is_same_v<T, Multiplayer::TradeAcceptance>)
                {
                    // Sent as the peer names them: its own offer first.
                    if (a_value.ownRevision != peerRevision || a_value.peerRevision != ownRevision || ownDirty)
                    {
                        return {};
                    }
                    peerAccepted = true;
                    return { .outcome = Outcome::kPeerAccepted };
                }
                else if constexpr (std::is_same_v<T, Multiplayer::TradeCommit>)
                {
                    Commit commit;
                    if (a_value.ownRevision != 0)
                    {
                        const auto sent = sentOffers.find(a_value.ownRevision);
                        if (sent != sentOffers.end() && ownReportRevision == a_value.ownRevision)
                        {
                            for (std::size_t i = 0; i < sent->second.size(); ++i)
                            {
                                if (!IsSkipped(ownSkips, i))
                                {
                                    commit.given.push_back(sent->second[i]);
                                }
                            }
                        }
                        else
                        {
                            // Without the peer's report it is unknown what the peer takes;
                            // keeping the items loses nothing.
                            logs::error("Trade {} committed own revision {} without its report; keeping the items",
                                id, static_cast<unsigned long long>(a_value.ownRevision));
                        }
                    }
                    if (a_value.peerRevision == peerRevision && reportedPeerRevision == peerRevision)
                    {
                        for (std::size_t i = 0; i < peerItems.size(); ++i)
                        {
                            if (!IsSkipped(peerSkips, i))
                            {
                                commit.received.push_back(peerItems[i]);
                                commit.receivedIndices.push_back(static_cast<std::uint8_t>(i));
                            }
                        }
                    }
                    else if (a_value.peerRevision != 0)
                    {
                        logs::error("Trade {} committed peer revision {}, but this client holds {}", id,
                            static_cast<unsigned long long>(a_value.peerRevision),
                            static_cast<unsigned long long>(peerRevision));
                    }
                    auto result = Close(TradeCloseReason::kCancelled);
                    result.outcome = Outcome::kCommitted;
                    result.commit = std::move(commit);
                    return result;
                }
                return {};
            }
        },
        a_event);
}

bool TradeSession::SetOwnItems(std::vector<Multiplayer::TradeItem> a_items)
{
    if (phase != Phase::kOpen || a_items == ownItems)
    {
        return false;
    }
    ownItems = std::move(a_items);
    ownDirty = true;
    sinceEdit = 0.0F;
    ownAccepted = false;
    peerAccepted = false;
    return true;
}

Multiplayer::TradeReport TradeSession::ReportPeerOffer(std::vector<Multiplayer::TradeSkip> a_skips)
{
    peerSkips = std::move(a_skips);
    reportedPeerRevision = peerRevision;
    return Multiplayer::TradeReport{ .id = id, .revision = peerRevision, .skipped = peerSkips };
}

std::optional<Multiplayer::TradeAcceptance> TradeSession::Accept()
{
    if (GetAcceptBlocker() != Blocker::kNone)
    {
        return std::nullopt;
    }
    ownAccepted = true;
    return Multiplayer::TradeAcceptance{ .id = id, .ownRevision = ownRevision, .peerRevision = peerRevision };
}

TradeSession::Tick TradeSession::Advance(float a_seconds)
{
    Tick tick;
    waited += a_seconds;
    switch (phase)
    {
        case Phase::kRequesting:
            tick.timedOut = waited > kRequestTimeoutSeconds;
            break;
        case Phase::kInviting:
        case Phase::kInvited:
            tick.timedOut = waited > kInviteSeconds + kInviteGraceSeconds;
            break;
        case Phase::kOpen:
            sinceEdit += a_seconds;
            if (ownDirty && sinceEdit >= kOfferQuietSeconds)
            {
                ownDirty = false;
                ++ownRevision;
                sentOffers.insert_or_assign(ownRevision, ownItems);
                while (sentOffers.size() > kKeptOfferRevisions)
                {
                    sentOffers.erase(sentOffers.begin());
                }
                tick.offer = Multiplayer::TradeOffer{ .id = id, .revision = ownRevision, .items = ownItems };
            }
            break;
        default:
            break;
    }
    return tick;
}
