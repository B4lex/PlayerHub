#ifdef TRACY_ENABLE
#    include "runtime/Profiling.h"

#    include <mutex>
#    include <utility>
#    include <vector>

namespace Profiling
{
    namespace
    {
        // Counters register during module load (hooks) and on first use on any thread (allocations).
        std::mutex& CounterLock()
        {
            static std::mutex lock;
            return lock;
        }

        std::vector<FrameCounter*>& Counters()
        {
            static std::vector<FrameCounter*> counters;
            return counters;
        }

        std::atomic<std::int64_t> s_allocations{ 0 };
        std::atomic<std::int64_t> s_allocatedBytes{ 0 };
        thread_local std::int64_t t_allocations{ 0 };
    }

    FrameCounter::FrameCounter() noexcept
    {
        const std::scoped_lock lock{ CounterLock() };
        Counters().push_back(this);
    }

    void HookCounter::Plot() noexcept
    {
        TracyPlot(callsPlot, calls.exchange(0, std::memory_order_relaxed));
        TracyPlot(timePlot, static_cast<double>(nanoseconds.exchange(0, std::memory_order_relaxed)) / 1000.0);
    }

    void AllocationCounter::Plot() noexcept
    {
        TracyPlot(plot, allocations.exchange(0, std::memory_order_relaxed));
    }

    void CountAllocation(std::size_t a_size) noexcept
    {
        ++t_allocations;
        s_allocations.fetch_add(1, std::memory_order_relaxed);
        s_allocatedBytes.fetch_add(static_cast<std::int64_t>(a_size), std::memory_order_relaxed);
    }

    std::int64_t GetThreadAllocations() noexcept
    {
        return t_allocations;
    }

    void EndFrame() noexcept
    {
        {
            const std::scoped_lock lock{ CounterLock() };
            for (auto* counter : Counters())
            {
                counter->Plot();
            }
        }
        // Every plugin thread's allocations, not only the main loop's.
        TracyPlot("Plugin allocations", s_allocations.exchange(0, std::memory_order_relaxed));
        // Everything this thread allocated since the last pass, engine callbacks included.
        static std::int64_t s_previousThreadAllocations = t_allocations;
        const auto threadAllocations = t_allocations - std::exchange(s_previousThreadAllocations, t_allocations);
        TracyPlot("Main-loop thread allocations", threadAllocations);
        TracyPlot("Plugin allocated bytes", s_allocatedBytes.exchange(0, std::memory_order_relaxed));
    }
}
#endif
