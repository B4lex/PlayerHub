#pragma once

#include "text/AsciiCase.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace Multiplayer
{
    inline constexpr std::size_t kMaximumIdleEventLength = 64;
    inline constexpr std::string_view kIdleEventPrefix = "Idle";
    // Ends the carry-arm overlays some idle clips start, such as the drink that
    // IdleMQ201Drink leaves in the right hand.
    inline constexpr std::string_view kOffsetStopEvent = "OffsetStop";

    // An idle graph event name. Shared, so the checkpoints merged with one context
    // copy no string.
    using IdleEvent = std::shared_ptr<const std::string>;

    [[nodiscard]] inline std::string_view GetIdleEventName(const IdleEvent& a_event) noexcept
    {
        return a_event ? std::string_view{ *a_event } : std::string_view{};
    }

    [[nodiscard]] inline bool IsSameIdleEvent(const IdleEvent& a_left, const IdleEvent& a_right) noexcept
    {
        return a_left == a_right || GetIdleEventName(a_left) == GetIdleEventName(a_right);
    }

    [[nodiscard]] constexpr bool IsIdleEventCharacter(char a_character) noexcept
    {
        return (a_character >= 'A' && a_character <= 'Z') || (a_character >= 'a' && a_character <= 'z')
            || (a_character >= '0' && a_character <= '9') || a_character == '_';
    }

    // An event a player may be held in: the Idle prefix, compared without case as
    // graph events are, then identifier characters.
    [[nodiscard]] constexpr bool IsValidHeldIdleEvent(std::string_view a_name) noexcept
    {
        if (a_name.size() <= kIdleEventPrefix.size() || a_name.size() > kMaximumIdleEventLength
            || !AsciiCase::StartsWith(a_name, kIdleEventPrefix))
        {
            return false;
        }
        for (const char character : a_name)
        {
            if (!IsIdleEventCharacter(character))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr bool IsValidIdleActionEvent(std::string_view a_name) noexcept
    {
        return IsValidHeldIdleEvent(a_name) || AsciiCase::Equals(a_name, kOffsetStopEvent);
    }

    static_assert(IsValidHeldIdleEvent("IdleLaugh") && IsValidHeldIdleEvent("idleLooseSweepingStart"));
    static_assert(IsValidHeldIdleEvent("IdlePointFar_02"));
    static_assert(!IsValidHeldIdleEvent("Idle") && !IsValidHeldIdleEvent("attackStart"));
    static_assert(!IsValidHeldIdleEvent("Idle Laugh") && !IsValidHeldIdleEvent("Idle.Laugh"));
    static_assert(!IsValidHeldIdleEvent("OffsetStop") && IsValidIdleActionEvent("OffsetStop"));
    static_assert(IsValidIdleActionEvent("offsetStop") && !IsValidIdleActionEvent("OffsetStart"));
}
