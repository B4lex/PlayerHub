#pragma once

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>

#    include <atomic>
#    include <chrono>
#    include <cstddef>
#    include <cstdint>

namespace Profiling
{
    // A total that EndFrame plots once per main-loop pass and then resets.
    class FrameCounter
    {
    public:
        FrameCounter() noexcept;
        FrameCounter(const FrameCounter&) = delete;
        FrameCounter& operator=(const FrameCounter&) = delete;

        virtual void Plot() noexcept = 0;

    protected:
        ~FrameCounter() = default;
    };

    // Engine hooks that run for every actor on animation and AI threads are too frequent
    // for a zone per call, so each keeps totals instead.
    class HookCounter final : public FrameCounter
    {
    public:
        HookCounter(const char* a_callsPlot, const char* a_timePlot) noexcept
            : callsPlot(a_callsPlot)
            , timePlot(a_timePlot)
        {}

        void AddCall() noexcept { calls.fetch_add(1, std::memory_order_relaxed); }
        void AddTime(std::chrono::steady_clock::duration a_time) noexcept
        {
            nanoseconds.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(a_time).count(), std::memory_order_relaxed);
        }
        void Plot() noexcept override;

    private:
        const char* callsPlot;
        const char* timePlot;
        std::atomic<std::int64_t> calls{ 0 };
        std::atomic<std::int64_t> nanoseconds{ 0 };
    };

    class HookTimer
    {
    public:
        HookTimer(HookCounter& a_counter, bool a_call) noexcept
            : counter(a_counter)
        {
            if (a_call)
            {
                counter.AddCall();
            }
        }
        ~HookTimer() { counter.AddTime(std::chrono::steady_clock::now() - start); }
        HookTimer(const HookTimer&) = delete;
        HookTimer& operator=(const HookTimer&) = delete;

    private:
        HookCounter& counter;
        std::chrono::steady_clock::time_point start{ std::chrono::steady_clock::now() };
    };

    void CountAllocation(std::size_t a_size) noexcept;
    // Allocations made so far by the calling thread.
    [[nodiscard]] std::int64_t GetThreadAllocations() noexcept;

    // The calling thread's allocations within a scope, nested scopes included.
    class AllocationCounter final : public FrameCounter
    {
    public:
        explicit AllocationCounter(const char* a_plot) noexcept
            : plot(a_plot)
        {}

        void Add(std::int64_t a_count) noexcept { allocations.fetch_add(a_count, std::memory_order_relaxed); }
        void Plot() noexcept override;

    private:
        const char* plot;
        std::atomic<std::int64_t> allocations{ 0 };
    };

    class AllocationScope
    {
    public:
        explicit AllocationScope(AllocationCounter& a_counter) noexcept
            : counter(a_counter)
        {}
        ~AllocationScope() { counter.Add(GetThreadAllocations() - start); }
        AllocationScope(const AllocationScope&) = delete;
        AllocationScope& operator=(const AllocationScope&) = delete;

    private:
        AllocationCounter& counter;
        std::int64_t start{ GetThreadAllocations() };
    };

    // Main-loop thread, once per pass.
    void EndFrame() noexcept;
}

#    define PROFILE_FUNCTION() ZoneScoped
#    define PROFILE_SCOPE(a_name) ZoneScopedN(a_name)
#    define PROFILE_FRAME()          \
        do                           \
        {                            \
            ::Profiling::EndFrame(); \
            FrameMark;               \
        } while (false)
#    define PROFILE_PLOT(a_name, a_value) TracyPlot(a_name, static_cast<std::int64_t>(a_value))
#    define PROFILE_THREAD_NAME(a_name) tracy::SetThreadName(a_name)
// Tracy sends every lock event through one global mutex, which serializes the threads
// that take an instrumented lock and inflates their timings, so lock events are opt-in.
#    ifdef PLAYERHUB_TRACY_LOCKS
#        define PROFILE_LOCKABLE(a_type, a_name) TracyLockable(a_type, a_name)
#        define PROFILE_SHARED_LOCKABLE(a_type, a_name) TracySharedLockable(a_type, a_name)
#    else
#        define PROFILE_LOCKABLE(a_type, a_name) a_type a_name
#        define PROFILE_SHARED_LOCKABLE(a_type, a_name) a_type a_name
#    endif
#    define PROFILE_HOOK_COUNTER(a_var, a_name) \
        ::Profiling::HookCounter a_var{ "Hook " a_name " calls", "Hook " a_name " us" }
// Counts one call and times the rest of the scope; keep the engine's original call outside it.
#    define PROFILE_HOOK(a_counter) const ::Profiling::HookTimer profileHookTimer{ a_counter, true }
// Times a second stretch of the same call, after the engine's original call.
#    define PROFILE_HOOK_RESUME(a_counter) const ::Profiling::HookTimer profileHookResume{ a_counter, false }
#    define PROFILE_ALLOCATIONS(a_name)                                                            \
        static ::Profiling::AllocationCounter profileAllocationCounter{ "Allocations " a_name }; \
        const ::Profiling::AllocationScope profileAllocationScope{ profileAllocationCounter }
#else
#    define PROFILE_FUNCTION()
#    define PROFILE_SCOPE(a_name)
#    define PROFILE_FRAME()
#    define PROFILE_PLOT(a_name, a_value)
#    define PROFILE_THREAD_NAME(a_name)
#    define PROFILE_LOCKABLE(a_type, a_name) a_type a_name
#    define PROFILE_SHARED_LOCKABLE(a_type, a_name) a_type a_name
#    define PROFILE_HOOK_COUNTER(a_var, a_name) static_assert(true)
#    define PROFILE_HOOK(a_counter)
#    define PROFILE_HOOK_RESUME(a_counter)
#    define PROFILE_ALLOCATIONS(a_name)
#endif
