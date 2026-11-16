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
    }

    void Record(std::uint32_t a_scanCode, bool a_pressed)
    {
        if (a_scanCode < kKeys)
        {
            s_held[a_scanCode].store(a_pressed, std::memory_order_release);
        }
    }

    bool IsHeld(std::uint32_t a_scanCode)
    {
        return a_scanCode != 0 && a_scanCode < kKeys && s_held[a_scanCode].load(std::memory_order_acquire);
    }
}
