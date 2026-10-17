#pragma once

#include "protocol/Shapeshift.h"

#include <cstdint>

struct RemoteActor;

namespace ActorShapeshift
{
    struct State
    {
        bool levitationTarget{ false };
        float levitationSampleElapsed{ 0.0F };
        float levitationMismatchElapsed{ 0.0F };
        float levitationToggleElapsed{ 0.0F };
        std::uint32_t levitationToggles{ 0 };
        bool castRequested{ false };
        bool mistFormShown{ false };
        bool batsShown{ false };
    };

    [[nodiscard]] bool DispatchAction(RemoteActor& a_remote, Multiplayer::BeastAction a_action);

    // A vampire lord only.
    void ReconcileLevitation(RemoteActor& a_remote, bool a_levitating, bool a_moving, float a_deltaTime);
    void ApplyCastRequests(RemoteActor& a_remote, bool a_castingLeft, bool a_castingRight);
    void ApplyVampireLordPowers(RemoteActor& a_remote, bool a_mistForm, bool a_bats);
}
