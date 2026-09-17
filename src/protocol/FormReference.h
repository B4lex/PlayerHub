#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Multiplayer
{
    // Load-order independent form identity: the defining plugin's file name and
    // the form ID local to that file. An empty plugin name means "no form".
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
}
