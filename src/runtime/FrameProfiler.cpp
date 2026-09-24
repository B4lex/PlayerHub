#include "runtime/FrameProfiler.h"

#ifdef SKYRIM_MP_FRAME_PROFILER

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <string>

namespace FrameProfiler
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // Log throttling only, so a frame count rather than a duration.
        constexpr std::uint32_t kReportIntervalFrames = 200;

        constexpr auto kSectionCount = static_cast<std::size_t>(Section::Count);
        constexpr std::array<const char*, kSectionCount> kSectionNames{ "mainLoop", "overlay" };

        struct Durations
        {
            std::int64_t totalNanoseconds{ 0 };
            std::int64_t maximumNanoseconds{ 0 };

            void Add(std::int64_t a_nanoseconds)
            {
                totalNanoseconds += a_nanoseconds;
                maximumNanoseconds = (std::max)(maximumNanoseconds, a_nanoseconds);
            }
        };

        // The overlay section closes on the render path, which is not guaranteed
        // to be the main-loop thread.
        std::array<std::atomic<std::int64_t>, kSectionCount> s_frameNanoseconds{};

        // Accessed only on the main-loop thread.
        std::array<Durations, kSectionCount> s_sections{};
        Durations s_plugin;
        std::uint32_t s_frames = 0;
        std::optional<Clock::time_point> s_windowStart;

        double ToMilliseconds(std::int64_t a_nanoseconds)
        {
            return static_cast<double>(a_nanoseconds) / 1'000'000.0;
        }

        void Report(Clock::time_point a_now)
        {
            const auto windowNanoseconds
                = std::chrono::duration_cast<std::chrono::nanoseconds>(a_now - *s_windowStart).count();
            const auto averageFrameNanoseconds = windowNanoseconds / s_frames;
            const auto averagePluginNanoseconds = s_plugin.totalNanoseconds / s_frames;
            const double pluginShare = averageFrameNanoseconds > 0
                ? 100.0 * static_cast<double>(averagePluginNanoseconds) / static_cast<double>(averageFrameNanoseconds)
                : 0.0;

            std::string sections;
            for (std::size_t index = 0; index < kSectionCount; ++index)
            {
                const auto& section = s_sections[index];
                std::format_to(std::back_inserter(sections), " {}Average={:.3f}ms {}Maximum={:.3f}ms",
                    kSectionNames[index], ToMilliseconds(section.totalNanoseconds / s_frames), kSectionNames[index],
                    ToMilliseconds(section.maximumNanoseconds));
            }

            logs::info(
                "Frame profiler: frames={} averageFrame={:.3f}ms pluginAverage={:.3f}ms ({:.2f}%) "
                "pluginMaximum={:.3f}ms{}",
                s_frames, ToMilliseconds(averageFrameNanoseconds), ToMilliseconds(averagePluginNanoseconds),
                pluginShare, ToMilliseconds(s_plugin.maximumNanoseconds), sections);
        }
    }

    ScopedSection::ScopedSection(Section a_section)
        : section(a_section)
        , start(Clock::now())
    {
    }

    ScopedSection::~ScopedSection()
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        s_frameNanoseconds[static_cast<std::size_t>(section)].fetch_add(elapsed, std::memory_order_relaxed);
    }

    void EndFrame()
    {
        const auto now = Clock::now();
        std::array<std::int64_t, kSectionCount> frame{};
        for (std::size_t index = 0; index < kSectionCount; ++index)
        {
            frame[index] = s_frameNanoseconds[index].exchange(0, std::memory_order_relaxed);
        }

        // Work before the first pass, such as startup and loading, has no frame
        // to belong to.
        if (!s_windowStart)
        {
            s_windowStart = now;
            logs::info("Frame profiler: enabled, reporting every {} main-loop passes", kReportIntervalFrames);
            return;
        }

        std::int64_t pluginNanoseconds = 0;
        for (std::size_t index = 0; index < kSectionCount; ++index)
        {
            s_sections[index].Add(frame[index]);
            pluginNanoseconds += frame[index];
        }
        s_plugin.Add(pluginNanoseconds);

        if (++s_frames < kReportIntervalFrames)
        {
            return;
        }

        Report(now);
        s_sections = {};
        s_plugin = {};
        s_frames = 0;
        s_windowStart = now;
    }
}

#endif
