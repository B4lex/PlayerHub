#include "ui/InputLanguage.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string_view>

namespace UI::InputLanguage
{
    namespace
    {
        // BSWin32KeyboardDevice::Poll (vtable slot 2) turns each key into a
        // character with MapVirtualKeyA and ToUnicode, which use the calling
        // thread's layout. Poll also runs on job threads (seen while a plugin
        // window has focus), and Windows switches the input language only for
        // the thread that owns the game window, so those threads kept the old
        // layout. Poll holds each runtime's only call to either import.
        constexpr REL::RelocationID kPollID{ 67472, 68782 };
        constexpr std::ptrdiff_t kMapVirtualKeyCallOffset = 0xEC;
        constexpr std::ptrdiff_t kSEToUnicodeCallOffset = 0x20D;
        constexpr std::ptrdiff_t kAEToUnicodeCallOffset = 0x2CB;

        std::atomic<DWORD> s_windowThread{ 0 };
        std::atomic<HKL> s_loggedLayout{ nullptr };

        HKL WindowLayout()
        {
            auto thread = s_windowThread.load(std::memory_order_relaxed);
            if (thread == 0)
            {
                const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
                const auto window = renderer ? renderer->GetRuntimeData().renderWindows[0].hWnd : nullptr;
                if (!window)
                {
                    return ::GetKeyboardLayout(0);
                }
                thread = ::GetWindowThreadProcessId(reinterpret_cast<HWND>(window), nullptr);
                s_windowThread.store(thread, std::memory_order_relaxed);
            }
            return ::GetKeyboardLayout(thread);
        }

        UINT WINAPI MapVirtualKeyThunk(UINT a_code, UINT a_mapType)
        {
            return ::MapVirtualKeyExA(a_code, a_mapType, WindowLayout());
        }

        int WINAPI ToUnicodeThunk(
            UINT a_virtualKey, UINT a_scanCode, const BYTE* a_keyState, LPWSTR a_buffer, int a_size, UINT a_flags)
        {
            const auto layout = WindowLayout();
            const auto result = ::ToUnicodeEx(a_virtualKey, a_scanCode, a_keyState, a_buffer, a_size, a_flags, layout);
            if (s_loggedLayout.exchange(layout, std::memory_order_relaxed) != layout)
            {
                logs::debug("UI input: typing with keyboard layout {:#x} of the game window's thread; input thread "
                            "{} has {:#x}; first character U+{:04X}",
                    reinterpret_cast<std::uintptr_t>(layout), ::GetCurrentThreadId(),
                    reinterpret_cast<std::uintptr_t>(::GetKeyboardLayout(0)),
                    result > 0 ? static_cast<std::uint32_t>(a_buffer[0]) : 0U);
            }
            return result;
        }

        // Patches only a `call [rip+disp]` that still reads the user32 import,
        // so a site another plugin already took is left alone.
        bool WriteImportCall(std::ptrdiff_t a_offset, std::uintptr_t a_import, std::uintptr_t a_thunk,
            std::string_view a_name)
        {
            const auto site = REL::Relocation<std::uintptr_t>{ kPollID }.address() + a_offset;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(site);
            if (bytes[0] != 0xFF || bytes[1] != 0x15)
            {
                logs::error("UI input: did not patch {}: no indirect call at ID {} + {:#x}", a_name, kPollID.id(),
                    a_offset);
                return false;
            }
            std::int32_t displacement = 0;
            std::memcpy(&displacement, bytes + 2, sizeof(displacement));
            const auto slot = *reinterpret_cast<const std::uintptr_t*>(site + 6 + displacement);
            if (slot != a_import)
            {
                logs::warn("UI input: did not patch {}: the call no longer reaches user32", a_name);
                return false;
            }
            SKSE::GetTrampoline().write_call<6>(site, a_thunk);
            return true;
        }
    }

    void Install()
    {
        const auto mapVirtualKey = WriteImportCall(kMapVirtualKeyCallOffset,
            reinterpret_cast<std::uintptr_t>(&::MapVirtualKeyA), reinterpret_cast<std::uintptr_t>(&MapVirtualKeyThunk),
            "MapVirtualKeyA");
        const auto toUnicode = WriteImportCall(REL::Relocate(kSEToUnicodeCallOffset, kAEToUnicodeCallOffset),
            reinterpret_cast<std::uintptr_t>(&::ToUnicode), reinterpret_cast<std::uintptr_t>(&ToUnicodeThunk),
            "ToUnicode");
        logs::debug("UI input: keyboard text follows the game window's layout: mapVirtualKey={} toUnicode={}",
            mapVirtualKey, toUnicode);
    }
}
