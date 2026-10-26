#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/EquipmentState.h"
#include "sync/StandInLevel.h"

#include <RE/Skyrim.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace DefaultOutfit
{
    void Initialize();

    void Refresh();

    void Select(Multiplayer::PlayerEquipment& a_equipment);

    void SelectRace(Multiplayer::PlayerAppearance& a_appearance);

    [[nodiscard]] std::array<RE::TESObjectARMO*, Multiplayer::DefaultOutfit::kMaximumArmor> SelectArmor(
        const Multiplayer::DefaultOutfit& a_outfit, std::span<RE::TESObjectARMO* const> a_resolved,
        std::uint32_t a_missingSlots, std::vector<RE::TESObjectARMO*>& a_armor);

    [[nodiscard]] RE::TESBoundObject* FindHand(
        const Multiplayer::DefaultOutfit& a_outfit, const Multiplayer::EquipmentHand& a_entry);

    [[nodiscard]] RE::TESAmmo* FindAmmo(const Multiplayer::DefaultOutfit& a_outfit);

    [[nodiscard]] StandIn::Level GetArmorStandInLevel(std::uint32_t a_slotMask);
}
