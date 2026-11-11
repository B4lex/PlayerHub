#include "sync/skeleton/PoseWindow.h"
#include "actor/GraphVocabulary.h"
#include "actor/ModdedGraph.h"
#include "config/PluginConfig.h"
#include "network/ServerClient.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
        // Poses past the modded state or reaction, so the receiver's blend-out starts
        // from the vanilla state its graph reproduces.
        constexpr float kTailSeconds = 0.25F;
        // A modded machine active this long outside windows belongs to the graph's
        // ordinary states, not to a behavior.
        constexpr float kAmbientMachineSeconds = 2.0F;
        constexpr std::size_t kMaximumEventName = 64;

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
            // The modded event or the reaction that opened the window.
            std::string trigger;
            float elapsed{ 0.0F };
            float sampleElapsed{ 0.0F };
            float inactiveElapsed{ 0.0F };
            bool observedModded{ false };
            bool observedReaction{ false };
        };

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

        const char* FindBlock(RE::PlayerCharacter& a_player, bool a_transformed)
        {
            // A window opened while disconnected is discarded on the same pass.
            if (!Network::IsConnected())
            {
                return "disconnected";
            }
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

        const char* FindReaction(RE::PlayerCharacter& a_player)
        {
            if (a_player.IsInKillMove())
            {
                return "kill-move";
            }
            const auto* actorState = a_player.AsActorState();
            if (actorState && actorState->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal)
            {
                return "knockdown";
            }
            if (actorState && actorState->IsStaggered())
            {
                return "stagger";
            }
            if (a_player.IsInRagdollState())
            {
                return "knockdown";
            }
            // The graph's output also follows a staggerStart a script sends straight to the graph.
            static const RE::BSFixedString isStaggeringVariable{ "IsStaggering" };
            bool staggering = false;
            if (a_player.GetGraphVariableBool(isStaggeringVariable, staggering) && staggering)
            {
                return "stagger";
            }
            return nullptr;
        }

        std::uint32_t ReadLifeState(RE::PlayerCharacter& a_player)
        {
            const auto* actorState = a_player.AsActorState();
            return actorState ? static_cast<std::uint32_t>(actorState->GetLifeState()) : 0;
        }

        void Close(const char* a_reason)
        {
            if (!s_window.open)
            {
                return;
            }
            logs::debug("Pose window closed: trigger={} reason={} seconds={:.3f} observedModded={} observedReaction={}",
                s_window.trigger, a_reason, s_window.elapsed, s_window.observedModded, s_window.observedReaction);
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
            logs::debug("Pose window observed local graph: vanillaPrefix={} appendedEvents={} activeGraph={}",
                s_graph.vanillaPrefix, layout.appendedEvents, layout.activeGraph);
        }

        void OpenForEvent(RE::PlayerCharacter& a_player, std::string_view a_event, std::uint32_t a_count)
        {
            s_window = Window{ .open = true, .trigger = std::string{ a_event } };
            logs::debug("Pose window opened: event={} moddedEvents={} activeGraph={}", a_event, a_count,
                ModdedGraph::ReadLayout(a_player).activeGraph);
        }

        void OpenForReaction(RE::PlayerCharacter& a_player, const char* a_reaction)
        {
            s_window = Window{ .open = true, .trigger = a_reaction, .observedReaction = true };
            logs::debug("Pose window opened: reaction={} lifeState={} ragdoll={} activeGraph={}", a_reaction,
                ReadLifeState(a_player), a_player.IsInRagdollState(), ModdedGraph::ReadLayout(a_player).activeGraph);
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
            const bool modded = reading.available && reading.modded;
            if (modded && !s_window.observedModded)
            {
                logs::debug("Pose window entered modded state: machine={} state={} seconds={:.3f}", reading.machine,
                    reading.state, s_window.elapsed);
            }
            s_window.observedModded = s_window.observedModded || modded;
            const char* reaction = FindReaction(a_player);
            if (reaction && !s_window.observedReaction)
            {
                logs::debug("Pose window entered reaction: reaction={} lifeState={} seconds={:.3f}", reaction,
                    ReadLifeState(a_player), s_window.elapsed);
            }
            s_window.observedReaction = s_window.observedReaction || reaction;
            if (modded || reaction)
            {
                s_window.inactiveElapsed = 0.0F;
                return;
            }
            // Reactions are actor state, readable when the graph is not.
            if (!reading.available && !s_window.observedReaction)
            {
                return;
            }
            if (!s_window.observedModded && !s_window.observedReaction && s_window.elapsed < kMinimumWindowSeconds)
            {
                return;
            }
            s_window.inactiveElapsed += seconds;
            if (s_window.inactiveElapsed >= kTailSeconds)
            {
                Close(s_window.observedModded ? "graph-vanilla"
                        : s_window.observedReaction ? "reaction-ended"
                                                    : "graph-unobserved");
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
        // The graph accepts the events of every behavior file it loaded, not only those
        // 0_master lists: Pandora wires an FNIS list in as a reference alone.
        const std::string_view name{ a_eventName };
        return !name.empty() && !GraphVocabulary::IsVanillaEvent(GraphVocabulary::HashName(name));
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
            OpenForEvent(player, { observedName.data(), observedLength }, observedCount);
            return true;
        }
        if (!s_window.open)
        {
            if (const char* reaction = FindReaction(player))
            {
                OpenForReaction(player, reaction);
                return true;
            }
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
