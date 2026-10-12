#pragma once

#include "text/AsciiCase.h"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Multiplayer
{
    struct FormReference
    {
        static constexpr std::size_t kMaximumPluginNameLength = 260;
        static constexpr std::uint32_t kMaximumLocalFormID = 0x00FFFFFF;

        std::string plugin;
        std::uint32_t localFormID{ 0 };

        [[nodiscard]] bool IsEmpty() const noexcept { return plugin.empty(); }

        bool operator==(const FormReference&) const = default;
        auto operator<=>(const FormReference&) const = default;
    };

    [[nodiscard]] inline bool IsValidFormReference(const FormReference& a_reference, bool a_allowEmpty)
    {
        if (a_reference.IsEmpty())
        {
            return a_allowEmpty && a_reference.localFormID == 0;
        }
        return a_reference.plugin.size() <= FormReference::kMaximumPluginNameLength
            && a_reference.localFormID <= FormReference::kMaximumLocalFormID;
    }

    namespace Detail
    {
        // Plugin names compare without case, as the game's lookup does.
        [[nodiscard]] constexpr bool PluginNameLess(std::string_view a_left, std::string_view a_right) noexcept
        {
            return std::ranges::lexicographical_compare(a_left, a_right,
                [](char a_first, char a_second) { return AsciiCase::ToLower(a_first) < AsciiCase::ToLower(a_second); });
        }

        // The Skyrim Special Edition masters, and Anniversary Edition's backpacks,
        // which only use slot 47 and so leave other slots as they are on installs
        // without them.
        inline constexpr std::array<std::string_view, 6> kBaseGamePlugins{
            "ccFSVSSE001-Backpacks.esl",
            "Dawnguard.esm",
            "Dragonborn.esm",
            "HearthFires.esm",
            "Skyrim.esm",
            "Update.esm",
        };
        static_assert(std::ranges::is_sorted(kBaseGamePlugins, PluginNameLess));
    }

    [[nodiscard]] constexpr bool IsBaseGamePlugin(std::string_view a_plugin) noexcept
    {
        return std::ranges::binary_search(Detail::kBaseGamePlugins, a_plugin, Detail::PluginNameLess);
    }
}
