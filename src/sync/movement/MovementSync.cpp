#include "sync/movement/MovementSync.h"
#include "actor/ActorMovement.h"
#include "actor/RemoteActor.h"
#include "forms/FormLookup.h"

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

        RE::FormID s_localSpaceCellID = 0;
        RE::FormID s_localSpaceWorldspaceID = 0;
        LocalSpace s_localSpace;
        bool s_hasLocalSpace = false;

        float SanitizePitch(float a_angle)
        {
            return std::isfinite(a_angle) ? std::clamp(a_angle, -Network::kMaximumPitch, Network::kMaximumPitch) : 0.0F;
        }
    }

    bool IsSameCoordinateSpace(const Network::PlayerTransform& a_first, const Network::PlayerTransform& a_second)
    {
        return a_first.worldspace == a_second.worldspace
            && (!a_first.worldspace.IsEmpty() || a_first.interiorCell == a_second.interiorCell);
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

        const auto* actorState = a_carrier.AsActorState();
        const bool swimming = actorState && actorState->IsSwimming();
        float heading = a_carrier.GetAngleZ();
        if (!std::isfinite(heading))
        {
            heading = 0.0F;
        }
        static const RE::BSFixedString directionVariable{ "Direction" };
        float direction = 0.0F;
        const bool graphDirectionAvailable
            = a_carrier.GetGraphVariableFloat(directionVariable, direction) && std::isfinite(direction);
        // The Speed channel feeds the graph the intended movement speed divided by
        // the actor's scale, not the distance covered: climbing stairs keeps the
        // full walk pace while the controller covers less ground.
        static const RE::BSFixedString speedVariable{ "Speed" };
        float graphSpeed = 0.0F;
        const bool onFoot = &a_carrier == static_cast<RE::Actor*>(&a_local.player);
        const bool graphSpeedAvailable = onFoot && a_carrier.GetGraphVariableFloat(speedVariable, graphSpeed)
            && std::isfinite(graphSpeed) && graphSpeed >= 0.0F;
        float speed = 0.0F;
        if (comparableLocalSpace && a_snapshotElapsed > 0.0F)
        {
            const float deltaX = position.x - s_previousPosition.x;
            const float deltaY = position.y - s_previousPosition.y;
            const float deltaZ = position.z - s_previousPosition.z;
            const float horizontalDistanceSquared = deltaX * deltaX + deltaY * deltaY;
            // A mount's gait follows the ground it covers. A swimmer moves along its
            // view, vertical included.
            const float travelSquared
                = swimming ? horizontalDistanceSquared + deltaZ * deltaZ : horizontalDistanceSquared;
            speed = std::sqrt(travelSquared) / a_snapshotElapsed;
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
        const bool grounded
            = controller && controller->context.currentState == RE::hkpCharacterStateType::kOnGround;

        return LocalSample{ .position = position,
            .heading = heading,
            .pitch = pitch,
            .measuredSpeed = speed,
            .graphSpeed = graphSpeedAvailable ? std::optional<float>{ graphSpeed } : std::nullopt,
            .direction = direction,
            .swimming = swimming,
            .grounded = grounded,
            .relocated = !comparableLocalSpace,
            .cellID = cellID,
            .worldspaceID = worldspaceID };
    }

    float GetCheckpointSpeed(const LocalSample& a_sample, Multiplayer::MovementState a_state)
    {
        // An idle checkpoint keeps the measured speed, so a graph Speed still easing
        // to zero does not defeat the server's idle repeat suppression.
        return a_sample.graphSpeed && Multiplayer::IsMoving(a_state) ? *a_sample.graphSpeed : a_sample.measuredSpeed;
    }

    const LocalSpace& SampleLocalSpace(const LocalSample& a_sample)
    {
        // Exterior cell crossings keep the worldspace's reference.
        const auto cellID = a_sample.worldspaceID == 0 ? a_sample.cellID : 0;
        if (s_hasLocalSpace && cellID == s_localSpaceCellID && a_sample.worldspaceID == s_localSpaceWorldspaceID)
        {
            return s_localSpace;
        }
        s_hasLocalSpace = true;
        s_localSpaceCellID = cellID;
        s_localSpaceWorldspaceID = a_sample.worldspaceID;

        auto reference
            = FormLookup::MakeFormReference(RE::TESForm::LookupByID(cellID != 0 ? cellID : a_sample.worldspaceID))
                  .value_or(Multiplayer::FormReference{});
        if (reference.IsEmpty())
        {
            logs::warn("Local space has no form reference: cell={:08X} worldspace={:08X}; peers will not place the "
                       "player in it",
                cellID, a_sample.worldspaceID);
        }
        else
        {
            logs::debug("Publishing local space: cell={:08X} worldspace={:08X} reference={}|{:06X}", cellID,
                a_sample.worldspaceID, reference.plugin, reference.localFormID);
        }
        s_localSpace = cellID != 0 ? LocalSpace{ .interiorCell = std::move(reference) }
                                   : LocalSpace{ .worldspace = std::move(reference) };
        return s_localSpace;
    }

    void OnCheckpointPlayed(RemoteState& a_state, const CheckpointUpdate& a_update)
    {
        const auto& checkpoint = a_update.checkpoint;
        if (!a_update.inserted && a_update.previous.interiorCell == checkpoint.interiorCell
            && a_update.previous.worldspace == checkpoint.worldspace)
        {
            return;
        }

        const auto& reference = checkpoint.worldspace.IsEmpty() ? checkpoint.interiorCell : checkpoint.worldspace;
        const auto formID = FormLookup::ResolveFormID(reference);
        a_state = checkpoint.worldspace.IsEmpty() ? RemoteState{ .cellID = formID }
                                                  : RemoteState{ .worldspaceID = formID };
        if (formID == 0 && !reference.IsEmpty())
        {
            logs::warn("Remote player {} space could not be resolved: sequence={} interior={} reference={}|{:06X}; "
                       "it is not in the local load order",
                a_update.playerID, static_cast<unsigned long long>(checkpoint.sequence),
                checkpoint.worldspace.IsEmpty(), reference.plugin, reference.localFormID);
            return;
        }
        logs::debug("Remote player {} space resolved: sequence={} cell={:08X} worldspace={:08X} reference={}|{:06X}",
            a_update.playerID, static_cast<unsigned long long>(checkpoint.sequence), a_state.cellID,
            a_state.worldspaceID, reference.plugin, reference.localFormID);
    }

    bool IsInLocalSpace(const RemoteFrame& a_remote, RE::FormID a_localCellID, RE::FormID a_localWorldspaceID)
    {
        return IsSameCoordinateSpace(a_remote.cellID, a_remote.worldspaceID, a_localCellID, a_localWorldspaceID);
    }

    void SyncMovement(const RemoteFrame& a_remote, std::uint64_t& a_appliedSequence, RemoteActor& a_actor)
    {
        if (a_appliedSequence != a_remote.checkpoint.sequence)
        {
            const RE::NiPoint3 checkpointPosition{ a_remote.checkpoint.x, a_remote.checkpoint.y,
                a_remote.checkpoint.z };
            ActorMovement::SetTargetMovement(a_actor, checkpointPosition, a_remote.checkpoint.speed,
                a_remote.checkpoint.swimming, a_remote.checkpoint.grounded);
            a_appliedSequence = a_remote.checkpoint.sequence;
        }
        ActorMovement::SetAnimationSample(a_actor, a_remote.pose.sampledSpeed, a_remote.pose.direction);
        ActorMovement::SetTargetTransform(a_actor, a_remote.pose.position, a_remote.pose.heading,
            a_remote.pose.headingRate, a_remote.pose.pitch, a_remote.pose.speed);
    }
}
