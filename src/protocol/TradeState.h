#pragma once

#include "protocol/ChatMessage.h"
#include "protocol/FormReference.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Multiplayer
{
    using TradeID = std::uint32_t;

    struct TradeEffect
    {
        FormReference effect;
        float magnitude{ 0.0F };
        std::uint32_t area{ 0 };
        std::uint32_t duration{ 0 };
        float cost{ 0.0F };

        bool operator==(const TradeEffect&) const = default;
    };

    struct TradeItem
    {
        static constexpr std::size_t kMaximumEffects = 4;
        static constexpr std::uint32_t kMaximumCount = (std::numeric_limits<std::int32_t>::max)();
        // A receiver creates each unit of an item with extra data on its own.
        static constexpr std::uint32_t kMaximumInstanceCount = 1000;
        static constexpr float kMaximumEffectValue = 1e6F;
        static constexpr float kMaximumEffectCost = 1e7F;
        static constexpr std::uint32_t kMaximumCharge = 0xFFFF;
        static constexpr float kMaximumHealth = 100.0F;
        static constexpr std::size_t kMaximumNameBytes = 128;
        static constexpr std::uint8_t kMaximumSoul = 5;

        FormReference item;
        std::uint32_t count{ 1 };
        // A plugin enchantment, or the effects of one a player made; never both.
        FormReference enchantment;
        std::vector<TradeEffect> effects;
        std::uint16_t maximumCharge{ 0 };
        float charge{ 0.0F };
        // 0 when untempered.
        float health{ 0.0F };
        // A name the player gave the item, or empty.
        std::string name;
        std::uint8_t soul{ 0 };

        [[nodiscard]] bool IsEnchanted() const noexcept { return !enchantment.IsEmpty() || !effects.empty(); }

        [[nodiscard]] bool HasExtraData() const noexcept
        {
            return IsEnchanted() || health != 0.0F || !name.empty() || soul != 0;
        }

        [[nodiscard]] bool IsSameInstance(const TradeItem& a_other) const
        {
            return item == a_other.item && enchantment == a_other.enchantment && effects == a_other.effects
                && maximumCharge == a_other.maximumCharge && charge == a_other.charge && health == a_other.health
                && name == a_other.name && soul == a_other.soul;
        }

        bool operator==(const TradeItem&) const = default;
    };

    inline constexpr std::size_t kMaximumTradeItems = 32;
    // Under the server's 16 KiB message limit, with room for the envelope.
    inline constexpr std::size_t kMaximumTradeOfferBytes = 15 * 1024;

    namespace Detail
    {
        [[nodiscard]] inline bool IsTradeValue(float a_value, float a_maximum)
        {
            return std::isfinite(a_value) && a_value >= 0.0F && a_value <= a_maximum;
        }

        [[nodiscard]] inline bool IsTradeText(std::string_view a_text, std::size_t a_maximumBytes)
        {
            if (a_text.size() > a_maximumBytes)
            {
                return false;
            }
            for (const auto character : a_text)
            {
                const auto byte = static_cast<unsigned char>(character);
                if (byte < 0x20 || byte == 0x7F)
                {
                    return false;
                }
            }
            return true;
        }
    }

    [[nodiscard]] inline bool IsValidTradeItem(const TradeItem& a_item)
    {
        using Detail::IsTradeValue;
        if (a_item.item.IsEmpty() || !IsValidFormReference(a_item.item, false) || a_item.count == 0
            || a_item.count > TradeItem::kMaximumCount || a_item.effects.size() > TradeItem::kMaximumEffects
            || (!a_item.enchantment.IsEmpty() && !a_item.effects.empty())
            || !IsValidFormReference(a_item.enchantment, true))
        {
            return false;
        }
        for (const auto& effect : a_item.effects)
        {
            if (effect.effect.IsEmpty() || !IsValidFormReference(effect.effect, false)
                || !IsTradeValue(effect.magnitude, TradeItem::kMaximumEffectValue)
                || effect.area > TradeItem::kMaximumEffectValue || effect.duration > TradeItem::kMaximumEffectValue
                || !IsTradeValue(effect.cost, TradeItem::kMaximumEffectCost))
            {
                return false;
            }
        }
        if (!IsTradeValue(a_item.charge, static_cast<float>(TradeItem::kMaximumCharge))
            || (!a_item.IsEnchanted() && (a_item.maximumCharge != 0 || a_item.charge != 0.0F)))
        {
            return false;
        }
        return IsTradeValue(a_item.health, TradeItem::kMaximumHealth)
            && Detail::IsTradeText(a_item.name, TradeItem::kMaximumNameBytes) && a_item.soul <= TradeItem::kMaximumSoul
            && (!a_item.HasExtraData() || a_item.count <= TradeItem::kMaximumInstanceCount);
    }

    // An upper bound of the item's encoded size, so an offer can be kept under the
    // message limit without encoding it.
    [[nodiscard]] inline std::size_t EstimateTradeItemBytes(const TradeItem& a_item)
    {
        constexpr std::size_t kFormReferenceOverhead = 12;
        constexpr std::size_t kItemOverhead = 48;
        constexpr std::size_t kEffectOverhead = 32;
        auto bytes = kItemOverhead + kFormReferenceOverhead + a_item.item.plugin.size() + a_item.name.size();
        if (!a_item.enchantment.IsEmpty())
        {
            bytes += kFormReferenceOverhead + a_item.enchantment.plugin.size();
        }
        for (const auto& effect : a_item.effects)
        {
            bytes += kEffectOverhead + kFormReferenceOverhead + effect.effect.plugin.size();
        }
        return bytes;
    }

    enum class TradeSkipReason : std::uint8_t
    {
        kMissingItem = 1,
        kWrongType,
        kMissingEffect,
        kBadEnchantment
    };

    struct TradeSkip
    {
        std::uint8_t index{ 0 };
        TradeSkipReason reason{ TradeSkipReason::kMissingItem };
        // The plugin the receiver lacks, when that is the reason.
        std::string plugin;

        bool operator==(const TradeSkip&) const = default;
    };

    [[nodiscard]] inline bool IsValidTradeSkips(const std::vector<TradeSkip>& a_skips)
    {
        std::uint32_t seen = 0;
        for (const auto& skip : a_skips)
        {
            if (skip.index >= kMaximumTradeItems || (seen & (1U << skip.index)) != 0
                || static_cast<std::uint8_t>(skip.reason) < static_cast<std::uint8_t>(TradeSkipReason::kMissingItem)
                || static_cast<std::uint8_t>(skip.reason) > static_cast<std::uint8_t>(TradeSkipReason::kBadEnchantment)
                || !Detail::IsTradeText(skip.plugin, FormReference::kMaximumPluginNameLength))
            {
                return false;
            }
            seen |= 1U << skip.index;
        }
        return a_skips.size() <= kMaximumTradeItems;
    }

    enum class TradeCloseReason : std::uint8_t
    {
        kDeclined = 1,
        kExpired,
        kCancelled,
        kBusy,
        kOutOfRange,
        kCombat,
        kMenu,
        kDisconnected,
        kFailed,
        kUnavailable
    };

    [[nodiscard]] constexpr const char* GetTradeCloseReasonName(TradeCloseReason a_reason) noexcept
    {
        switch (a_reason)
        {
            case TradeCloseReason::kDeclined:
                return "declined";
            case TradeCloseReason::kExpired:
                return "expired";
            case TradeCloseReason::kCancelled:
                return "cancelled";
            case TradeCloseReason::kBusy:
                return "busy";
            case TradeCloseReason::kOutOfRange:
                return "out of range";
            case TradeCloseReason::kCombat:
                return "combat";
            case TradeCloseReason::kMenu:
                return "menu";
            case TradeCloseReason::kDisconnected:
                return "disconnected";
            case TradeCloseReason::kFailed:
                return "failed";
            case TradeCloseReason::kUnavailable:
                return "unavailable";
        }
        return "unknown";
    }

    struct TradeInvite
    {
        TradeID id{ 0 };
        ChatSpeaker sender;
        ChatSpeaker recipient;
        // Set by the transport when the local player sent the invitation.
        bool outgoing{ false };
    };

    struct TradeOpen
    {
        TradeID id{ 0 };
    };

    struct TradeOffer
    {
        TradeID id{ 0 };
        std::uint64_t revision{ 0 };
        std::vector<TradeItem> items;
    };

    struct TradeReport
    {
        TradeID id{ 0 };
        std::uint64_t revision{ 0 };
        std::vector<TradeSkip> skipped;
    };

    // Both revisions as the accepting player names them: its own offer, then the other's.
    struct TradeAcceptance
    {
        TradeID id{ 0 };
        std::uint64_t ownRevision{ 0 };
        std::uint64_t peerRevision{ 0 };
    };

    struct TradeCommit
    {
        TradeID id{ 0 };
        std::uint64_t ownRevision{ 0 };
        std::uint64_t peerRevision{ 0 };
    };

    // An ID of 0 refuses the local player's request.
    struct TradeClose
    {
        TradeID id{ 0 };
        TradeCloseReason reason{ TradeCloseReason::kCancelled };
    };

    using TradeEvent
        = std::variant<TradeInvite, TradeOpen, TradeOffer, TradeReport, TradeAcceptance, TradeCommit, TradeClose>;
}
