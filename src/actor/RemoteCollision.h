#pragma once

#include <RE/Skyrim.h>

#include <cstddef>
#include <string_view>

struct RemoteActor;

namespace RemoteCollision
{
    // Trampoline bytes InstallHooks needs: one call detour.
    inline constexpr std::size_t kTrampolineSize = 14;

    struct State
    {
        bool networkPlaced{ false };
        bool skeletonIsolated{ false };
        float skeletonRecheckElapsed{ 0.0F };
    };

    void InstallHooks();

    void PlaceByNetwork(RE::Actor& a_actor, RemoteActor& a_remote);

    void ReleaseToEngine(RE::Actor& a_actor, RemoteActor& a_remote, std::string_view a_owner, bool a_carried);

    // Skyrim may rewrite the bone bodies' filters around seating or rebuild the 3D.
    void IsolateCarriedSkeleton(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime);
}
