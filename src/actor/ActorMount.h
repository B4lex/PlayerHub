#pragma once

#ifndef NDEBUG
#include "actor/GraphProbe.h"
#endif

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

// Skyrim's dismount plays a paired exit clip that links the mount's graph to the
// rider's, and removing either actor while it plays crashes.
namespace ActorMount
{
    struct Gait
    {
        float graphSpeed{ -1.0F };
        bool moving{ false };
        bool sprinting{ false };
        float gallopPendingElapsed{ 0.0F };
        float stopPendingElapsed{ 0.0F };
        float reconcileElapsed{ 0.0F };
        std::uint32_t rejectedEvents{ 0 };
        std::uint32_t corrections{ 0 };
        float diagnosticElapsed{ 0.0F };
#ifndef NDEBUG
        GraphProbe::Snapshot graphProbeSnapshot{};
#endif
    };

    struct Placement
    {
        RE::NiPoint3 position{};
        float heading{ 0.0F };
        bool valid{ false };
    };

    // A flying mount is always the same vanilla dragon.
    [[nodiscard]] RE::TESNPC* PickMountBase(bool a_flying);

    void InstallHooks();

    // Skyrim derives kIsAMount from the horse and dragon mount keywords when the actor
    // initializes; a remote dragon lacks the dragon one, and without the flag Skyrim treats
    // it as the rider of its own interaction.
    void AssertFlyingMount(RE::Actor& a_mount);

    // a_flying: the mount is a dragon, whose rider idles are gated on Dragonborn's mount keyword.
    [[nodiscard]] bool Seat(RemoteActor& a_rider, const RemoteActor& a_mount, bool a_flying);
    [[nodiscard]] bool IsSeated(const RemoteActor& a_rider, const RemoteActor& a_mount);
    [[nodiscard]] bool IsInTransition(const RemoteActor& a_rider, const RemoteActor& a_mount);

    [[nodiscard]] Placement CapturePlacement(const RemoteActor& a_mount);
    void CarryRider(const RemoteActor& a_rider, const RemoteActor& a_mount, const Placement& a_before);

    void ApplyGait(RE::Actor& a_actor, const RemoteActor& a_remote, Gait& a_gait, float a_speed, float a_deltaTime,
        bool a_sendEvents, const char* a_role);
}
