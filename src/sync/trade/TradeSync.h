#pragma once

#include "forms/ItemDetails.h"
#include "forms/ItemInstance.h"
#include "network/ServerClient.h"
#include "protocol/ChatMessage.h"
#include "protocol/TradeState.h"
#include "sync/trade/TradeSession.h"

#include <RE/Skyrim.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TradeSync
{
    // The server's default trade.request_radius, in game units, horizontal.
    inline constexpr float kRequestRadius = 400.0F;

    struct InventoryLine
    {
        std::string name;
        std::uint32_t count{ 0 };
        RE::FormID formID{ 0 };
        Forms::TradeCategory category{ Forms::TradeCategory::kMisc };
        Forms::ItemDetails details;
        // Disabled until the peer's game reports on the row's plugins.
        bool checking{ false };
        // Why it cannot be offered, or empty.
        std::string blocked;
    };

    struct OfferLine
    {
        // Index into View::inventory.
        std::size_t row{ 0 };
        std::uint32_t count{ 0 };
    };

    struct PeerLine
    {
        // Kept while the peer offers the same item and count.
        std::uint64_t id{ 0 };
        std::string name;
        std::uint32_t count{ 0 };
        RE::FormID formID{ 0 };
        Forms::ItemDetails details;
    };

    struct View
    {
        TradeSession::Phase phase{ TradeSession::Phase::kIdle };
        Multiplayer::PlayerIdentity peer;
        std::uint32_t secondsLeft{ 0 };
        std::uint64_t inventoryRevision{ 0 };
        std::vector<InventoryLine> inventory;
        // Whether any row is checking.
        bool checkingPlugins{ false };
        std::vector<OfferLine> offer;
        std::vector<PeerLine> peerOffer;
        bool ownAccepted{ false };
        bool peerAccepted{ false };
        // Why Accept is unavailable, or empty.
        std::string acceptBlocker;
    };

    // Overlay thread.
    void RequestTrade(const Multiplayer::PlayerIdentity& a_player);
    void AnswerInvitation(bool a_accept);
    void Cancel(Multiplayer::TradeCloseReason a_reason);
    void SetOfferCount(std::uint64_t a_inventoryRevision, std::size_t a_row, std::uint32_t a_count);
    void Accept();
    [[nodiscard]] bool CopyView(std::uint64_t& a_revision, View& a_view);
    [[nodiscard]] TradeSession::Phase GetPhase();
    // The other player of the open trade.
    [[nodiscard]] std::optional<Multiplayer::PlayerIdentity> GetOpenPeer();
    // The slot of the player whose invitation awaits an answer, or 0.
    [[nodiscard]] Multiplayer::PeerSlot GetInvitingPlayer();

    // Game thread.
    void RegisterEventSinks();
    void QueueNetworkChanges(Network::RemotePlayerChanges& a_changes);
    void Sync(RE::PlayerCharacter& a_player, bool a_inWorld, float a_delta);
}
