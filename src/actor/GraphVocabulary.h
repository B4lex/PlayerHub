#pragma once

#include "text/AsciiCase.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <string_view>

namespace GraphVocabulary
{
    // Pandora, Nemesis, and FNIS append their events and variables to the lists of
    // the vanilla third-person 0_master, so entries past these counts are modded.
    inline constexpr std::int32_t kVanillaEventCount = 1217;
    inline constexpr std::int32_t kVanillaVariableCount = 230;

    // FNV-1a 64 of the ASCII-lowercased name, as scripts/generate_vanilla_graph_names.py
    // hashes the vanilla names: graph names compare without case.
    [[nodiscard]] constexpr std::uint64_t HashName(std::string_view a_name) noexcept
    {
        std::uint64_t hash = 0xCBF29CE484222325ULL;
        for (const char character : a_name)
        {
            hash = (hash ^ static_cast<std::uint8_t>(AsciiCase::ToLower(character))) * 0x100000001B3ULL;
        }
        return hash;
    }

    static_assert(HashName("RootBehaviorGraph") == 0x1609DB7EC065E555ULL);
    static_assert(HashName("rootbehaviorgraph") == HashName("RootBehaviorGraph"));

    // The last vanilla event and variable are where the vanilla third-person
    // 0_master lists end; the first-person graph has its own lists.
    [[nodiscard]] bool HasVanillaPrefix(const RE::hkbBehaviorGraphStringData& a_strings);
    [[nodiscard]] bool IsVanillaMachine(std::uint64_t a_nameHash) noexcept;
    [[nodiscard]] bool IsVanillaState(std::uint64_t a_nameHash) noexcept;
}
