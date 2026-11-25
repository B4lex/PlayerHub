#pragma once

#include "protocol/FacePose.h"

#include <RE/Skyrim.h>

#include <cstddef>
#include <optional>

struct RemoteActor;

namespace ActorFace
{
    // Trampoline bytes InstallHooks needs: one call detour.
    inline constexpr std::size_t kTrampolineSize = 14;

    void InstallHooks();

    // The weights the actor's head last showed, in wire order.
    [[nodiscard]] std::optional<Network::FaceWeights> ReadFace(RE::Actor& a_actor);

    // Shows a_weights on the head from its next face update until cleared, blended over the
    // head's own by a_weight; a phoneme shows the larger of the two.
    void SetFace(const RemoteActor& a_remote, RE::Actor& a_actor, const Network::FaceWeights& a_weights, float a_weight);
    // Gives the head its own weights back on its next face update.
    void ClearFace(const RemoteActor& a_remote);
    // Drops the face of an actor that is going away, without waiting for a face update.
    void ForgetFace(const RemoteActor& a_remote);
}
