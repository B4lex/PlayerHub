#pragma once

#include "forms/ItemFilter.h"

#include <RE/Skyrim.h>

#include <optional>

// Reads item forms from the engine. Main-loop thread only.
namespace Forms
{
    // Nullopt for anything but armor with biped slots, a weapon of a synchronized
    // weapon type, a carriable torch, ammunition, or a playable race.
    [[nodiscard]] std::optional<ItemTraits> GetItemTraits(RE::TESForm& a_form);

    // Publishes the ItemCatalog of every named item and playable race. Requires
    // Skyrim's data-loaded state.
    void BuildItemCatalog();
}
