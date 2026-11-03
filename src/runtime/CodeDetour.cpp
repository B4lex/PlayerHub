#include "runtime/CodeDetour.h"

#include <array>
#include <cstring>

REX_W32_IMPORT(void*, RtlLookupFunctionEntry, std::uint64_t, std::uint64_t*, void*);

namespace CodeDetour
{
    namespace
    {
        // The x64 RUNTIME_FUNCTION and UNWIND_INFO layouts. A chunk whose unwind
        // info has UNW_FLAG_CHAININFO continues the function of the RUNTIME_FUNCTION
        // stored after its unwind codes, whose count is rounded up to even.
        struct RuntimeFunction
        {
            std::uint32_t begin;
            std::uint32_t end;
            std::uint32_t unwindInfo;
        };

        constexpr std::uint8_t kUnwindChainInfo = 0x4;

        const RuntimeFunction* LookupFunctionEntry(std::uintptr_t a_address, std::uintptr_t& a_imageBase)
        {
            std::uint64_t imageBase = 0;
            const auto* entry = static_cast<const RuntimeFunction*>(
                ::W32_IMPL_RtlLookupFunctionEntry(static_cast<std::uint64_t>(a_address), &imageBase, nullptr));
            a_imageBase = static_cast<std::uintptr_t>(imageBase);
            return entry;
        }

        const RuntimeFunction* FindRootEntry(const RuntimeFunction* a_entry, std::uintptr_t a_imageBase)
        {
            while (true)
            {
                const auto* unwind = reinterpret_cast<const std::uint8_t*>(a_imageBase + a_entry->unwindInfo);
                if (((unwind[0] >> 3) & kUnwindChainInfo) == 0)
                {
                    return a_entry;
                }
                const std::size_t codes = (unwind[2] + 1u) & ~1u;
                a_entry = reinterpret_cast<const RuntimeFunction*>(unwind + 4 + codes * sizeof(std::uint16_t));
            }
        }

        // The end of the code contiguous with a_address that belongs to its
        // function: its own chunk and the chunks right after it that chain to the
        // same function, as the compiler emits a function split around a call. 0
        // when the address has no function entry.
        std::uintptr_t FindFunctionEnd(std::uintptr_t a_address)
        {
            std::uintptr_t imageBase = 0;
            const auto* entry = LookupFunctionEntry(a_address, imageBase);
            if (!entry)
            {
                return 0;
            }
            const auto rootBegin = FindRootEntry(entry, imageBase)->begin;
            auto end = imageBase + entry->end;
            while (const auto* next = LookupFunctionEntry(end, imageBase))
            {
                if (next->begin != end - imageBase || FindRootEntry(next, imageBase)->begin != rootBegin)
                {
                    break;
                }
                end = imageBase + next->end;
            }
            return end;
        }
    }

    std::uintptr_t WriteEntryDetour(REL::ID a_id, std::span<const std::uint8_t> a_prologue, std::uintptr_t a_thunk,
        std::string_view a_name, std::ptrdiff_t a_offset)
    {
        const auto target = REL::Relocation<std::uintptr_t>{ a_id }.address() + a_offset;
        if (a_prologue.size() < kBranchSize
            || std::memcmp(reinterpret_cast<const void*>(target), a_prologue.data(), a_prologue.size()) != 0)
        {
            logs::error(
                "Did not install {} hook: unexpected entry bytes at ID {} + {:#x}", a_name, a_id.id(), a_offset);
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

    std::uintptr_t WriteCalleeDetour(
        REL::ID a_caller, REL::ID a_callee, std::uintptr_t a_thunk, std::string_view a_name)
    {
        const auto caller = REL::Relocation<std::uintptr_t>{ a_caller }.address();
        const auto callee = REL::Relocation<std::uintptr_t>{ a_callee }.address();
        const auto end = FindFunctionEnd(caller);
        if (end == 0)
        {
            logs::error("Did not install {} hook: no function entry for ID {}", a_name, a_caller.id());
            return 0;
        }
        for (auto site = caller; site + kBranchSize <= end; ++site)
        {
            if (ReadCallTarget(site) == callee)
            {
                logs::debug("Found {} call at ID {} + {:#x}", a_name, a_caller.id(), site - caller);
                return SKSE::GetTrampoline().write_call<5>(site, a_thunk);
            }
        }
        logs::error("Did not install {} hook: no call to ID {} in ID {}", a_name, a_callee.id(), a_caller.id());
        return 0;
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
