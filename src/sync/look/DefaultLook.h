#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/EquipmentState.h"

#include <RE/Skyrim.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace DefaultLook
{
    // What resolving a [DefaultLook] Preset for a [DefaultLook] Race left out, for the Default Look tab.
    struct PresetOutcome
    {
        std::string preset;
        Multiplayer::FormReference race;
        bool unreadable{ false };
        // Not from a base-game plugin, or missing from this game.
        std::uint32_t moddedParts{ 0 };
        // Not valid for the race or for the preset's sex.
        std::uint32_t unfitParts{ 0 };
        // Not one of the race's tint layers, or over the limit.
        std::uint32_t droppedTints{ 0 };
    };

    void Initialize();

    void Refresh();

    void Select(Multiplayer::PlayerEquipment& a_equipment);

    [[nodiscard]] std::optional<Multiplayer::FormReference> GetAlwaysUseRace();

    // The [DefaultLook] Preset face; unset without one.
    [[nodiscard]] const std::optional<Multiplayer::AppearanceDefaultFace>& GetDefaultFace();

    // Safe from any thread. Resolved by the next ServeRequests unless it is the latest outcome.
    void RequestPresetOutcome(std::string_view a_preset, const Multiplayer::FormReference& a_race);

    // Safe from any thread: the applied settings' outcome, or the latest requested one.
    [[nodiscard]] PresetOutcome GetPresetOutcome();

    // Every main-loop pass, paused included: the settings window asks while the game is paused.
    void ServeRequests();

    void SelectRace(Multiplayer::PlayerAppearance& a_appearance);

    [[nodiscard]] std::array<RE::TESObjectARMO*, Multiplayer::DefaultOutfit::kMaximumArmor> SelectArmor(
        const Multiplayer::DefaultOutfit& a_outfit, std::span<RE::TESObjectARMO* const> a_resolved,
        std::uint32_t a_missingSlots, std::vector<RE::TESObjectARMO*>& a_armor);

    [[nodiscard]] RE::TESBoundObject* FindHand(
        const Multiplayer::DefaultOutfit& a_outfit, const Multiplayer::EquipmentHand& a_entry);

    [[nodiscard]] RE::TESAmmo* FindAmmo(const Multiplayer::DefaultOutfit& a_outfit);
}
