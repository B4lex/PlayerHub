#pragma once

#include <RE/Skyrim.h>

namespace StaffSpell
{
    // What the staff in that hand casts, or null without a staff or a castable
    // selection.
    [[nodiscard]] RE::MagicItem* GetSpell(RE::Actor& a_actor, bool a_leftHand);

    // Whether the hand's selection may stand for a staff that casts nothing: none,
    // or a weapon's own enchantment, which the cast idles ignore.
    [[nodiscard]] bool IsCastless(const RE::MagicItem* a_selected);

    [[nodiscard]] RE::MagicItem* GetSelected(RE::Actor& a_actor, bool a_leftHand);

    // Game thread only.
    void Select(RE::Actor& a_actor, bool a_leftHand, RE::MagicItem* a_spell);
}
