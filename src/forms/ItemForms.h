#pragma once

#include "forms/ItemFilter.h"
#include "protocol/EquipmentState.h"

#include <RE/Skyrim.h>

#include <optional>

// Reads item forms from the engine. Main-loop thread only.
namespace Forms
{
    // Nullopt for anything but armor with biped slots, a weapon of a synchronized
    // weapon type, a carriable torch, or ammunition.
    [[nodiscard]] std::optional<ItemTraits> GetItemTraits(RE::TESForm& a_form);

    [[nodiscard]] Multiplayer::EquipmentArmorType GetArmorType(const RE::TESObjectARMO& a_armor);

    // Publishes the ItemCatalog of every named item. Requires Skyrim's
    // data-loaded state.
    void BuildItemCatalog();
}
