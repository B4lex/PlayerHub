#include "runtime/KeyState.h"

#include <array>
#include <atomic>

namespace KeyState
{
    namespace
    {
        // DirectInput scan codes fit a byte.
        constexpr std::uint32_t kKeys = 256;

        std::array<std::atomic_bool, kKeys> s_held{};
        std::array<std::atomic_uint32_t, kKeys> s_presses{};
    }

    void Record(std::uint32_t a_scanCode, bool a_pressed)
    {
        if (a_scanCode >= kKeys)
        {
            return;
        }
        if (a_pressed && !s_held[a_scanCode].exchange(true, std::memory_order_acq_rel))
        {
            s_presses[a_scanCode].fetch_add(1, std::memory_order_acq_rel);
        }
        else if (!a_pressed)
        {
            s_held[a_scanCode].store(false, std::memory_order_release);
        }
    }

    bool IsHeld(std::uint32_t a_scanCode)
    {
        return a_scanCode != 0 && a_scanCode < kKeys && s_held[a_scanCode].load(std::memory_order_acquire);
    }

    std::uint32_t GetPresses(std::uint32_t a_scanCode)
    {
        return a_scanCode < kKeys ? s_presses[a_scanCode].load(std::memory_order_acquire) : 0;
    }
}
