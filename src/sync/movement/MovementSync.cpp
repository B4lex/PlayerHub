#include "sync/movement/MovementSync.h"
#include "actor/ActorMovement.h"
#include "actor/RemoteActor.h"
#include "protocol/PlayerState.h"

#include <algorithm>
#include <cmath>

namespace MovementSync
{
    namespace
    {
        constexpr float kMinimumHeadingTravel = 1.0F;
        constexpr float kTwoPi = 6.28318530717958647692F;

        RE::NiPoint3 s_previousPosition{};
        RE::FormID s_previousCellID = 0;
        RE::FormID s_previousWorldspaceID = 0;
        bool s_hasPreviousPosition = false;

        float SanitizePitch(float a_angle)
        {
            return std::isfinite(a_angle) ? std::clamp(a_angle, -Network::kMaximumPitch, Network::kMaximumPitch) : 0.0F;
        }
    }

    std::uint32_t GetWorldspaceID(const RE::TESObjectREFR& a_reference)
    {
        const auto* worldspace = a_reference.GetWorldspace();
        return worldspace ? worldspace->GetFormID() : 0;
    }

    LocalSample SampleLocalMovement(
        const LocalFrame& a_local, RE::Actor& a_carrier, const RE::TESObjectCELL& a_cell, float a_snapshotElapsed)
    {
        const auto position = a_carrier.GetPosition();
        const auto cellID = a_cell.GetFormID();
        const auto worldspaceID = GetWorldspaceID(a_local.player);
        const bool comparableLocalSpace = s_hasPreviousPosition
            && IsSameCoordinateSpace(cellID, worldspaceID, s_previousCellID, s_previousWorldspaceID)
            && position.GetDistance(s_previousPosition) <= kRelocationDistance;

        float speed = 0.0F;
        float heading = a_carrier.GetAngleZ();
        if (!std::isfinite(heading))
        {
            heading = 0.0F;
        }
        static const RE::BSFixedString directionVariable{ "Direction" };
        float direction = 0.0F;
        const bool graphDirectionAvailable
            = a_carrier.GetGraphVariableFloat(directionVariable, direction) && std::isfinite(direction);
        if (comparableLocalSpace && a_snapshotElapsed > 0.0F)
        {
            const float deltaX = position.x - s_previousPosition.x;
            const float deltaY = position.y - s_previousPosition.y;
            const float deltaZ = position.z - s_previousPosition.z;
            const float horizontalDistanceSquared = deltaX * deltaX + deltaY * deltaY;
            speed = std::sqrt(horizontalDistanceSquared + deltaZ * deltaZ) / a_snapshotElapsed;
            if (!std::isfinite(speed))
            {
                speed = 0.0F;
            }

            if (!graphDirectionAvailable && horizontalDistanceSquared >= kMinimumHeadingTravel * kMinimumHeadingTravel)
            {
                    // Skyrim heading zero faces positive Y.
                direction = (std::atan2(deltaX, deltaY) - heading) / kTwoPi;
            }
        }
        direction = Multiplayer::NormalizeMovementDirection(direction);

        s_previousPosition = position;
        s_previousCellID = cellID;
        s_previousWorldspaceID = worldspaceID;
        s_hasPreviousPosition = true;

        // The carrier's own movement update eases it toward the ground, and its
        // per-frame 3D update turns it into the body's rotation.
        const auto* controller = a_carrier.GetCharController();
        const float pitch = controller ? SanitizePitch(controller->pitchAngle) : 0.0F;

        const auto* actorState = a_carrier.AsActorState();
        return LocalSample{ .position = position,
            .heading = heading,
            .pitch = pitch,
            .speed = speed,
            .direction = direction,
            .swimming = actorState && actorState->IsSwimming(),
            .cellID = cellID,
            .worldspaceID = worldspaceID };
    }

    bool IsInLocalSpace(const RemoteFrame& a_remote, RE::FormID a_localCellID, RE::FormID a_localWorldspaceID)
    {
        return IsSameCoordinateSpace(
            a_remote.checkpoint.cellID, a_remote.checkpoint.worldspaceID, a_localCellID, a_localWorldspaceID);
    }

    void SyncMovement(const RemoteFrame& a_remote, std::uint64_t& a_appliedSequence, RemoteActor& a_actor)
    {
        if (a_appliedSequence != a_remote.checkpoint.sequence)
        {
            const RE::NiPoint3 checkpointPosition{ a_remote.checkpoint.x, a_remote.checkpoint.y,
                a_remote.checkpoint.z };
            ActorMovement::SetTargetMovement(
                a_actor, checkpointPosition, a_remote.checkpoint.speed, a_remote.checkpoint.swimming);
            a_appliedSequence = a_remote.checkpoint.sequence;
        }
        ActorMovement::SetAnimationSample(a_actor, a_remote.pose.sampledSpeed, a_remote.pose.direction);
        ActorMovement::SetTargetTransform(a_actor, a_remote.pose.position, a_remote.pose.heading,
            a_remote.pose.headingRate, a_remote.pose.pitch, a_remote.pose.speed);
    }
}
