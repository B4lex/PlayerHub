#include "sync/customanimation/GraphEventSurvey.h"

#ifndef NDEBUG
#include "actor/GraphProbe.h"
#include "runtime/FrameTime.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GraphEventSurvey
{
    namespace
    {
        constexpr std::size_t kMaximumNames = 1024;
        constexpr std::size_t kMaximumPendingSamples = 64;
        // Lets the transition the event starts activate its clips.
        constexpr float kClipSampleDelaySeconds = 0.3F;

        struct NameHash
        {
            using is_transparent = void;

            std::size_t operator()(std::string_view a_name) const noexcept
            {
                return std::hash<std::string_view>{}(a_name);
            }
        };

        struct PendingSample
        {
            std::string eventName;
            float remainingSeconds{ kClipSampleDelaySeconds };
        };

        std::mutex s_lock;
        std::unordered_set<std::string, NameHash, std::equal_to<>> s_seen;
        std::vector<std::string> s_firstSeen;

        std::vector<std::string> s_drained;
        std::vector<PendingSample> s_pending;
    }

    void ObserveLocalGraphEvent(const RE::BSFixedString& a_eventName)
    {
        const std::string_view name{ a_eventName };
        if (name.empty())
        {
            return;
        }
        const std::scoped_lock lock{ s_lock };
        if (s_seen.size() >= kMaximumNames || s_seen.contains(name))
        {
            return;
        }
        s_seen.emplace(name);
        s_firstSeen.emplace_back(name);
    }

    void LogLocalGraphEvents(const LocalFrame& a_local, float a_delta)
    {
        {
            const std::scoped_lock lock{ s_lock };
            s_drained.swap(s_firstSeen);
        }
        for (auto& name : s_drained)
        {
            if (s_pending.size() < kMaximumPendingSamples)
            {
                s_pending.push_back({ .eventName = std::move(name) });
            }
        }
        s_drained.clear();

        const float delta = FrameTime::Clamp(a_delta);
        std::erase_if(s_pending, [&](PendingSample& a_sample) {
            a_sample.remainingSeconds -= delta;
            if (a_sample.remainingSeconds > 0.0F)
            {
                return false;
            }
            logs::debug("Local graph event first seen: event={} {}", a_sample.eventName,
                GraphProbe::DescribeActiveClips(a_local.player));
            return true;
        });
    }
}
#endif
