#include "sync/trade/TradeSync.h"
#include "forms/FormLookup.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"
#include "sync/filters/PlayerFilters.h"
#include "sync/notices/Notices.h"
#include "sync/trade/TradeInventory.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <numeric>
#include <utility>
#include <variant>

namespace TradeSync
{
    namespace
    {
        using Multiplayer::TradeCloseReason;
        using Phase = TradeSession::Phase;

        constexpr float kMaximumFrameSeconds = 0.1F;
        constexpr std::size_t kMaximumQueuedEvents = 256;

        struct Command
        {
            enum class Kind : std::uint8_t
            {
                kRequest,
                kAnswer,
                kCancel,
                kSetCount,
                kAccept
            };

            Kind kind{ Kind::kRequest };
            Multiplayer::RosterEntry player;
            bool accept{ false };
            TradeCloseReason reason{ TradeCloseReason::kCancelled };
            std::uint64_t inventoryRevision{ 0 };
            std::size_t row{ 0 };
            std::uint32_t count{ 0 };
        };

        // Overlay thread to game thread.
        std::mutex s_commandsLock;
        std::vector<Command> s_commands;

        // Game thread to overlay thread.
        std::mutex s_viewLock;
        View s_view;
        std::uint64_t s_viewRevision = 1;
        std::atomic<Phase> s_phase{ Phase::kIdle };
        std::atomic<Multiplayer::PeerSlot> s_invitingPlayer{ 0 };

        // Set by the hit event sink on any thread while a trade is active.
        std::atomic_bool s_hit{ false };

        // Game thread only.
        TradeSession s_session;
        std::vector<Multiplayer::TradeEvent> s_events;
        bool s_eventsLost = false;
        bool s_connected = false;
        std::uint64_t s_connectionGeneration = 0;
        std::vector<TradeInventory::Row> s_inventory;
        std::uint64_t s_inventoryRevision = 0;
        std::vector<OfferLine> s_offer;
        std::vector<PeerLine> s_peerOffer;
        std::uint32_t s_secondsLeft = 0;
        bool s_viewDirty = false;

        class HitEventSink final : public RE::BSTEventSink<RE::TESHitEvent>
        {
        public:
            static HitEventSink* GetSingleton()
            {
                static HitEventSink singleton;
                return std::addressof(singleton);
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
            {
                if (s_phase.load(std::memory_order_acquire) != Phase::kIdle && a_event
                    && a_event->target.get() == RE::PlayerCharacter::GetSingleton())
                {
                    s_hit.store(true, std::memory_order_release);
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            HitEventSink() = default;
        };

        void PushCommand(Command a_command)
        {
            const std::scoped_lock lock{ s_commandsLock };
            s_commands.push_back(std::move(a_command));
        }

        const std::string& PeerName()
        {
            return s_session.GetPeer().name;
        }

        const char* DescribeBlock(Forms::OfferBlock a_block)
        {
            switch (a_block)
            {
                case Forms::OfferBlock::kQuestItem:
                    return "A quest needs it.";
                case Forms::OfferBlock::kPlayerMade:
                    return "Made by you, so other games do not have it.";
                case Forms::OfferBlock::kUnreferenceable:
                    break;
            }
            return "It cannot be described to another game.";
        }

        std::string DescribeSkip(const Multiplayer::TradeSkip& a_skip, std::string_view a_who, bool a_local)
        {
            const auto lacks = a_local ? std::string{ "You do not have " } : std::string{ a_who } + " does not have ";
            switch (a_skip.reason)
            {
                case Multiplayer::TradeSkipReason::kMissingItem:
                    return a_skip.plugin.empty() ? lacks + "this item." : lacks + a_skip.plugin + ".";
                case Multiplayer::TradeSkipReason::kMissingEffect:
                    return a_skip.plugin.empty() ? lacks + "this item's enchantment."
                                                 : lacks + a_skip.plugin + ", which its enchantment needs.";
                case Multiplayer::TradeSkipReason::kWrongType:
                    return a_local ? std::string{ "In your game this is not an item that can be traded." }
                                   : std::string{ "In " } + std::string{ a_who } + "'s game this is another kind of form.";
                case Multiplayer::TradeSkipReason::kBadEnchantment:
                    break;
            }
            return a_local ? std::string{ "Your game cannot rebuild its enchantment." }
                           : std::string{ a_who } + "'s game cannot rebuild its enchantment.";
        }

        std::string FormatItems(const std::vector<std::string>& a_names)
        {
            if (a_names.empty())
            {
                return "nothing";
            }
            return std::accumulate(std::next(a_names.begin()), a_names.end(), a_names.front(),
                [](std::string a_left, const std::string& a_right) { return std::move(a_left) + ", " + a_right; });
        }

        void ClearTrade()
        {
            s_session.Reset();
            s_inventory.clear();
            s_offer.clear();
            s_peerOffer.clear();
            s_hit.store(false, std::memory_order_release);
            s_viewDirty = true;
        }

        void PublishView()
        {
            const auto blocker = s_session.GetAcceptBlocker();
            View view{ .phase = s_session.GetPhase(),
                .peer = s_session.GetPeer(),
                .secondsLeft = s_secondsLeft,
                .inventoryRevision = s_inventoryRevision,
                .offer = s_offer,
                .peerOffer = s_peerOffer,
                .ownAccepted = s_session.IsOwnAccepted(),
                .peerAccepted = s_session.IsPeerAccepted() };
            switch (blocker)
            {
                case TradeSession::Blocker::kEmpty:
                    view.acceptBlocker = "Add an item, or wait for " + PeerName() + " to add one.";
                    break;
                case TradeSession::Blocker::kSending:
                    view.acceptBlocker = "Sending your offer...";
                    break;
                case TradeSession::Blocker::kAwaitingReport:
                    view.acceptBlocker = "Waiting for " + PeerName() + "'s game to check the offers...";
                    break;
                default:
                    break;
            }
            view.inventory.reserve(s_inventory.size());
            for (const auto& row : s_inventory)
            {
                view.inventory.push_back(InventoryLine{ .name = row.name,
                    .count = row.item.count,
                    .formID = row.formID,
                    .category = row.category,
                    .weight = row.weight,
                    .equipped = row.equipped,
                    .favorited = row.favorited,
                    .enchanted = row.item.IsEnchanted(),
                    .blocked = row.block ? DescribeBlock(*row.block) : std::string{} });
            }
            s_phase.store(view.phase, std::memory_order_release);
            s_invitingPlayer.store(view.phase == Phase::kInvited ? view.peer.playerID : 0, std::memory_order_release);
            const std::scoped_lock lock{ s_viewLock };
            s_view = std::move(view);
            ++s_viewRevision;
        }

        void CaptureInventory(RE::PlayerCharacter& a_player)
        {
            s_inventory = TradeInventory::Capture(a_player);
            ++s_inventoryRevision;
            s_offer.clear();
            s_viewDirty = true;
            logs::debug("Trade captured the inventory: rows={}", s_inventory.size());
        }

        std::vector<Multiplayer::TradeItem> BuildOwnItems()
        {
            std::vector<Multiplayer::TradeItem> items;
            items.reserve(s_offer.size());
            for (const auto& line : s_offer)
            {
                auto item = s_inventory[line.row].item;
                item.count = line.count;
                items.push_back(std::move(item));
            }
            return items;
        }

        void RefreshOwnSkips()
        {
            const auto* skips = s_session.FindOwnSkips();
            for (std::size_t i = 0; i < s_offer.size(); ++i)
            {
                auto& line = s_offer[i];
                line.skipped.clear();
                if (!skips)
                {
                    continue;
                }
                const auto skip = std::ranges::find(*skips, i, &Multiplayer::TradeSkip::index);
                if (skip != skips->end())
                {
                    line.skipped = DescribeSkip(*skip, PeerName(), false);
                }
            }
            s_viewDirty = true;
        }

        void SetOfferCount(const Command& a_command)
        {
            if (s_session.GetPhase() != Phase::kOpen || a_command.inventoryRevision != s_inventoryRevision
                || a_command.row >= s_inventory.size() || s_inventory[a_command.row].block)
            {
                return;
            }
            const auto& row = s_inventory[a_command.row];
            const auto count = (std::min)(a_command.count, row.item.count);
            auto offer = s_offer;
            const auto line = std::ranges::find(offer, a_command.row, &OfferLine::row);
            if (count == 0)
            {
                if (line == offer.end())
                {
                    return;
                }
                offer.erase(line);
            }
            else if (line != offer.end())
            {
                line->count = count;
            }
            else
            {
                if (offer.size() >= Multiplayer::kMaximumTradeItems)
                {
                    Notices::Post(std::format("An offer holds at most {} items.", Multiplayer::kMaximumTradeItems));
                    return;
                }
                offer.push_back(OfferLine{ .row = a_command.row, .count = count });
            }
            const auto bytes = std::accumulate(offer.begin(), offer.end(), std::size_t{ 0 },
                [](std::size_t a_sum, const OfferLine& a_line) {
                    return a_sum + Multiplayer::EstimateTradeItemBytes(s_inventory[a_line.row].item);
                });
            if (bytes > Multiplayer::kMaximumTradeOfferBytes)
            {
                Notices::Post("Your offer is full; these items carry too much data to send together.");
                return;
            }
            s_offer = std::move(offer);
            s_session.SetOwnItems(BuildOwnItems());
            RefreshOwnSkips();
        }

        // Drops offered items the player no longer holds, which a script may have taken.
        bool KeepHeldItems(RE::PlayerCharacter& a_player)
        {
            std::vector<std::string> missing;
            for (const auto& line : s_offer)
            {
                auto item = s_inventory[line.row].item;
                item.count = line.count;
                if (TradeInventory::Count(a_player, item) < line.count)
                {
                    missing.push_back(s_inventory[line.row].name);
                }
            }
            if (missing.empty())
            {
                return true;
            }
            Notices::Post("You no longer have " + FormatItems(missing) + "; the trade shows what you hold now.");
            CaptureInventory(a_player);
            s_session.SetOwnItems({});
            return false;
        }

        void CancelTrade(TradeCloseReason a_reason, std::string_view a_notice)
        {
            const auto phase = s_session.GetPhase();
            const auto id = s_session.GetID();
            logs::info("Trade {} with {} ended here: reason={} phase={}", id, PeerName(),
                Multiplayer::GetTradeCloseReasonName(a_reason), static_cast<int>(phase));
            if (phase == Phase::kInvited)
            {
                Network::PublishTradeAnswer(id, false);
            }
            else if (phase == Phase::kInviting || phase == Phase::kOpen)
            {
                Network::PublishTradeCancel(id, a_reason);
            }
            if (!a_notice.empty())
            {
                Notices::Post(std::string{ a_notice });
            }
            ClearTrade();
        }

        void ApplyCommit(RE::PlayerCharacter& a_player, const TradeSession::Commit& a_commit, std::string_view a_peer)
        {
            std::size_t given = 0;
            for (const auto& item : a_commit.given)
            {
                auto* object = FormLookup::ResolveForm<RE::TESBoundObject>(item.item);
                const auto name = object ? Forms::GetDisplayName(*object, item) : item.item.plugin;
                const auto removed = TradeInventory::Remove(a_player, item);
                if (removed < item.count)
                {
                    logs::warn("Trade gave {} of {} {} ({}|{:06X}); the rest was gone", removed, item.count, name,
                        item.item.plugin, item.item.localFormID);
                }
                if (removed > 0)
                {
                    ++given;
                }
            }
            std::size_t received = 0;
            for (const auto& item : a_commit.received)
            {
                // Resolved again: the forms resolved at the offer are not kept across passes.
                auto resolved = Forms::ResolveItem(item);
                if (!resolved)
                {
                    logs::error("Trade could not resolve a received item it reported as resolvable: {}|{:06X}",
                        item.item.plugin, item.item.localFormID);
                    continue;
                }
                const auto added = TradeInventory::Add(a_player, *resolved, item);
                const auto name = Forms::GetDisplayName(*resolved->object, item);
                if (added < item.count)
                {
                    logs::warn("Trade received {} of {} {} ({}|{:06X})", added, item.count, name, item.item.plugin,
                        item.item.localFormID);
                }
                if (added > 0)
                {
                    ++received;
                }
            }
            logs::info("Trade with {} completed: gave {} entries, received {} entries", a_peer, given, received);
            Notices::Post("You successfully traded.");
        }

        void ReportPeerOffer()
        {
            std::vector<Multiplayer::TradeSkip> skips;
            s_peerOffer.clear();
            const auto& items = s_session.GetPeerItems();
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                const auto& item = items[i];
                PeerLine line{ .count = item.count, .enchanted = item.IsEnchanted() };
                auto resolved = Forms::ResolveItem(item);
                if (resolved)
                {
                    line.name = Forms::GetDisplayName(*resolved->object, item);
                    line.formID = resolved->object->GetFormID();
                }
                else
                {
                    auto skip = resolved.error();
                    skip.index = static_cast<std::uint8_t>(i);
                    line.name = item.name.empty() ? std::format("{}|0x{:06X}", item.item.plugin, item.item.localFormID)
                                                  : item.name;
                    line.skipped = DescribeSkip(skip, PeerName(), true);
                    logs::info("Trade cannot receive {}|{:06X}: reason={} plugin={}", item.item.plugin,
                        item.item.localFormID, static_cast<int>(skip.reason), skip.plugin);
                    skips.push_back(std::move(skip));
                }
                s_peerOffer.push_back(std::move(line));
            }
            Network::PublishTradeReport(s_session.ReportPeerOffer(std::move(skips)));
            s_viewDirty = true;
        }

        void OnClosed(const TradeSession::EventResult& a_result, const std::string& a_peer)
        {
            const bool requested = a_result.closedPhase == Phase::kRequesting;
            const bool inviting = a_result.closedPhase == Phase::kInviting;
            const bool invited = a_result.closedPhase == Phase::kInvited;
            logs::info("Trade with {} closed by the server: reason={} phase={}", a_peer,
                Multiplayer::GetTradeCloseReasonName(a_result.reason), static_cast<int>(a_result.closedPhase));
            switch (a_result.reason)
            {
                case TradeCloseReason::kBusy:
                    Notices::Post(a_peer + " is busy with another trade.");
                    break;
                case TradeCloseReason::kUnavailable:
                    Notices::Post(a_peer + " cannot trade right now.");
                    break;
                case TradeCloseReason::kOutOfRange:
                    Notices::Post(requested ? a_peer + " is too far away to trade."
                                            : "The trade with " + a_peer + " ended: you are too far apart.");
                    break;
                case TradeCloseReason::kDeclined:
                    Notices::Post(a_peer + " declined your trade offer.");
                    break;
                case TradeCloseReason::kExpired:
                    Notices::Post(inviting ? a_peer + " did not answer your trade offer."
                                           : "The trade offer from " + a_peer + " expired.");
                    break;
                case TradeCloseReason::kCombat:
                    Notices::Post("The trade with " + a_peer + " ended: they are in combat.");
                    break;
                case TradeCloseReason::kMenu:
                case TradeCloseReason::kCancelled:
                    Notices::Post(invited ? a_peer + " withdrew the trade offer." : a_peer + " cancelled the trade.");
                    break;
                case TradeCloseReason::kDisconnected:
                    Notices::Post(a_peer + " left; the trade ended.");
                    break;
                case TradeCloseReason::kFailed:
                    Notices::Post("The trade with " + a_peer + " could not open.");
                    break;
            }
        }

        void HandleEvent(RE::PlayerCharacter& a_player, const Multiplayer::TradeEvent& a_event)
        {
            const auto peer = PeerName();
            auto result = s_session.OnEvent(a_event);
            switch (result.outcome)
            {
                case TradeSession::Outcome::kIgnored:
                    logs::debug("Trade ignored a message for another trade or phase: type={}", a_event.index());
                    return;
                case TradeSession::Outcome::kInviteSent:
                    logs::info("Trade {} offered to {}", s_session.GetID(), PeerName());
                    break;
                case TradeSession::Outcome::kInvited:
                {
                    const auto& sender = s_session.GetPeer();
                    if (PlayerFilters::IsMuted(sender.session))
                    {
                        // Unanswered, so the server expires it as for a player who ignored it.
                        logs::info("Trade {} from muted player {} ignored", s_session.GetID(), sender.name);
                        ClearTrade();
                        return;
                    }
                    logs::info("Trade {} offered by {}", s_session.GetID(), sender.name);
                    break;
                }
                case TradeSession::Outcome::kOpened:
                    logs::info("Trade {} with {} opened", s_session.GetID(), PeerName());
                    CaptureInventory(a_player);
                    break;
                case TradeSession::Outcome::kPeerOffer:
                    logs::debug("Trade {} received offer revision {} with {} items", s_session.GetID(),
                        static_cast<unsigned long long>(s_session.GetPeerRevision()), s_session.GetPeerItems().size());
                    ReportPeerOffer();
                    break;
                case TradeSession::Outcome::kReport:
                    RefreshOwnSkips();
                    break;
                case TradeSession::Outcome::kPeerAccepted:
                    logs::debug("Trade {}: {} accepted", s_session.GetID(), PeerName());
                    break;
                case TradeSession::Outcome::kCommitted:
                    ApplyCommit(a_player, *result.commit, peer);
                    ClearTrade();
                    break;
                case TradeSession::Outcome::kClosed:
                    OnClosed(result, s_session.GetPeer().name);
                    ClearTrade();
                    break;
            }
            s_viewDirty = true;
        }

        bool IsInCombat(RE::PlayerCharacter& a_player)
        {
            return a_player.IsInCombat() || s_hit.exchange(false, std::memory_order_acq_rel);
        }

        void HandleCommand(RE::PlayerCharacter& a_player, bool a_inWorld, const Command& a_command)
        {
            const auto phase = s_session.GetPhase();
            switch (a_command.kind)
            {
                case Command::Kind::kRequest:
                    if (phase != Phase::kIdle)
                    {
                        Notices::Post("Finish your current trade first.");
                        return;
                    }
                    if (!a_inWorld || a_player.IsInCombat())
                    {
                        Notices::Post("You cannot trade in combat or away from the world.");
                        return;
                    }
                    if (!Network::PublishTradeRequest(a_command.player.playerID))
                    {
                        Notices::Post("Not connected to the server.");
                        return;
                    }
                    s_session.BeginRequest(a_command.player);
                    logs::info("Trade requested with {} (slot {})", a_command.player.name, a_command.player.playerID);
                    break;
                case Command::Kind::kAnswer:
                    if (phase != Phase::kInvited)
                    {
                        return;
                    }
                    if (!a_command.accept)
                    {
                        CancelTrade(TradeCloseReason::kDeclined, "You declined the trade offer from " + PeerName() + ".");
                        return;
                    }
                    if (!a_inWorld || a_player.IsInCombat())
                    {
                        Notices::Post("You cannot trade in combat or away from the world.");
                        return;
                    }
                    Network::PublishTradeAnswer(s_session.GetID(), true);
                    logs::info("Trade {} accepted by the local player", s_session.GetID());
                    break;
                case Command::Kind::kCancel:
                    if (phase == Phase::kInviting || phase == Phase::kOpen || phase == Phase::kInvited)
                    {
                        CancelTrade(a_command.reason, a_command.reason == TradeCloseReason::kFailed
                                ? "The trade window could not open while another menu was open."
                                : std::string_view{});
                    }
                    return;
                case Command::Kind::kSetCount:
                    SetOfferCount(a_command);
                    break;
                case Command::Kind::kAccept:
                    if (s_session.GetAcceptBlocker() != TradeSession::Blocker::kNone || !KeepHeldItems(a_player))
                    {
                        break;
                    }
                    if (const auto acceptance = s_session.Accept())
                    {
                        logs::info("Trade {} accepted offers {} and {}", acceptance->id,
                            static_cast<unsigned long long>(acceptance->ownRevision),
                            static_cast<unsigned long long>(acceptance->peerRevision));
                        Network::PublishTradeAccept(*acceptance);
                    }
                    break;
            }
            s_viewDirty = true;
        }
    }

    void RequestTrade(const Multiplayer::RosterEntry& a_player)
    {
        PushCommand(Command{ .kind = Command::Kind::kRequest, .player = a_player });
    }

    void AnswerInvitation(bool a_accept)
    {
        PushCommand(Command{ .kind = Command::Kind::kAnswer, .accept = a_accept });
    }

    void Cancel(TradeCloseReason a_reason)
    {
        PushCommand(Command{ .kind = Command::Kind::kCancel, .reason = a_reason });
    }

    void SetOfferCount(std::uint64_t a_inventoryRevision, std::size_t a_row, std::uint32_t a_count)
    {
        PushCommand(Command{
            .kind = Command::Kind::kSetCount, .inventoryRevision = a_inventoryRevision, .row = a_row, .count = a_count });
    }

    void Accept()
    {
        PushCommand(Command{ .kind = Command::Kind::kAccept });
    }

    bool CopyView(std::uint64_t& a_revision, View& a_view)
    {
        const std::scoped_lock lock{ s_viewLock };
        if (a_revision == s_viewRevision)
        {
            return false;
        }
        a_revision = s_viewRevision;
        a_view = s_view;
        return true;
    }

    TradeSession::Phase GetPhase()
    {
        return s_phase.load(std::memory_order_acquire);
    }

    Multiplayer::PeerSlot GetInvitingPlayer()
    {
        return s_invitingPlayer.load(std::memory_order_acquire);
    }

    void RegisterEventSinks()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }
        auto* eventSources = RE::ScriptEventSourceHolder::GetSingleton();
        if (!eventSources)
        {
            logs::critical("Unable to register the trade hit event sink");
            return;
        }
        eventSources->AddEventSink(HitEventSink::GetSingleton());
        registered = true;
    }

    void QueueNetworkChanges(Network::RemotePlayerChanges& a_changes)
    {
        const bool lost = !a_changes.connected || a_changes.connectionGeneration != s_connectionGeneration;
        if (lost && s_session.IsActive())
        {
            logs::info("Trade with {} ended with the connection", PeerName());
            Notices::Post("The trade with " + PeerName() + " ended: the connection to the server was lost.");
            ClearTrade();
            s_events.clear();
        }
        s_connected = a_changes.connected;
        s_connectionGeneration = a_changes.connectionGeneration;
        if (a_changes.tradeEventsLost || s_events.size() + a_changes.tradeEvents.size() > kMaximumQueuedEvents)
        {
            s_events.clear();
            s_eventsLost = true;
        }
        for (auto& event : a_changes.tradeEvents)
        {
            s_events.push_back(std::move(event));
        }
        // Sync does not run without a player, as in the main menu.
        if (std::exchange(s_viewDirty, false))
        {
            PublishView();
        }
    }

    void Sync(RE::PlayerCharacter& a_player, bool a_inWorld, float a_delta)
    {
        PROFILE_FUNCTION();
        if (!GameThread::Require("TradeSync::Sync"))
        {
            return;
        }
        if (std::exchange(s_eventsLost, false) && s_session.IsActive())
        {
            CancelTrade(TradeCloseReason::kFailed, "The trade with " + PeerName() + " ended: its messages were lost.");
        }
        for (const auto& event : s_events)
        {
            HandleEvent(a_player, event);
        }
        s_events.clear();

        std::vector<Command> commands;
        {
            const std::scoped_lock lock{ s_commandsLock };
            commands.swap(s_commands);
        }
        for (const auto& command : commands)
        {
            HandleCommand(a_player, a_inWorld, command);
        }

        if (s_session.IsActive())
        {
            const auto phase = s_session.GetPhase();
            // Leaving the world needs nothing here: the server closes the trade on Exit.
            if (phase != Phase::kRequesting && IsInCombat(a_player))
            {
                CancelTrade(TradeCloseReason::kCombat, phase == Phase::kInvited
                        ? "You are in combat; the trade offer from " + PeerName() + " was declined."
                        : "You are in combat; the trade with " + PeerName() + " ended.");
            }
        }

        if (s_session.IsActive())
        {
            const auto tick = s_session.Advance(std::clamp(a_delta, 0.0F, kMaximumFrameSeconds));
            if (tick.timedOut)
            {
                logs::warn("Trade with {} timed out in phase {}", PeerName(), static_cast<int>(s_session.GetPhase()));
                CancelTrade(TradeCloseReason::kExpired, "The trade with " + PeerName() + " got no answer.");
            }
            else if (tick.offer)
            {
                logs::debug("Trade {} publishing offer revision {} with {} items", tick.offer->id,
                    static_cast<unsigned long long>(tick.offer->revision), tick.offer->items.size());
                Network::PublishTradeOffer(*tick.offer);
                RefreshOwnSkips();
            }
            const auto phase = s_session.GetPhase();
            const auto secondsLeft = phase == Phase::kInviting || phase == Phase::kInvited
                ? static_cast<std::uint32_t>(
                      std::ceil((std::max)(TradeSession::kInviteSeconds - s_session.GetWaited(), 0.0F)))
                : 0;
            if (secondsLeft != s_secondsLeft)
            {
                s_secondsLeft = secondsLeft;
                s_viewDirty = true;
            }
        }
        if (std::exchange(s_viewDirty, false))
        {
            PublishView();
        }
    }
}
