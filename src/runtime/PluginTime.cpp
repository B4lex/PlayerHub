#include "runtime/PluginTime.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace PluginTime
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr auto kAverageWindowSeconds = std::chrono::milliseconds{ 100 };
        constexpr std::int64_t kNoAverage = -1;

        // The overlay's scope closes on the render path, which is not guaranteed
        // to be the main-loop thread.
        std::atomic<std::int64_t> s_windowNanoseconds{ 0 };
        std::atomic<std::int64_t> s_averageNanoseconds{ kNoAverage };

        // Main-loop thread only.
        std::optional<Clock::time_point> s_windowStart;
        std::int64_t s_windowFrames = 0;
    }

    Scope::Scope()
        : start(Clock::now())
    {
    }

    Scope::~Scope()
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
        s_windowNanoseconds.fetch_add(elapsed.count(), std::memory_order_relaxed);
    }

    void EndFrame()
    {
        const auto now = Clock::now();

        // Work before the first pass, such as startup and loading, has no frame
        // to belong to.
        if (!s_windowStart)
        {
            s_windowNanoseconds.store(0, std::memory_order_relaxed);
            s_windowStart = now;
            return;
        }

        ++s_windowFrames;
        if (now - *s_windowStart < kAverageWindowSeconds)
        {
            return;
        }

        const auto total = s_windowNanoseconds.exchange(0, std::memory_order_relaxed);
        s_averageNanoseconds.store(total / s_windowFrames, std::memory_order_relaxed);
        s_windowFrames = 0;
        s_windowStart = now;
    }

    std::optional<std::chrono::nanoseconds> GetAverageFrameTime()
    {
        const auto average = s_averageNanoseconds.load(std::memory_order_relaxed);
        if (average == kNoAverage)
        {
            return std::nullopt;
        }
        return std::chrono::nanoseconds{ average };
    }
}
