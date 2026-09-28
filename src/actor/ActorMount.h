#pragma once

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

// Skyrim-native riding of a remote mount. The mount is an ordinary registered
// remote actor, so it has the same survival, combat, and collision isolation as
// its rider. Main-loop thread only.
//
// A rider is never taken off its mount explicitly: Skyrim's dismount plays a
// paired exit clip that links the mount's graph to the rider's, and removing
// either actor while it plays crashes. A rider and its mount are removed together
// while seated instead.
namespace ActorMount
{
    // Riding locomotion requested of one graph, reconciled against the graph's
    // own state.
    struct Gait
    {
        float graphSpeed{ -1.0F };
        bool moving{ false };
        bool sprinting{ false };
        // Seconds the speed has stayed in the gallop range without galloping.
        float gallopPendingElapsed{ 0.0F };
        // Seconds since the graph's state was last reconciled.
        float reconcileElapsed{ 0.0F };
        std::uint32_t rejectedEvents{ 0 };
        std::uint32_t corrections{ 0 };
        // Seconds since the last travelling gait sample was logged.
        float diagnosticElapsed{ 0.0F };
    };

    // The vanilla base every remote mount is spawned from, whatever the source
    // rides, so that it exists in every peer's load order. Null if unavailable.
    [[nodiscard]] RE::TESNPC* GetMountBase();

    // Seats the rider on the mount at once, without the mount animation. Returns
    // whether Skyrim started the interaction; IsSeated confirms the result.
    [[nodiscard]] bool Seat(RemoteActor& a_rider, const RemoteActor& a_mount);
    [[nodiscard]] bool IsSeated(const RemoteActor& a_rider, const RemoteActor& a_mount);
    // Either actor is getting on or off a mount, so a paired clip may link their
    // graphs.
    [[nodiscard]] bool IsInTransition(const RemoteActor& a_rider, const RemoteActor& a_mount);

    // Actor-module internal, called by ActorRuntime: drives the riding
    // locomotion of a mount, or of its seated rider, from the mount's speed. Both
    // graphs pick their walk, trot, and run from their own Speed. Only the
    // mount's graph takes the locomotion events (a_sendEvents); the rider's
    // rejects them. a_role names the graph in logs.
    void ApplyGait(RE::Actor& a_actor, const RemoteActor& a_remote, Gait& a_gait, float a_speed, float a_deltaTime,
        bool a_sendEvents, const char* a_role);
}
