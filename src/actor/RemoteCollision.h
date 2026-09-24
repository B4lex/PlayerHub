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
        // Mirrors the registration's network placement.
        bool networkPlaced{ false };
    };

    // Skips Skyrim's movement update for a network-placed remote actor, and lets
    // a remote actor moved by Skyrim pass through local characters. Requires
    // kTrampolineSize trampoline bytes.
    void InstallHooks();

    // Actor-module internal, called by ActorRuntime on each update in which the
    // network transform places the actor. Skyrim's movement update is skipped
    // for it from then on, its character body leaves every collision and no
    // longer places the actor, and its controller is reported supported by the
    // ground unless its source swims.
    void PlaceByNetwork(RE::Actor& a_actor, RemoteActor& a_remote);

    // Actor-module internal, called by ActorRuntime on each update in which a
    // native interaction, named by a_owner, places the actor, and by ActorDeath
    // at the kill: Skyrim's movement update runs for it again. Its character body collides again unless its
    // mount carries it.
    void ReleaseToEngine(RE::Actor& a_actor, RemoteActor& a_remote, std::string_view a_owner, bool a_carried);
}
