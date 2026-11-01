#include "actor/GraphVocabulary.h"
#include "actor/BehaviorGraphLayout.h"
#include "actor/VanillaGraphNames.h"

#include <algorithm>
#include <string_view>

namespace GraphVocabulary
{
    namespace
    {
        static_assert(std::ranges::is_sorted(Generated::kVanillaMachineNameHashes));
        static_assert(std::ranges::is_sorted(Generated::kVanillaStateNameHashes));
        static_assert(std::ranges::binary_search(Generated::kVanillaMachineNameHashes, HashName("RootBehaviorGraph")));
        static_assert(std::ranges::binary_search(Generated::kVanillaStateNameHashes, HashName("JumpFallDirectionalBlend")));
        static_assert(!std::ranges::binary_search(Generated::kVanillaStateNameHashes, HashName("Paraglide_State")));
    }

    bool HasVanillaPrefix(const RE::hkbBehaviorGraphStringData& a_strings)
    {
        using BehaviorGraphLayout::GetName;
        return a_strings.eventNames.size() >= kVanillaEventCount
            && a_strings.variableNames.size() >= kVanillaVariableCount
            && std::string_view{ GetName(a_strings.eventNames[kVanillaEventCount - 1]) }
                   == "SoundPlay.NPCKillDLC2HeartStone"
            && std::string_view{ GetName(a_strings.variableNames[kVanillaVariableCount - 1]) }
                   == "iWantMountedWeaponAnims";
    }

    bool IsVanillaMachine(std::uint64_t a_nameHash) noexcept
    {
        return std::ranges::binary_search(Generated::kVanillaMachineNameHashes, a_nameHash);
    }

    bool IsVanillaState(std::uint64_t a_nameHash) noexcept
    {
        return std::ranges::binary_search(Generated::kVanillaStateNameHashes, a_nameHash);
    }
}
