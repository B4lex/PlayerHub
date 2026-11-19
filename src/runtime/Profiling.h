#pragma once

#include "runtime/HookTiming.h"

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
#    define PROFILE_ALLOCATIONS(a_name)
#endif

// Engine hooks and plugin callbacks too frequent for a zone per call keep totals instead: Tracy
// plots them once per pass, and the time_breakdown option shows them in the plugin time breakdown.
#ifdef PLAYERHUB_HOOK_TIMING
#    define PROFILE_HOOK_COUNTER(a_var, a_name) \
        ::HookTiming::Counter a_var{ a_name, "Hook " a_name " calls", "Hook " a_name " us" }
// Counts one call and times the rest of the scope.
#    define PROFILE_HOOK(a_counter) ::HookTiming::Timer profileHookTimer{ a_counter, true }
// Times a second stretch of the same call.
#    define PROFILE_HOOK_RESUME(a_counter) const ::HookTiming::Timer profileHookResume{ a_counter, false }
// Evaluates the engine's original call outside the time PROFILE_HOOK measures in this scope.
#    define PROFILE_HOOK_ORIGINAL(a_call)                                            \
        [&]() -> decltype(auto) {                                                     \
            const ::HookTiming::Exclusion profileHookExclusion{ profileHookTimer }; \
            return a_call;                                                            \
        }()
#else
#    define PROFILE_HOOK_COUNTER(a_var, a_name) static_assert(true)
#    define PROFILE_HOOK(a_counter)
#    define PROFILE_HOOK_RESUME(a_counter)
#    define PROFILE_HOOK_ORIGINAL(a_call) a_call
#endif
