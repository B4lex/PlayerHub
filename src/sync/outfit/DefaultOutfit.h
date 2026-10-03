#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/EquipmentState.h"

#include <RE/Skyrim.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// The local player's default outfit, from Config::DefaultOutfitSettings, which
// peers show in place of its race and equipment they cannot resolve; and a
// remote player's outfit, shown in place of its equipment this game cannot
// resolve. Main-loop thread only.
namespace DefaultOutfit
{
    // Resolves the configured items and logs unusable ones. Requires Skyrim's
    // data-loaded state; until then the local outfit is empty.
    void Initialize();

    // Once per pass: resolves the items again after the settings window changed
    // them. Captures after that select from the new items.
    void Refresh();

    // Replaces a_equipment.defaultOutfit with the local outfit pieces that stand
    // in for its items from other than base-game plugins, which peers may lack.
    void Select(Multiplayer::PlayerEquipment& a_equipment);

    // Replaces a_appearance.defaultRace with the local race and sex when its race
    // is from other than a base-game plugin.
    void SelectRace(Multiplayer::PlayerAppearance& a_appearance);

    // Fills a_armor with a_resolved, a remote player's armor that resolved, and
    // the a_outfit pieces standing in for a_missingSlots, the slots its
    // unresolved armor covers. Items are taken by the rank of their main slot
    // in kArmorSlotPrecedence, a resolved item before a piece of the same rank,
    // and an item overlapping one already taken is left out, so a hooded body
    // piece wins over a head piece or a resolved helmet rather than leave the
    // body bare. Returns the armor taken for each a_outfit piece, null for the
    // pieces left out.
    [[nodiscard]] std::array<RE::TESObjectARMO*, Multiplayer::DefaultOutfit::kMaximumArmor> SelectArmor(
        const Multiplayer::DefaultOutfit& a_outfit, std::span<RE::TESObjectARMO* const> a_resolved,
        std::uint32_t a_missingSlots, std::vector<RE::TESObjectARMO*>& a_armor);

    // A weapon of the same weapon type, or a torch. Null when a_outfit has none
    // that resolves.
    [[nodiscard]] RE::TESBoundObject* FindHand(
        const Multiplayer::DefaultOutfit& a_outfit, const Multiplayer::EquipmentHand& a_entry);

    [[nodiscard]] RE::TESAmmo* FindAmmo(const Multiplayer::DefaultOutfit& a_outfit);
}
