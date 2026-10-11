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
#include <iterator>
#include <mutex>
#include <numeric>
#include <optional>
#include <string_view>
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

        struct RowCheck
        {
            bool confirmed{ false };
            // Why the peer's game cannot receive it, or empty.
            std::string rejected;
        };

        // Parallel to s_inventory.
        std::vector<RowCheck> s_rowChecks;
        std::vector<std::string> s_peerMissingPlugins;
        // Includes lines the peer's game has not confirmed yet.
        std::vector<OfferLine> s_offer;
        std::vector<PeerLine> s_peerOffer;
        // Parallel to s_peerOffer.
        std::vector<Multiplayer::TradeItem> s_peerItems;
        std::uint64_t s_nextPeerLineID = 1;
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

        std::string DescribeRejection(const Multiplayer::TradeSkip& a_skip, const Multiplayer::TradeItem& a_item)
        {
            const auto& peer = PeerName();
            if (!a_skip.plugin.empty())
            {
                return peer + " does not have " + a_skip.plugin + " installed.";
            }
            switch (a_skip.reason)
            {
                case Multiplayer::TradeSkipReason::kMissingItem:
                case Multiplayer::TradeSkipReason::kWrongType:
                    return peer + " has a different version of " + a_item.item.plugin + ".";
                case Multiplayer::TradeSkipReason::kMissingEffect:
                    return peer + "'s game does not have the enchantment.";
                case Multiplayer::TradeSkipReason::kBadEnchantment:
                    break;
            }
            return peer + "'s game cannot rebuild the enchantment.";
        }

        bool ReferencesPlugin(const Multiplayer::TradeItem& a_item, std::string_view a_plugin)
        {
            return a_item.item.plugin == a_plugin || a_item.enchantment.plugin == a_plugin
                || std::ranges::any_of(
                    a_item.effects, [&](const auto& a_effect) { return a_effect.effect.plugin == a_plugin; });
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
            s_rowChecks.clear();
            s_peerMissingPlugins.clear();
            s_offer.clear();
            s_peerOffer.clear();
            s_peerItems.clear();
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
            std::ranges::copy_if(s_offer, std::back_inserter(view.offer),
                [](const OfferLine& a_line) { return s_rowChecks[a_line.row].confirmed; });
            view.inventory.reserve(s_inventory.size());
            for (std::size_t i = 0; i < s_inventory.size(); ++i)
            {
                const auto& row = s_inventory[i];
                const auto& rejected = s_rowChecks[i].rejected;
                view.inventory.push_back(InventoryLine{ .name = row.name,
                    .count = row.item.count,
                    .formID = row.formID,
                    .category = row.category,
                    .details = row.details,
                    .enchanted = row.item.IsEnchanted(),
                    .blocked = row.block ? DescribeBlock(*row.block) : rejected });
            }
            s_phase.store(view.phase, std::memory_order_release);
            s_invitingPlayer.store(view.phase == Phase::kInvited ? view.peer.playerID : 0, std::memory_order_release);
            const std::scoped_lock lock{ s_viewLock };
            s_view = std::move(view);
            ++s_viewRevision;
        }

        std::optional<std::size_t> FindRow(const Multiplayer::TradeItem& a_item)
        {
            const auto row = std::ranges::find_if(
                s_inventory, [&](const TradeInventory::Row& a_row) { return a_row.item.IsSameInstance(a_item); });
            return row != s_inventory.end() ? std::optional{ static_cast<std::size_t>(row - s_inventory.begin()) }
                                            : std::nullopt;
        }

        // Returns whether the row was in the offer, which it leaves.
        bool RejectRow(std::size_t a_row, std::string a_reason)
        {
            s_rowChecks[a_row] = RowCheck{ .rejected = std::move(a_reason) };
            return std::erase_if(s_offer, [a_row](const OfferLine& a_line) { return a_line.row == a_row; }) > 0;
        }

        // Calls a_rejected with each row the plugin newly rejects that was in the offer.
        template <class Callback>
        void RejectPluginRows(const std::string& a_plugin, Callback&& a_rejected)
        {
            for (std::size_t i = 0; i < s_inventory.size(); ++i)
            {
                if (!s_inventory[i].block && s_rowChecks[i].rejected.empty()
                    && ReferencesPlugin(s_inventory[i].item, a_plugin))
                {
                    if (RejectRow(i, PeerName() + " does not have " + a_plugin + " installed."))
                    {
                        a_rejected(i);
                    }
                }
            }
        }

        void CaptureInventory(RE::PlayerCharacter& a_player)
        {
            auto inventory = TradeInventory::Capture(a_player);
            std::vector<RowCheck> checks(inventory.size());
            // A capture during the trade may order the rows differently.
            for (std::size_t i = 0; i < s_inventory.size(); ++i)
            {
                if (!s_rowChecks[i].confirmed && s_rowChecks[i].rejected.empty())
                {
                    continue;
                }
                const auto row = std::ranges::find_if(inventory,
                    [&](const TradeInventory::Row& a_row) { return a_row.item.IsSameInstance(s_inventory[i].item); });
                if (row != inventory.end())
                {
                    checks[static_cast<std::size_t>(row - inventory.begin())] = std::move(s_rowChecks[i]);
                }
            }
            s_inventory = std::move(inventory);
            s_rowChecks = std::move(checks);
            ++s_inventoryRevision;
            s_offer.clear();
            for (const auto& plugin : s_peerMissingPlugins)
            {
                RejectPluginRows(plugin, [](std::size_t) {});
            }
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

        // Confirms the lines the peer's report accepts and takes the others out of the offer.
        void ApplyOwnReport()
        {
            const auto* items = s_session.FindReportedOffer();
            if (!items)
            {
                return;
            }
            const auto& skips = s_session.GetReportedSkips();
            // One notice for each reason.
            struct Rejection
            {
                std::string reason;
                std::vector<std::string> names;
            };
            std::vector<Rejection> rejections;
            const auto note = [&rejections](std::size_t a_row) {
                const auto& reason = s_rowChecks[a_row].rejected;
                auto entry = std::ranges::find(rejections, reason, &Rejection::reason);
                if (entry == rejections.end())
                {
                    entry = rejections.insert(rejections.end(), Rejection{ .reason = reason });
                }
                entry->names.push_back(s_inventory[a_row].name);
            };
            bool offerChanged = false;
            for (std::size_t i = 0; i < items->size(); ++i)
            {
                const auto& item = (*items)[i];
                const auto row = FindRow(item);
                if (!row || !s_rowChecks[*row].rejected.empty())
                {
                    continue;
                }
                const auto skip = std::ranges::find(skips, i, &Multiplayer::TradeSkip::index);
                if (skip == skips.end())
                {
                    s_rowChecks[*row].confirmed = true;
                    continue;
                }
                logs::info("Trade {}: {} cannot receive {}|{:06X}: reason={} plugin={}", s_session.GetID(), PeerName(),
                    item.item.plugin, item.item.localFormID, static_cast<int>(skip->reason), skip->plugin);
                offerChanged = RejectRow(*row, DescribeRejection(*skip, item)) || offerChanged;
                note(*row);
                if (!skip->plugin.empty()
                    && std::ranges::find(s_peerMissingPlugins, skip->plugin) == s_peerMissingPlugins.end())
                {
                    s_peerMissingPlugins.push_back(skip->plugin);
                    RejectPluginRows(skip->plugin, [&](std::size_t a_row) {
                        offerChanged = true;
                        note(a_row);
                    });
                }
            }
            for (const auto& rejection : rejections)
            {
                Notices::Post("You cannot offer " + FormatItems(rejection.names) + ": " + rejection.reason);
            }
            if (offerChanged)
            {
                s_session.SetOwnItems(BuildOwnItems());
            }
            s_viewDirty = true;
        }

        void SetOfferCount(const Command& a_command)
        {
            if (s_session.GetPhase() != Phase::kOpen || a_command.inventoryRevision != s_inventoryRevision
                || a_command.row >= s_inventory.size() || s_inventory[a_command.row].block
                || !s_rowChecks[a_command.row].rejected.empty())
            {
                return;
            }
            const auto& row = s_inventory[a_command.row];
            auto offer = s_offer;
            const auto line = std::ranges::find(offer, a_command.row, &OfferLine::row);
            const auto count = (std::min)(a_command.count, row.item.count);
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
            s_viewDirty = true;
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
            TradeInventory::PlayPickUpSound(a_player);
            Notices::Post("You successfully traded.");
        }

        void ReportPeerOffer()
        {
            std::vector<Multiplayer::TradeSkip> skips;
            auto previousLines = std::exchange(s_peerOffer, {});
            auto previousItems = std::exchange(s_peerItems, {});
            const auto& items = s_session.GetPeerItems();
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                const auto& item = items[i];
                auto resolved = Forms::ResolveItem(item);
                if (!resolved)
                {
                    // Not shown: the peer's game takes it out of the offer on this report.
                    auto skip = resolved.error();
                    skip.index = static_cast<std::uint8_t>(i);
                    logs::info("Trade cannot receive {}|{:06X}: reason={} plugin={}", item.item.plugin,
                        item.item.localFormID, static_cast<int>(skip.reason), skip.plugin);
                    skips.push_back(std::move(skip));
                    continue;
                }
                const auto previous = std::ranges::find(previousItems, item);
                const auto id = previous != previousItems.end()
                    ? previousLines[static_cast<std::size_t>(previous - previousItems.begin())].id
                    : s_nextPeerLineID++;
                s_peerItems.push_back(item);
                s_peerOffer.push_back(PeerLine{ .id = id,
                    .name = Forms::GetDisplayName(*resolved->object, item),
                    .count = item.count,
                    .formID = resolved->object->GetFormID(),
                    .enchanted = item.IsEnchanted(),
                    .details = Forms::GetItemDetails(*resolved, item) });
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
                    ApplyOwnReport();
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
                s_viewDirty = true;
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
