#pragma once

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

namespace IdlePresentation
{
    struct State
    {
        // From an accepted idle event until the graph is seen back in its default state.
        bool owned{ false };
        bool observedIdle{ false };
        float ownedElapsed{ 0.0F };
        float sampleElapsed{ 0.0F };
        // The held-idle instance this graph was given the entering event of; 0 for none.
        std::uint64_t appliedInstance{ 0 };
        // A release's event gets one graph update before locomotion.
        bool releaseSettling{ false };
    };

    // The graph, locomotion, weapon stance, and hands are settled, the actor's weapon
    // matches a_weaponDrawn, and nothing else owns the graph.
    [[nodiscard]] bool CanStart(const RemoteActor& a_remote, const RE::Actor& a_actor, bool a_weaponDrawn);
    [[nodiscard]] bool Dispatch(RemoteActor& a_remote, const RE::BSFixedString& a_event, bool a_exit);
    // Returns the actor's graph from an owned idle to its default state.
    void Release(RemoteActor& a_remote, const char* a_reason);
    // Ends ownership once the graph has left the idle. Run before the locomotion dispatch.
    void Advance(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime);
    // True once, for the pass after a release.
    [[nodiscard]] bool ConsumeReleaseSettle(RemoteActor& a_remote);

    [[nodiscard]] bool IsPlaying(const RemoteActor& a_remote);
    [[nodiscard]] std::uint64_t GetAppliedInstance(const RemoteActor& a_remote);
    void SetAppliedInstance(RemoteActor& a_remote, std::uint64_t a_instance);
}
