#pragma once

#if defined(TRACY_ENABLE) || defined(PLAYERHUB_TIME_BREAKDOWN)
#    define PLAYERHUB_HOOK_TIMING

#    include <array>
#    include <atomic>
#    include <chrono>
#    include <cstddef>
#    include <cstdint>
#    include <mutex>
#    include <span>

namespace HookTiming
{
    enum class Role : std::uint8_t
    {
        kMainThread,
        kOtherThread,
        // Inside a plugin time scope or another timed hook, whose time already counts.
        kNested,
    };
    inline constexpr std::size_t kRoleCount = 3;

    struct Totals
    {
        std::int64_t calls{ 0 };
        std::int64_t nanoseconds{ 0 };
    };

    class Counter
    {
    public:
        Counter(const char* a_name, const char* a_callsPlot, const char* a_timePlot) noexcept;
        Counter(const Counter&) = delete;
        Counter& operator=(const Counter&) = delete;

        void Add(Role a_role, bool a_call, std::chrono::steady_clock::duration a_time) noexcept;
        // Cumulative since load; readers keep their own previous totals.
        [[nodiscard]] Totals GetTotals(Role a_role) const noexcept;

        [[nodiscard]] const char* GetName() const noexcept { return name; }
        [[nodiscard]] const char* GetCallsPlot() const noexcept { return callsPlot; }
        [[nodiscard]] const char* GetTimePlot() const noexcept { return timePlot; }

    private:
        struct RoleTotals
        {
            std::atomic<std::int64_t> calls{ 0 };
            std::atomic<std::int64_t> nanoseconds{ 0 };
        };

        const char* name;
        const char* callsPlot;
        const char* timePlot;
        std::array<RoleTotals, kRoleCount> totals;
    };

    class Timer
    {
    public:
        Timer(Counter& a_counter, bool a_call) noexcept;
        ~Timer();
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;

    private:
        friend class Exclusion;

        Counter& counter;
        Role role;
        bool call;
        std::chrono::steady_clock::time_point start;
        std::chrono::steady_clock::duration excluded{};
    };

    // Leaves the engine's original call out of the enclosing timer; hooks it reaches count on their own.
    class Exclusion
    {
    public:
        explicit Exclusion(Timer& a_timer) noexcept;
        ~Exclusion();
        Exclusion(const Exclusion&) = delete;
        Exclusion& operator=(const Exclusion&) = delete;

    private:
        Timer& timer;
        std::chrono::steady_clock::time_point start;
    };

    // Counters are namespace-scope statics, so the list is complete once the plugin has loaded and
    // only grows; hold the lock while reading it.
    [[nodiscard]] std::mutex& GetCounterLock() noexcept;
    [[nodiscard]] std::span<Counter* const> GetCounters() noexcept;
}
#endif
