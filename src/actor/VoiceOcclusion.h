#pragma once

#include <RE/Skyrim.h>

namespace VoiceOcclusion
{
    // Whether level geometry stands between two points of the cell's Havok world, so a voice from
    // one is heard muffled at the other. Characters, water, and clutter do not count. Game thread.
    [[nodiscard]] bool IsBlocked(RE::TESObjectCELL& a_cell, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to);
}
