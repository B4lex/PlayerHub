#pragma once

#include <REL/ID.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace CodeDetour
{
    // jmp [rip+0] followed by the 64-bit destination.
    inline constexpr std::size_t kAbsoluteJumpSize = 14;
    inline constexpr std::size_t kBranchSize = 5;

    // A gateway of the moved prologue plus the absolute jump write_branch<5> allocates.
    constexpr std::size_t EntryDetourTrampolineSize(std::size_t a_prologueSize)
    {
        return a_prologueSize + 2 * kAbsoluteJumpSize;
    }

    // The absolute jump write_call<5> allocates.
    inline constexpr std::size_t kCallDetourTrampolineSize = kAbsoluteJumpSize;

    [[nodiscard]] std::uintptr_t WriteEntryDetour(
        REL::ID a_id, std::span<const std::uint8_t> a_prologue, std::uintptr_t a_thunk, std::string_view a_name);

    [[nodiscard]] std::uintptr_t WriteCallDetour(
        REL::ID a_id, std::ptrdiff_t a_offset, std::uintptr_t a_thunk, std::string_view a_name);

    [[nodiscard]] std::uintptr_t ReadCallTarget(std::uintptr_t a_site);
}
