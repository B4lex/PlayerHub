#pragma once

#include "protocol/MovementState.h"

#include <RE/Skyrim.h>

struct RemoteActor;

namespace WeaponTransition
{
    struct State
    {
        // Weapon draw/sheathe can make Skyrim's graph leave its active locomotion branch.
        bool reassertPending{ false };
        bool completionObserved{ false };
        float waitElapsed{ 0.0F };
        bool animationObserved{ false };
        bool exitForced{ false };
        bool defaultStateDispatched{ false };
        bool replayBaseDispatched{ false };
        bool restoredWithoutCompletion{ false };
    };

    struct Progress
    {
        bool completed{ false };
        // The graph takes this update for the transition; locomotion waits for the next.
        bool settling{ false };
    };

    // Run before the pass's locomotion dispatch, after the landing phase.
    [[nodiscard]] Progress Advance(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime);

    struct Replay
    {
        Multiplayer::AnimationState state{};
        bool active{ false };
        bool replayingBase{ false };
        bool needsFollowup{ false };
    };

    [[nodiscard]] Replay PlanReplay(State& a_state, const Multiplayer::AnimationState& a_target,
        const Multiplayer::AnimationState& a_latched, bool a_completed);
    // Returns true once the replay restored the target and ended the transition.
    bool OnReplayLatched(RE::Actor& a_actor, RemoteActor& a_remote, const Replay& a_replay, bool a_eventTimedOut);

    [[nodiscard]] bool IsPlaying(const RemoteActor& a_remote);
    [[nodiscard]] bool IsUnsettled(const RemoteActor& a_remote);

    void RequestNativeTransition(RE::Actor& a_actor, bool a_weaponDrawn);
    void ApplyRiderWeaponState(RE::Actor& a_actor, RemoteActor& a_remote, bool a_weaponDrawn);
}
