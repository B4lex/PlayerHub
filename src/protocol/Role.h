#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Multiplayer
{
    // Zero is no role.
    using RoleID = std::uint32_t;

    inline constexpr std::size_t kMaximumRoles = 32;
    inline constexpr std::size_t kMaximumRoleNameCharacters = 24;
    // UTF-8 takes at most 4 bytes for a code point.
    inline constexpr std::size_t kMaximumRoleNameBytes = kMaximumRoleNameCharacters * 4;
    inline constexpr std::uint32_t kMaximumRoleColor = 0xFFFFFF;

    struct Role
    {
        RoleID id{ 0 };
        std::string name;
        // 0xRRGGBB
        std::uint32_t color{ 0 };
    };

    [[nodiscard]] inline bool IsValidRole(const Role& a_role)
    {
        if (a_role.id == 0 || a_role.name.empty() || a_role.name.size() > kMaximumRoleNameBytes
            || a_role.color > kMaximumRoleColor)
        {
            return false;
        }
        std::size_t characters = 0;
        for (const auto character : a_role.name)
        {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20 || byte == 0x7F)
            {
                return false;
            }
            characters += (byte & 0xC0) != 0x80 ? 1 : 0;
        }
        return characters <= kMaximumRoleNameCharacters;
    }
}
