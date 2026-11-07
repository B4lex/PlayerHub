#pragma once

#include "protocol/FlyState.h"

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

// Drives a remote flying mount's own flight graph from its source's fly state.
namespace ActorFlight
{
    struct Flight
    {
        Multiplayer::FlyState requested{ Multiplayer::FlyState::kNone };
        bool started{ false };
        float requestedElapsed{ 0.0F };
        float reconcileElapsed{ 0.0F };
        // Seconds between graph checks, longer after a rejected correction.
        float reconcileInterval{ 0.25F };
        // A transition event the graph rejected, sent again until it takes it.
        const char* pendingEvent{ nullptr };
        float flapThreshold{ 0.0F };
        float featherThreshold{ 0.0F };
        bool hasWingThresholds{ false };
        float previousTargetPitch{ 0.0F };
        bool hasPreviousTargetPitch{ false };
        float pitchRate{ 0.0F };
        bool moving{ false };
        std::uint32_t rejectedEvents{ 0 };
        std::uint32_t corrections{ 0 };
        float diagnosticElapsed{ 0.0F };
    };

    // The local player's mount's wing stroke while it cruises.
    [[nodiscard]] Multiplayer::WingState ReadWingState(RE::Actor& a_mount, Multiplayer::FlyState a_state);

    // a_speed is the source mount's speed along its flight.
    void Apply(RE::Actor& a_actor, const RemoteActor& a_remote, Flight& a_flight, Multiplayer::FlyState a_state,
        Multiplayer::WingState a_wing, float a_speed, float a_deltaTime);
}
