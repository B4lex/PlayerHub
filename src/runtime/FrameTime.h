#pragma once

#include <algorithm>

namespace FrameTime
{
    inline constexpr float kMaximumSeconds = 0.1F;

    // For timers, timeouts, and retry budgets: a hitch or a resumed pause counts as
    // one long frame at most.
    constexpr float Clamp(float a_delta) noexcept
    {
        return std::clamp(a_delta, 0.0F, kMaximumSeconds);
    }
}
