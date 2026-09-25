#pragma once

#include "protocol/EquipmentState.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <vector>

// Local stand-ins, from Config::EquipmentFallbackSettings, for remote equipment
// this game cannot resolve. Main-loop thread only.
namespace EquipmentFallback
{
    // Resolves the configured items once and logs unusable ones. Requires Skyrim's
    // data-loaded state; until then nothing has a fallback.
    void Initialize();

    // Appends fallback armor for an unresolved entry: one per slot the entry
    // covers, skipping any whose slots overlap a_occupiedSlots, which then gains
    // the appended armor's slots.
    void AppendArmor(const Multiplayer::EquipmentArmor& a_entry, std::uint32_t& a_occupiedSlots,
        std::vector<RE::TESObjectARMO*>& a_armor);

    // A weapon of the same weapon type, or a torch. Null when none is configured.
    [[nodiscard]] RE::TESBoundObject* FindHand(const Multiplayer::EquipmentHand& a_entry);

    [[nodiscard]] RE::TESAmmo* FindAmmo(bool a_bolt);
}
