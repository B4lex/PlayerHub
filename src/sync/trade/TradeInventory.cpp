#include "sync/trade/TradeInventory.h"
#include "forms/FormLookup.h"
#include "runtime/GameThread.h"

#include <algorithm>

namespace TradeInventory
{
    namespace
    {
        struct Stack
        {
            RE::TESBoundObject* object{ nullptr };
            // Null for the units without extra data.
            RE::ExtraDataList* extra{ nullptr };
            std::int32_t count{ 0 };
        };

        bool IsWorn(const RE::ExtraDataList* a_extra)
        {
            return a_extra
                && (a_extra->HasType(RE::ExtraDataType::kWorn) || a_extra->HasType(RE::ExtraDataType::kWornLeft));
        }

        // The live extra lists of the player's inventory changes, since GetInventory hands
        // out copied entries.
        template <class Visitor>
        void VisitStacks(RE::PlayerCharacter& a_player, RE::TESBoundObject* a_only, Visitor&& a_visitor)
        {
            auto* changes = a_player.GetInventoryChanges();
            auto counts = a_player.GetInventoryCounts(
                [a_only](RE::TESBoundObject& a_object) { return !a_only || &a_object == a_only; });
            for (const auto& [object, total] : counts)
            {
                if (!object || total <= 0)
                {
                    continue;
                }
                std::int32_t listed = 0;
                if (changes && changes->entryList)
                {
                    for (auto* entry : *changes->entryList)
                    {
                        if (!entry || entry->object != object || !entry->extraLists)
                        {
                            continue;
                        }
                        for (auto* extra : *entry->extraLists)
                        {
                            const auto count = extra ? extra->GetCount() : 0;
                            if (count <= 0)
                            {
                                continue;
                            }
                            listed += count;
                            a_visitor(Stack{ .object = object, .extra = extra, .count = (std::min)(count, total) });
                        }
                    }
                }
                if (total > listed)
                {
                    a_visitor(Stack{ .object = object, .count = total - listed });
                }
            }
        }

        std::string GetName(RE::TESBoundObject& a_object, RE::ExtraDataList* a_extra)
        {
            const char* name = a_extra ? a_extra->GetDisplayName(&a_object) : a_object.GetName();
            return name ? std::string{ name } : std::string{};
        }
    }

    std::vector<Row> Capture(RE::PlayerCharacter& a_player)
    {
        std::vector<Row> rows;
        if (!GameThread::Require("TradeInventory::Capture"))
        {
            return rows;
        }
        VisitStacks(a_player, nullptr, [&](const Stack& a_stack) {
            const auto category = Forms::GetTradeCategory(*a_stack.object);
            if (!category)
            {
                return;
            }
            auto name = GetName(*a_stack.object, a_stack.extra);
            if (name.empty())
            {
                return;
            }
            auto described = Forms::DescribeItem(*a_stack.object, a_stack.extra);
            Row row{ .formID = a_stack.object->GetFormID(),
                .name = std::move(name),
                .category = *category,
                .weight = a_stack.object->GetWeight(),
                .equipped = IsWorn(a_stack.extra),
                .favorited = a_stack.extra && a_stack.extra->HasType(RE::ExtraDataType::kHotkey) };
            if (described)
            {
                row.item = std::move(*described);
            }
            else
            {
                row.block = described.error();
            }
            row.item.count = static_cast<std::uint32_t>(a_stack.count);
            // Several lists may hold the same item, such as one worn and the rest carried.
            const auto same = std::ranges::find_if(rows, [&](const Row& a_row) {
                return !row.block && !a_row.block && a_row.formID == row.formID && a_row.item.IsSameInstance(row.item);
            });
            if (same == rows.end())
            {
                rows.push_back(std::move(row));
                return;
            }
            same->item.count += row.item.count;
            same->equipped = same->equipped || row.equipped;
            same->favorited = same->favorited || row.favorited;
        });
        std::ranges::sort(rows, [](const Row& a_left, const Row& a_right) { return a_left.name < a_right.name; });
        return rows;
    }

    std::uint32_t Count(RE::PlayerCharacter& a_player, const Multiplayer::TradeItem& a_item)
    {
        auto* object = FormLookup::ResolveForm<RE::TESBoundObject>(a_item.item);
        if (!object)
        {
            return 0;
        }
        std::uint32_t count = 0;
        VisitStacks(a_player, object, [&](const Stack& a_stack) {
            const auto described = Forms::DescribeItem(*a_stack.object, a_stack.extra);
            if (described && described->IsSameInstance(a_item))
            {
                count += static_cast<std::uint32_t>(a_stack.count);
            }
        });
        return count;
    }

    std::uint32_t Remove(RE::PlayerCharacter& a_player, const Multiplayer::TradeItem& a_item)
    {
        if (!GameThread::Require("TradeInventory::Remove"))
        {
            return 0;
        }
        auto* object = FormLookup::ResolveForm<RE::TESBoundObject>(a_item.item);
        if (!object)
        {
            return 0;
        }
        std::vector<Stack> stacks;
        VisitStacks(a_player, object, [&](const Stack& a_stack) {
            const auto described = Forms::DescribeItem(*a_stack.object, a_stack.extra);
            if (described && described->IsSameInstance(a_item))
            {
                stacks.push_back(a_stack);
            }
        });
        std::ranges::stable_partition(stacks, [](const Stack& a_stack) { return !IsWorn(a_stack.extra); });

        std::uint32_t removed = 0;
        for (const auto& stack : stacks)
        {
            if (removed >= a_item.count)
            {
                break;
            }
            const auto count = (std::min)(static_cast<std::uint32_t>(stack.count), a_item.count - removed);
            a_player.RemoveItem(object, static_cast<std::int32_t>(count), RE::ITEM_REMOVE_REASON::kRemove, stack.extra,
                nullptr);
            removed += count;
        }
        return removed;
    }

    std::uint32_t Add(
        RE::PlayerCharacter& a_player, const Forms::ResolvedItem& a_resolved, const Multiplayer::TradeItem& a_item)
    {
        if (!GameThread::Require("TradeInventory::Add") || !a_resolved.object)
        {
            return 0;
        }
        if (!a_item.HasExtraData())
        {
            a_player.AddObjectToContainer(a_resolved.object, nullptr, static_cast<std::int32_t>(a_item.count), nullptr);
            return a_item.count;
        }

        auto* enchantment = a_resolved.enchantment;
        if (!enchantment && !a_resolved.effects.empty())
        {
            enchantment = Forms::CreateEnchantment(a_resolved);
            if (!enchantment)
            {
                logs::warn("Trade could not create the enchantment of {}|{:06X}; the item arrives without it",
                    a_item.item.plugin, a_item.item.localFormID);
            }
        }
        // An inventory stack's extra data cannot be built here, but a placed reference has
        // its own list, which picking it up moves into the inventory.
        std::uint32_t added = 0;
        for (std::uint32_t unit = 0; unit < a_item.count; ++unit)
        {
            const auto reference = a_player.PlaceObjectAtMe(a_resolved.object, false);
            if (!reference)
            {
                logs::warn("Trade could not place {}|{:06X} to pick it up", a_item.item.plugin, a_item.item.localFormID);
                break;
            }
            Forms::ApplyExtraData(*reference, a_resolved, a_item, enchantment);
            a_player.PickUpObject(reference.get(), 1, false, false);
            ++added;
        }
        return added;
    }
}
