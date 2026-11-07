#include "sync/movement/MovementSync.h"
#include "actor/ActorMovement.h"
#include "actor/AimPresentation.h"
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

    LocalSample SampleLocalMovement(LocalState& a_state, const LocalFrame& a_local, RE::Actor& a_carrier,
        const RE::TESObjectCELL& a_cell, float a_snapshotElapsed)
    {
        const auto position = a_carrier.GetPosition();
        const auto cellID = a_cell.GetFormID();
        const auto worldspaceID = GetWorldspaceID(a_local.player);
        const bool comparableLocalSpace = a_state.hasPreviousPosition
            && IsSameCoordinateSpace(cellID, worldspaceID, a_state.previousCellID, a_state.previousWorldspaceID)
            && position.GetDistance(a_state.previousPosition) <= kRelocationDistance;

        const auto* actorState = a_carrier.AsActorState();
        const bool swimming = actorState && actorState->IsSwimming();
        const bool flying = actorState && actorState->IsFlying();
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
            const float deltaX = position.x - a_state.previousPosition.x;
            const float deltaY = position.y - a_state.previousPosition.y;
            const float deltaZ = position.z - a_state.previousPosition.z;
            const float horizontalDistanceSquared = deltaX * deltaX + deltaY * deltaY;
            // A mount's gait follows the ground it covers. A swimmer moves along its
            // view and a flying mount along its flight, vertical included.
            const float travelSquared
                = swimming || flying ? horizontalDistanceSquared + deltaZ * deltaZ : horizontalDistanceSquared;
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

        a_state.previousPosition = position;
        a_state.previousCellID = cellID;
        a_state.previousWorldspaceID = worldspaceID;
        a_state.hasPreviousPosition = true;

        // The carrier's own movement update eases it toward the ground, and its
        // per-frame 3D update turns it into the body's rotation.
        const auto* controller = a_carrier.GetCharController();
        const float pitch = controller ? SanitizePitch(controller->pitchAngle) : 0.0F;
        // A ragdoll carries the reference with the body, off the ground, while the
        // controller still reports kOnGround.
        const bool grounded = controller && controller->context.currentState == RE::hkpCharacterStateType::kOnGround
            && !a_carrier.IsInRagdollState();
        // The player's aim, not a mount's.
        const float lookPitch = SanitizePitch(a_local.player.GetAngleX());

        return LocalSample{ .position = position,
            .heading = heading,
            .pitch = pitch,
            .lookPitch = lookPitch,
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

    float GetCheckpointLookPitch(const LocalSample& a_sample, const Multiplayer::AnimationState& a_state)
    {
        // Only a drawn weapon or spell aims, and a sheathed player's view would defeat the
        // server's idle repeat suppression.
        return a_state.weaponDrawn ? a_sample.lookPitch : 0.0F;
    }

    const LocalSpace& SampleLocalSpace(LocalState& a_state, const LocalSample& a_sample)
    {
        // Exterior cell crossings keep the worldspace's reference.
        const auto cellID = a_sample.worldspaceID == 0 ? a_sample.cellID : 0;
        if (a_state.hasSpace && cellID == a_state.spaceCellID && a_sample.worldspaceID == a_state.spaceWorldspaceID)
        {
            return a_state.space;
        }
        a_state.hasSpace = true;
        a_state.spaceCellID = cellID;
        a_state.spaceWorldspaceID = a_sample.worldspaceID;

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
        a_state.space = cellID != 0 ? LocalSpace{ .interiorCell = std::move(reference) }
                                    : LocalSpace{ .worldspace = std::move(reference) };
        return a_state.space;
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
            // A SkyParkour climb keeps its source's controller on the ground while it rises
            // a ledge, so a probe under a posed checkpoint pulled the actor down to the floor
            // below.
            ActorMovement::SetTargetMovement(a_actor, checkpointPosition, a_remote.checkpoint.speed,
                a_remote.checkpoint.swimming, a_remote.checkpoint.grounded, !a_remote.checkpoint.skeleton.empty());
            a_appliedSequence = a_remote.checkpoint.sequence;
        }
        ActorMovement::SetAnimationSample(a_actor, a_remote.pose.sampledSpeed, a_remote.pose.direction);
        ActorMovement::SetTargetTransform(a_actor, a_remote.pose.position, a_remote.pose.heading,
            a_remote.pose.headingRate, a_remote.pose.pitch, a_remote.pose.speed);
    }

    void SyncLookPitch(const RemoteFrame& a_remote, RemoteActor& a_actor)
    {
        AimPresentation::SetLookPitch(a_actor, a_remote.pose.lookPitch);
    }
}
