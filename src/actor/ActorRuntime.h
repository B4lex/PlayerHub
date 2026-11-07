#pragma once

#include "protocol/FlyState.h"
#include "protocol/MovementState.h"

#include <optional>

struct RemoteActor;

namespace ActorMount
{
    struct Gait;
}

namespace ActorFlight
{
    struct Flight;
}

namespace ActorRuntime
{
    // a_presentableState is the network state the actor should show, or none while
    // furniture owns it.
    [[nodiscard]] bool Update(
        RemoteActor& a_remote, float a_deltaTime, std::optional<Multiplayer::AnimationState> a_presentableState);

    // A flying mount's rider takes no gait.
    [[nodiscard]] bool UpdateRider(RemoteActor& a_remote, ActorMount::Gait& a_gait, float a_mountSpeed,
        bool a_flyingMount, bool a_weaponDrawn, float a_deltaTime);

    // a_flight is set for a flying mount, which flies in a_flyState with a_wing's strokes
    // instead of taking a gait.
    [[nodiscard]] bool UpdateMount(RemoteActor& a_remote, ActorMount::Gait& a_gait, ActorFlight::Flight* a_flight,
        Multiplayer::FlyState a_flyState, Multiplayer::WingState a_wing, float a_deltaTime);
}
