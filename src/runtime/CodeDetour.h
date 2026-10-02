#pragma once

#include <REL/ID.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Detours for engine functions that have no virtual-table slot. Both write
// through the SKSE trampoline, so callers must reserve the sizes below.
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

    // Redirects a function's first instructions to a_thunk and returns the address
    // of a gateway that runs them and resumes the original function. The moved
    // instructions must be exactly a_prologue, at least five bytes, and position
    // independent. Returns 0 without patching if the bytes differ, for example on
    // another runtime or when another plugin already patched the function.
    [[nodiscard]] std::uintptr_t WriteEntryDetour(
        REL::ID a_id, std::span<const std::uint8_t> a_prologue, std::uintptr_t a_thunk, std::string_view a_name);

    // Redirects the 5-byte call at a_id + a_offset to a_thunk and returns the
    // previous destination, which may be another plugin's detour. Returns 0
    // without patching if the instruction there is not a relative call.
    [[nodiscard]] std::uintptr_t WriteCallDetour(
        REL::ID a_id, std::ptrdiff_t a_offset, std::uintptr_t a_thunk, std::string_view a_name);

    // The destination of the 5-byte relative call at a_site, or 0 if the
    // instruction there is not one.
    [[nodiscard]] std::uintptr_t ReadCallTarget(std::uintptr_t a_site);
}
