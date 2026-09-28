#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/EquipmentState.h"

#include <RE/Skyrim.h>

#include <cstdint>
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

    // Appends a_outfit armor for an unresolved entry: the pieces whose stand-in
    // slots the entry covers, skipping any whose slots overlap a_occupiedSlots,
    // which then gains the appended armor's slots.
    void AppendArmor(const Multiplayer::DefaultOutfit& a_outfit, const Multiplayer::EquipmentArmor& a_entry,
        std::uint32_t& a_occupiedSlots, std::vector<RE::TESObjectARMO*>& a_armor);

    // A weapon of the same weapon type, or a torch. Null when a_outfit has none
    // that resolves.
    [[nodiscard]] RE::TESBoundObject* FindHand(
        const Multiplayer::DefaultOutfit& a_outfit, const Multiplayer::EquipmentHand& a_entry);

    [[nodiscard]] RE::TESAmmo* FindAmmo(const Multiplayer::DefaultOutfit& a_outfit);
}
