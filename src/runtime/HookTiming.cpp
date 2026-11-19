#include "runtime/HookTiming.h"

#ifdef PLAYERHUB_HOOK_TIMING
#    include "runtime/GameThread.h"
#    include "runtime/PluginTime.h"

#    include <vector>

namespace HookTiming
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // Registered from static initializers in other translation units.
        std::vector<Counter*>& Counters()
        {
            static std::vector<Counter*> counters;
            return counters;
        }

        thread_local int t_timerDepth{ 0 };

        Role GetCurrentRole() noexcept
        {
            if (t_timerDepth > 0 || PluginTime::IsInsideScope())
            {
                return Role::kNested;
            }
            return GameThread::IsAuthoritative() ? Role::kMainThread : Role::kOtherThread;
        }
    }

    Counter::Counter(const char* a_name, const char* a_callsPlot, const char* a_timePlot) noexcept
        : name(a_name)
        , callsPlot(a_callsPlot)
        , timePlot(a_timePlot)
    {
        const std::scoped_lock lock{ GetCounterLock() };
        Counters().push_back(this);
    }

    void Counter::Add(Role a_role, bool a_call, Clock::duration a_time) noexcept
    {
        auto& role = totals[static_cast<std::size_t>(a_role)];
        if (a_call)
        {
            role.calls.fetch_add(1, std::memory_order_relaxed);
        }
        role.nanoseconds.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(a_time).count(), std::memory_order_relaxed);
    }

    Totals Counter::GetTotals(Role a_role) const noexcept
    {
        const auto& role = totals[static_cast<std::size_t>(a_role)];
        return Totals{ .calls = role.calls.load(std::memory_order_relaxed),
            .nanoseconds = role.nanoseconds.load(std::memory_order_relaxed) };
    }

    Timer::Timer(Counter& a_counter, bool a_call) noexcept
        : counter(a_counter)
        , role(GetCurrentRole())
        , call(a_call)
    {
        ++t_timerDepth;
        start = Clock::now();
    }

    Timer::~Timer()
    {
        const auto elapsed = Clock::now() - start - excluded;
        --t_timerDepth;
        counter.Add(role, call, elapsed);
    }

    Exclusion::Exclusion(Timer& a_timer) noexcept
        : timer(a_timer)
        , start(Clock::now())
    {
        --t_timerDepth;
    }

    Exclusion::~Exclusion()
    {
        ++t_timerDepth;
        timer.excluded += Clock::now() - start;
    }

    std::mutex& GetCounterLock() noexcept
    {
        static std::mutex lock;
        return lock;
    }

    std::span<Counter* const> GetCounters() noexcept
    {
        return Counters();
    }
}
#endif
