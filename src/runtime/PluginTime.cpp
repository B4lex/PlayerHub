#include "runtime/PluginTime.h"

#include <atomic>
#include <chrono>
#include <cstdint>

#ifdef PLAYERHUB_TIME_BREAKDOWN
#    include "runtime/HookTiming.h"

#    include <algorithm>
#    include <cstddef>
#    include <mutex>
#endif

namespace PluginTime
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr auto kAverageWindowSeconds = std::chrono::milliseconds{ 100 };
        constexpr std::int64_t kNoAverage = -1;

        // The overlay's render path is not guaranteed to run on the main-loop thread.
        std::atomic<std::int64_t> s_windowNanoseconds{ 0 };
        std::atomic<std::int64_t> s_averageNanoseconds{ kNoAverage };

        std::optional<Clock::time_point> s_windowStart;
        std::int64_t s_windowFrames = 0;

#ifdef PLAYERHUB_HOOK_TIMING
        thread_local int t_scopeDepth{ 0 };
#endif
    }

#ifdef PLAYERHUB_TIME_BREAKDOWN
    struct StageAccess
    {
        static const char* GetName(const Stage& a_stage) { return a_stage.name; }
        static const Stage* GetParent(const Stage& a_stage) { return a_stage.parent; }
        static std::int64_t TakeNanoseconds(Stage& a_stage)
        {
            return a_stage.nanoseconds.exchange(0, std::memory_order_relaxed);
        }
    };

    namespace
    {
        constexpr std::size_t kMainThreadHookRows = 5;
        constexpr std::size_t kOtherThreadHookRows = 8;
        constexpr std::int64_t kMinimumRowNanoseconds = 5'000;
        constexpr const char* kHooksStageRow = "Hooks";
        constexpr const char* kMainThreadHooksRow = "Main thread";
        constexpr const char* kOtherThreadsRow = "Off-thread";
        constexpr const char* kRestRow = "other";

        thread_local const Stage* t_currentStage{ nullptr };

        // Stages register on first use, from whichever thread runs them.
        std::mutex& StageLock()
        {
            static std::mutex lock;
            return lock;
        }

        std::vector<Stage*>& Stages()
        {
            static std::vector<Stage*> stages;
            return stages;
        }

        struct HookBaseline
        {
            HookTiming::Totals mainThread;
            HookTiming::Totals otherThread;
        };

        struct HookSample
        {
            const char* name{ nullptr };
            std::int64_t calls{ 0 };
            std::int64_t nanoseconds{ 0 };
        };

        // Main-loop thread only; reused across windows.
        std::vector<HookBaseline> s_hookBaselines;
        std::vector<HookSample> s_mainThreadHooks;
        std::vector<HookSample> s_otherThreadHooks;
        std::vector<std::int64_t> s_stageNanoseconds;
        std::vector<BreakdownRow> s_stageRows;
        std::vector<BreakdownRow> s_hookRows;

        std::mutex s_publishedLock;
        std::vector<BreakdownRow> s_publishedStages;
        std::vector<BreakdownRow> s_publishedHooks;

        HookTiming::Totals TakeDelta(HookTiming::Totals& a_baseline, const HookTiming::Totals& a_current)
        {
            const HookTiming::Totals delta{ .calls = a_current.calls - a_baseline.calls,
                .nanoseconds = a_current.nanoseconds - a_baseline.nanoseconds };
            a_baseline = a_current;
            return delta;
        }

        // Returns the main-loop thread's hook time outside plugin time scopes, which plugin time adds.
        std::int64_t SampleHooks()
        {
            s_mainThreadHooks.clear();
            s_otherThreadHooks.clear();
            std::int64_t mainThreadNanoseconds = 0;

            const std::scoped_lock lock{ HookTiming::GetCounterLock() };
            const auto counters = HookTiming::GetCounters();
            s_hookBaselines.resize(counters.size());
            for (std::size_t index = 0; index < counters.size(); ++index)
            {
                const auto& counter = *counters[index];
                auto& baseline = s_hookBaselines[index];
                const auto mainThread
                    = TakeDelta(baseline.mainThread, counter.GetTotals(HookTiming::Role::kMainThread));
                const auto otherThread
                    = TakeDelta(baseline.otherThread, counter.GetTotals(HookTiming::Role::kOtherThread));
                if (mainThread.calls != 0)
                {
                    s_mainThreadHooks.push_back({ counter.GetName(), mainThread.calls, mainThread.nanoseconds });
                    mainThreadNanoseconds += mainThread.nanoseconds;
                }
                if (otherThread.calls != 0)
                {
                    s_otherThreadHooks.push_back({ counter.GetName(), otherThread.calls, otherThread.nanoseconds });
                }
            }
            return mainThreadNanoseconds;
        }

        void ResetStages()
        {
            const std::scoped_lock lock{ StageLock() };
            for (auto* stage : Stages())
            {
                (void)StageAccess::TakeNanoseconds(*stage);
            }
        }

        void AddRow(std::vector<BreakdownRow>& a_rows, const char* a_name, std::uint8_t a_depth,
            std::int64_t a_nanoseconds, std::optional<float> a_callsPerPass = std::nullopt)
        {
            a_rows.push_back({ .name = a_name,
                .depth = a_depth,
                .time = std::chrono::nanoseconds{ a_nanoseconds },
                .callsPerPass = a_callsPerPass });
        }

        // Every registered stage gets a row, in registration order, so the panel keeps its rows and order.
        void AddStageRows(const Stage* a_parent, std::uint8_t a_depth, std::int64_t a_frames)
        {
            const auto& stages = Stages();
            for (std::size_t index = 0; index < stages.size(); ++index)
            {
                if (StageAccess::GetParent(*stages[index]) != a_parent)
                {
                    continue;
                }
                const auto average = s_stageNanoseconds[index] / a_frames;
                AddRow(s_stageRows, StageAccess::GetName(*stages[index]), a_depth, average);
                const auto firstChild = s_stageRows.size();
                AddStageRows(stages[index], static_cast<std::uint8_t>(a_depth + 1), a_frames);
                if (s_stageRows.size() == firstChild)
                {
                    continue;
                }
                auto rest = average;
                for (auto child = firstChild; child < s_stageRows.size(); ++child)
                {
                    if (s_stageRows[child].depth == a_depth + 1)
                    {
                        rest -= s_stageRows[child].time.count();
                    }
                }
                // Integer averages can leave the children a few nanoseconds over their parent.
                AddRow(s_stageRows, kRestRow, static_cast<std::uint8_t>(a_depth + 1),
                    (std::max)(rest, std::int64_t{ 0 }));
            }
        }

        std::int64_t SumNanoseconds(const std::vector<HookSample>& a_hooks)
        {
            std::int64_t total = 0;
            for (const auto& hook : a_hooks)
            {
                total += hook.nanoseconds;
            }
            return total;
        }

        void AddHookRows(
            const char* a_name, std::vector<HookSample>& a_hooks, std::size_t a_shown, std::int64_t a_frames)
        {
            const auto total = SumNanoseconds(a_hooks);
            AddRow(s_hookRows, a_name, 0, total / a_frames);

            std::ranges::sort(a_hooks, std::ranges::greater{}, &HookSample::nanoseconds);
            std::int64_t rest = total;
            for (std::size_t index = 0; index < (std::min)(a_shown, a_hooks.size()); ++index)
            {
                const auto& hook = a_hooks[index];
                const auto average = hook.nanoseconds / a_frames;
                if (average < kMinimumRowNanoseconds)
                {
                    break;
                }
                AddRow(s_hookRows, hook.name, 1, average,
                    static_cast<float>(hook.calls) / static_cast<float>(a_frames));
                rest -= hook.nanoseconds;
            }
            if (rest != total)
            {
                AddRow(s_hookRows, kRestRow, 1, rest / a_frames);
            }
        }

        void PublishBreakdown(std::int64_t a_frames)
        {
            s_stageRows.clear();
            s_hookRows.clear();
            {
                const std::scoped_lock lock{ StageLock() };
                const auto& stages = Stages();
                s_stageNanoseconds.resize(stages.size());
                for (std::size_t index = 0; index < stages.size(); ++index)
                {
                    s_stageNanoseconds[index] = StageAccess::TakeNanoseconds(*stages[index]);
                }
                AddStageRows(nullptr, 1, a_frames);
            }
            // The main-thread hook total stays in the stage list, so the list adds up to plugin time.
            AddRow(s_stageRows, kHooksStageRow, 1, SumNanoseconds(s_mainThreadHooks) / a_frames);
            AddHookRows(kMainThreadHooksRow, s_mainThreadHooks, kMainThreadHookRows, a_frames);
            AddHookRows(kOtherThreadsRow, s_otherThreadHooks, kOtherThreadHookRows, a_frames);

            const std::scoped_lock lock{ s_publishedLock };
            s_publishedStages.assign(s_stageRows.begin(), s_stageRows.end());
            s_publishedHooks.assign(s_hookRows.begin(), s_hookRows.end());
        }
    }

    Stage::Stage(const char* a_name) noexcept
        : name(a_name)
        , parent(t_currentStage)
    {
        const std::scoped_lock lock{ StageLock() };
        Stages().push_back(this);
    }

    StageTimer::StageTimer(Stage& a_stage) noexcept
        : stage(a_stage)
        , previous(t_currentStage)
        , start(Clock::now())
    {
        t_currentStage = &stage;
    }

    StageTimer::~StageTimer()
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
        stage.nanoseconds.fetch_add(elapsed.count(), std::memory_order_relaxed);
        t_currentStage = previous;
    }

    void CopyBreakdown(std::vector<BreakdownRow>& a_stages, std::vector<BreakdownRow>& a_hooks)
    {
        const std::scoped_lock lock{ s_publishedLock };
        a_stages.assign(s_publishedStages.begin(), s_publishedStages.end());
        a_hooks.assign(s_publishedHooks.begin(), s_publishedHooks.end());
    }
#endif

    Scope::Scope() noexcept
        : start(Clock::now())
#ifdef PLAYERHUB_HOOK_TIMING
        , outermost(t_scopeDepth++ == 0)
#endif
    {
    }

    Scope::~Scope()
    {
#ifdef PLAYERHUB_HOOK_TIMING
        --t_scopeDepth;
        if (!outermost)
        {
            return;
        }
#endif
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
        s_windowNanoseconds.fetch_add(elapsed.count(), std::memory_order_relaxed);
    }

#ifdef PLAYERHUB_HOOK_TIMING
    bool IsInsideScope() noexcept
    {
        return t_scopeDepth > 0;
    }
#endif

    void EndFrame()
    {
        const auto now = Clock::now();

        if (!s_windowStart)
        {
            s_windowNanoseconds.store(0, std::memory_order_relaxed);
#ifdef PLAYERHUB_TIME_BREAKDOWN
            (void)SampleHooks();
            ResetStages();
#endif
            s_windowStart = now;
            return;
        }

        ++s_windowFrames;
        if (now - *s_windowStart < kAverageWindowSeconds)
        {
            return;
        }

        auto total = s_windowNanoseconds.exchange(0, std::memory_order_relaxed);
#ifdef PLAYERHUB_TIME_BREAKDOWN
        total += SampleHooks();
        PublishBreakdown(s_windowFrames);
#endif
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
