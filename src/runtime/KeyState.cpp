#include "runtime/KeyState.h"

#include <array>
#include <atomic>

namespace KeyState
{
    namespace
    {
        constexpr std::uint32_t kKeys = kMouseBase + kMouseButtons;

        std::array<std::atomic_bool, kKeys> s_held{};
    }

    void Record(std::uint32_t a_key, bool a_pressed)
    {
        if (a_key < kKeys)
        {
            s_held[a_key].store(a_pressed, std::memory_order_release);
        }
    }

    bool IsHeld(std::uint32_t a_key)
    {
        return a_key != 0 && a_key < kKeys && s_held[a_key].load(std::memory_order_acquire);
    }
}
