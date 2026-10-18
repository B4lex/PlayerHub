#pragma once

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
        std::uint32_t speedRestorations{ 0 };
        float diagnosticElapsed{ 0.0F };
    };

    struct Placement
    {
        RE::NiPoint3 position{};
        float heading{ 0.0F };
        bool valid{ false };
    };

    [[nodiscard]] RE::TESNPC* GetMountBase();

    [[nodiscard]] bool Seat(RemoteActor& a_rider, const RemoteActor& a_mount);
    [[nodiscard]] bool IsSeated(const RemoteActor& a_rider, const RemoteActor& a_mount);
    [[nodiscard]] bool IsInTransition(const RemoteActor& a_rider, const RemoteActor& a_mount);

    [[nodiscard]] Placement CapturePlacement(const RemoteActor& a_mount);
    void CarryRider(const RemoteActor& a_rider, const RemoteActor& a_mount, const Placement& a_before);

    void ApplyGait(RE::Actor& a_actor, const RemoteActor& a_remote, Gait& a_gait, float a_speed, float a_deltaTime,
        bool a_sendEvents, const char* a_role);
}
