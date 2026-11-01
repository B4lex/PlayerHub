#include "sync/skeleton/PoseWindow.h"
#include "actor/GraphVocabulary.h"
#include "actor/ModdedGraph.h"
#include "config/PluginConfig.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PoseWindow
{
    namespace
    {
        constexpr float kGraphSampleSeconds = 0.1F;
        // A state machine switches state on the graph update after the event, and a
        // modded event can play its animation through vanilla states, so a window holds
        // this long without a modded state.
        constexpr float kMinimumWindowSeconds = 1.0F;
        // Poses past the modded state, so the receiver's blend-out starts from the
        // vanilla state its graph reproduces.
        constexpr float kTailSeconds = 0.25F;
        // A modded machine active this long outside windows belongs to the graph's
        // ordinary states, not to a behavior.
        constexpr float kAmbientMachineSeconds = 2.0F;
        constexpr std::size_t kMaximumEventName = 64;

        // What the graph input hook needs, published by the main loop whenever it changes.
        struct Vocabulary
        {
            std::vector<std::uint64_t> moddedEvents;
        };

        struct AmbientCandidate
        {
            std::uint64_t machine{ 0 };
            float seconds{ 0.0F };
        };

        // The local player's third-person graph, as the main loop tracks it.
        struct LocalGraph
        {
            // Compared only, never dereferenced.
            const RE::hkbBehaviorGraph* graph{ nullptr };
            bool vanillaPrefix{ false };
            std::vector<AmbientCandidate> candidates;
            std::vector<std::uint64_t> ambient;
            float sampleElapsed{ 0.0F };
        };

        struct Window
        {
            bool open{ false };
            std::string event;
            float elapsed{ 0.0F };
            float sampleElapsed{ 0.0F };
            float vanillaElapsed{ 0.0F };
            bool observedModded{ false };
        };

        std::mutex s_vocabularyLock;
        std::shared_ptr<const Vocabulary> s_vocabulary;

        // The first modded event the hook saw since the last drain, and how many.
        std::mutex s_observedLock;
        std::array<char, kMaximumEventName> s_observedName{};
        std::size_t s_observedLength = 0;
        std::uint32_t s_observedCount = 0;

        LocalGraph s_graph;
        Window s_window;
        std::vector<std::uint64_t> s_machines;
        std::optional<std::uint64_t> s_configRevision;
        bool s_sharing = false;

        bool IsSharing()
        {
            const auto revision = Config::GetRevision();
            if (s_configRevision != revision)
            {
                s_configRevision = revision;
                s_sharing = Config::Get()->advanced.shareCustomAnimations;
            }
            return s_sharing;
        }

        std::shared_ptr<const Vocabulary> LoadVocabulary()
        {
            const std::scoped_lock lock{ s_vocabularyLock };
            return s_vocabulary;
        }

        const char* FindBlock(RE::PlayerCharacter& a_player, bool a_transformed)
        {
            if (a_transformed)
            {
                return "shapeshift";
            }
            if (a_player.GetOccupiedFurniture())
            {
                return "furniture";
            }
            if (a_player.IsOnMount())
            {
                return "mount";
            }
            if (a_player.IsDead())
            {
                return "dead";
            }
            return nullptr;
        }

        void Close(const char* a_reason)
        {
            if (!s_window.open)
            {
                return;
            }
            logs::debug("Pose window closed: event={} reason={} seconds={:.3f} observedModded={}", s_window.event,
                a_reason, s_window.elapsed, s_window.observedModded);
            s_window = {};
            s_graph.candidates.clear();
            s_graph.sampleElapsed = 0.0F;
        }

        void ObserveGraph(RE::PlayerCharacter& a_player)
        {
            const auto layout = ModdedGraph::ReadLayout(a_player);
            if (layout.behavior == s_graph.graph)
            {
                return;
            }
            Close("graph-changed");
            s_graph = LocalGraph{ .graph = layout.behavior, .vanillaPrefix = layout.behavior && layout.vanillaPrefix };
            std::shared_ptr<const Vocabulary> vocabulary;
            if (s_graph.vanillaPrefix)
            {
                Vocabulary collected;
                ModdedGraph::CollectModdedEvents(a_player, layout.behavior, collected.moddedEvents);
                vocabulary = std::make_shared<const Vocabulary>(std::move(collected));
            }
            {
                const std::scoped_lock lock{ s_vocabularyLock };
                s_vocabulary = vocabulary;
            }
            logs::debug("Pose window observed local graph: vanillaPrefix={} moddedEvents={} activeGraph={}",
                s_graph.vanillaPrefix, vocabulary ? vocabulary->moddedEvents.size() : 0, layout.activeGraph);
        }

        void Open(RE::PlayerCharacter& a_player, std::string_view a_event, std::uint32_t a_count)
        {
            s_window = Window{ .open = true, .event = std::string{ a_event } };
            logs::debug("Pose window opened: event={} moddedEvents={} activeGraph={}", a_event, a_count,
                ModdedGraph::ReadLayout(a_player).activeGraph);
        }

        void ObserveWindow(RE::PlayerCharacter& a_player, float a_delta)
        {
            s_window.elapsed += a_delta;
            s_window.sampleElapsed += a_delta;
            if (s_window.sampleElapsed < kGraphSampleSeconds)
            {
                return;
            }
            const float seconds = std::exchange(s_window.sampleElapsed, 0.0F);
            const auto reading = ModdedGraph::ReadModdedState(a_player, s_graph.ambient);
            if (!reading.available)
            {
                return;
            }
            if (reading.modded)
            {
                if (!s_window.observedModded)
                {
                    logs::debug("Pose window entered modded state: machine={} state={} seconds={:.3f}",
                        reading.machine, reading.state, s_window.elapsed);
                }
                s_window.observedModded = true;
                s_window.vanillaElapsed = 0.0F;
                return;
            }
            if (!s_window.observedModded && s_window.elapsed < kMinimumWindowSeconds)
            {
                return;
            }
            s_window.vanillaElapsed += seconds;
            if (s_window.vanillaElapsed >= kTailSeconds)
            {
                Close(s_window.observedModded ? "graph-vanilla" : "graph-unobserved");
            }
        }

        void ObserveAmbientMachines(RE::PlayerCharacter& a_player, float a_delta)
        {
            s_graph.sampleElapsed += a_delta;
            if (!s_graph.vanillaPrefix || s_graph.sampleElapsed < kGraphSampleSeconds)
            {
                return;
            }
            const float seconds = std::exchange(s_graph.sampleElapsed, 0.0F);
            ModdedGraph::CollectModdedMachines(a_player, s_machines);
            std::erase_if(s_graph.candidates, [](const AmbientCandidate& a_candidate) {
                return std::ranges::find(s_machines, a_candidate.machine) == s_machines.end();
            });
            for (const auto machine : s_machines)
            {
                if (std::ranges::find(s_graph.ambient, machine) != s_graph.ambient.end())
                {
                    continue;
                }
                auto candidate = std::ranges::find(s_graph.candidates, machine, &AmbientCandidate::machine);
                if (candidate == s_graph.candidates.end())
                {
                    s_graph.candidates.push_back({ .machine = machine });
                    continue;
                }
                candidate->seconds += seconds;
                if (candidate->seconds >= kAmbientMachineSeconds)
                {
                    s_graph.ambient.push_back(machine);
                    logs::debug("Pose window treats an always active modded machine as ordinary: machine={:016X} "
                                "ambient={}",
                        machine, s_graph.ambient.size());
                }
            }
        }
    }

    bool IsModdedEvent(const RE::BSFixedString& a_eventName)
    {
        const std::string_view name{ a_eventName };
        if (name.empty())
        {
            return false;
        }
        const auto vocabulary = LoadVocabulary();
        return vocabulary && std::ranges::binary_search(vocabulary->moddedEvents, GraphVocabulary::HashName(name));
    }

    void ObserveLocalGraphEvent(const RE::BSFixedString& a_eventName, bool a_modded)
    {
        if (!a_modded)
        {
            return;
        }
        const std::string_view name{ a_eventName };
        const std::scoped_lock lock{ s_observedLock };
        if (s_observedCount++ == 0)
        {
            s_observedLength = (std::min)(name.size(), s_observedName.size());
            std::memcpy(s_observedName.data(), name.data(), s_observedLength);
        }
    }

    bool Update(const LocalFrame& a_local, bool a_transformed, bool a_graphEpochChanged, float a_delta)
    {
        PROFILE_FUNCTION();
        std::array<char, kMaximumEventName> observedName{};
        std::size_t observedLength = 0;
        std::uint32_t observedCount = 0;
        {
            const std::scoped_lock lock{ s_observedLock };
            observedName = s_observedName;
            observedLength = s_observedLength;
            observedCount = std::exchange(s_observedCount, 0);
        }
        const float delta = FrameTime::Clamp(a_delta);
        auto& player = a_local.player;
        if (a_graphEpochChanged)
        {
            Close("graph-epoch");
        }
        ObserveGraph(player);

        const char* blocked = IsSharing() ? FindBlock(player, a_transformed) : "disabled";
        if (blocked)
        {
            Close(blocked);
            return false;
        }
        // Repeats of a modded event extend nothing: the graph's modded state holds the window.
        if (!s_window.open && observedCount != 0 && s_graph.vanillaPrefix)
        {
            Open(player, { observedName.data(), observedLength }, observedCount);
            return true;
        }
        if (s_window.open)
        {
            ObserveWindow(player, delta);
        }
        else
        {
            ObserveAmbientMachines(player, delta);
        }
        return false;
    }

    void Discard()
    {
        Close("discarded");
        const std::scoped_lock lock{ s_observedLock };
        s_observedCount = 0;
    }

    bool IsOpen()
    {
        return s_window.open;
    }
}
