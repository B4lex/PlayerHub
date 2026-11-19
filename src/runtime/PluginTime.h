#pragma once

#include "runtime/HookTiming.h"

#include <chrono>
#include <optional>

#ifdef PLAYERHUB_TIME_BREAKDOWN
#    include <atomic>
#    include <cstdint>
#    include <vector>
#endif

namespace PluginTime
{
    class Scope
    {
    public:
        Scope() noexcept;
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        std::chrono::steady_clock::time_point start;
#ifdef PLAYERHUB_HOOK_TIMING
        bool outermost;
#endif
    };

#ifdef PLAYERHUB_HOOK_TIMING
    // Lets hook timers leave out calls whose time a scope already counts.
    [[nodiscard]] bool IsInsideScope() noexcept;
#endif

    void EndFrame();

    [[nodiscard]] std::optional<std::chrono::nanoseconds> GetAverageFrameTime();

#ifdef PLAYERHUB_TIME_BREAKDOWN
    class Stage
    {
    public:
        explicit Stage(const char* a_name) noexcept;
        Stage(const Stage&) = delete;
        Stage& operator=(const Stage&) = delete;

    private:
        friend class StageTimer;
        friend struct StageAccess;

        const char* name;
        // The stage open on the thread when this one first ran.
        const Stage* parent;
        std::atomic<std::int64_t> nanoseconds{ 0 };
    };

    class StageTimer
    {
    public:
        explicit StageTimer(Stage& a_stage) noexcept;
        ~StageTimer();
        StageTimer(const StageTimer&) = delete;
        StageTimer& operator=(const StageTimer&) = delete;

    private:
        Stage& stage;
        const Stage* previous;
        std::chrono::steady_clock::time_point start;
    };

    struct BreakdownRow
    {
        const char* name{ nullptr };
        std::uint8_t depth{ 0 };
        std::chrono::nanoseconds time{ 0 };
        std::optional<float> callsPerPass;
    };

    // Replaces both lists with the last window's breakdown in display order: the stage tree with the
    // main-thread hook total, and the hook totals per thread role with their costliest hooks.
    void CopyBreakdown(std::vector<BreakdownRow>& a_stages, std::vector<BreakdownRow>& a_hooks);
#endif
}

#define PLUGIN_TIME_CONCAT_INNER(a_left, a_right) a_left##a_right
#define PLUGIN_TIME_CONCAT(a_left, a_right) PLUGIN_TIME_CONCAT_INNER(a_left, a_right)
#ifdef PLAYERHUB_TIME_BREAKDOWN
#    define PLUGIN_TIME_STAGE(a_name)                                                     \
        static ::PluginTime::Stage PLUGIN_TIME_CONCAT(pluginTimeStage, __LINE__){ a_name }; \
        const ::PluginTime::StageTimer PLUGIN_TIME_CONCAT(pluginTimeStageTimer, __LINE__)   \
        {                                                                                   \
            PLUGIN_TIME_CONCAT(pluginTimeStage, __LINE__)                                   \
        }
#else
#    define PLUGIN_TIME_STAGE(a_name) static_assert(true)
#endif
#define PLUGIN_TIME_SCOPE(a_name)                                         \
    const ::PluginTime::Scope PLUGIN_TIME_CONCAT(pluginTimeScope, __LINE__); \
    PLUGIN_TIME_STAGE(a_name)
