#include "runtime/CodeDetour.h"

#include <array>
#include <cstring>

namespace CodeDetour
{
    std::uintptr_t WriteEntryDetour(
        REL::ID a_id, std::span<const std::uint8_t> a_prologue, std::uintptr_t a_thunk, std::string_view a_name)
    {
        const auto target = REL::Relocation<std::uintptr_t>{ a_id }.address();
        if (a_prologue.size() < kBranchSize
            || std::memcmp(reinterpret_cast<const void*>(target), a_prologue.data(), a_prologue.size()) != 0)
        {
            logs::error("Did not install {} hook: unexpected entry bytes at ID {}", a_name, a_id.id());
            return 0;
        }

        auto& trampoline = SKSE::GetTrampoline();
        auto* gateway = static_cast<std::uint8_t*>(trampoline.allocate(a_prologue.size() + kAbsoluteJumpSize));
        std::memcpy(gateway, a_prologue.data(), a_prologue.size());
        const std::array<std::uint8_t, 6> jump{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
        const std::uintptr_t resume = target + a_prologue.size();
        std::memcpy(gateway + a_prologue.size(), jump.data(), jump.size());
        std::memcpy(gateway + a_prologue.size() + jump.size(), &resume, sizeof(resume));

        // Pad the rest of a longer prologue so the patched entry disassembles
        // cleanly; execution never reaches these bytes.
        for (auto offset = kBranchSize; offset < a_prologue.size(); ++offset)
        {
            REL::safe_write(target + offset, std::uint8_t{ 0x90 });
        }
        (void)trampoline.write_branch<5>(target, a_thunk);
        return reinterpret_cast<std::uintptr_t>(gateway);
    }

    std::uintptr_t WriteCallDetour(
        REL::ID a_id, std::ptrdiff_t a_offset, std::uintptr_t a_thunk, std::string_view a_name)
    {
        const auto site = REL::Relocation<std::uintptr_t>{ a_id }.address() + a_offset;
        if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8)
        {
            logs::error("Did not install {} hook: no call at ID {} + {:#x}", a_name, a_id.id(), a_offset);
            return 0;
        }
        return SKSE::GetTrampoline().write_call<5>(site, a_thunk);
    }

    std::uintptr_t ReadCallTarget(std::uintptr_t a_site)
    {
        if (*reinterpret_cast<const std::uint8_t*>(a_site) != 0xE8)
        {
            return 0;
        }
        std::int32_t displacement = 0;
        std::memcpy(&displacement, reinterpret_cast<const void*>(a_site + 1), sizeof(displacement));
        return a_site + kBranchSize + displacement;
    }
}
