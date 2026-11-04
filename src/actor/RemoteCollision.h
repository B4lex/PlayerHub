#pragma once

#include <RE/Skyrim.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

struct RemoteActor;

namespace RemoteCollision
{
    inline constexpr std::size_t kFilterInfoCallerCount = 7;
    // Trampoline bytes InstallHooks needs: the movement update's call detour and
    // one per collision filter info caller.
    inline constexpr std::size_t kTrampolineSize = (1 + kFilterInfoCallerCount) * 14;

    struct State
    {
        bool networkPlaced{ false };
        // Mirrors the registry's solid character group.
        std::uint16_t solidGroup{ 0 };
        bool skeletonIsolated{ false };
        float skeletonRecheckElapsed{ 0.0F };
    };

    void InstallHooks();

    // Main loop, each pass: the local player's controller, which solid remote
    // bodies collide with.
    void TrackLocalPlayer(RE::PlayerCharacter& a_player);

    void PlaceByNetwork(RE::Actor& a_actor, RemoteActor& a_remote);

    void ReleaseToEngine(RE::Actor& a_actor, RemoteActor& a_remote, std::string_view a_owner, bool a_carried);

    // Skyrim may rewrite the bone bodies' filters around seating or rebuild the 3D.
    void IsolateCarriedSkeleton(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime);
}
