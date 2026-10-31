#include "sync/CheckpointPublisher.h"
#include "network/ServerClient.h"
#include "protocol/Quantization.h"
#include "runtime/Profiling.h"
#include "sync/animation/AnimationSync.h"
#include "sync/combat/CombatSync.h"
#include "sync/death/DeathSync.h"
#include "sync/furniture/FurnitureSync.h"
#include "sync/idle/IdleSync.h"
#include "sync/mount/MountSync.h"
#include "sync/movement/MovementSync.h"
#include "sync/shapeshift/ShapeshiftSync.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace CheckpointPublisher
{
    namespace
    {
        std::uint32_t GetSampleTime()
        {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch());
            return static_cast<std::uint32_t>(now.count() % Network::kSampleTimeModulus);
        }
    }

    Publication PublishLocalCheckpoint(State& a_state, AnimationSync::LocalState& a_animation,
        const LocalFrame& a_local, float a_delta, std::uint64_t a_graphEpoch)
    {
        PROFILE_FUNCTION();
        // Not clamped: the elapsed time divides the distance travelled into the published speed.
        a_state.elapsed += a_delta;
        a_state.scheduleElapsed += a_delta;
        if (a_state.scheduleElapsed < kSnapshotIntervalSeconds || !Network::IsConnected())
        {
            return Publication{ .status = Network::IsConnected() ? Status::kNotDue : Status::kDisconnected };
        }
        // Keeping the remainder holds the average rate; whole missed intervals are dropped,
        // not sent as a burst.
        a_state.scheduleElapsed = std::fmod(a_state.scheduleElapsed, kSnapshotIntervalSeconds);
        const float snapshotElapsed = std::exchange(a_state.elapsed, 0.0F);

        auto* cell = a_local.player.GetParentCell();
        if (!cell)
        {
            return Publication{ .status = Status::kSkipped };
        }

        if (a_state.sequence >= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        {
            a_state.sequence = 0;
        }
        const auto sequence = ++a_state.sequence;

        const auto mount = MountSync::SampleLocalMount(a_local, sequence);
        const auto movement = MovementSync::SampleLocalMovement(
            a_state.movement, a_local, mount ? *mount : a_local.player, *cell, snapshotElapsed);
        const auto animationState = AnimationSync::SampleLocalAnimation(a_animation, a_local, sequence, movement);
        const auto& space = MovementSync::SampleLocalSpace(a_state.movement, movement);
        auto furniture = FurnitureSync::SampleLocalFurniture(a_local, sequence);
        const auto handPresentation = CombatSync::SampleLocalHandPresentation();
        const bool dead = DeathSync::SampleLocalDeath(a_local, sequence);
        const auto vampireLord = ShapeshiftSync::SampleLocalVampireLord(a_local);
        const auto form = ShapeshiftSync::GetLocalForm();
        auto idle = IdleSync::SampleLocalIdle({ .animationState = animationState,
            .furniture = !furniture.IsEmpty(),
            .mounted = static_cast<bool>(mount),
            .dead = dead,
            .swimming = movement.swimming,
            .transformed = form != Multiplayer::Shapeshift::kNone });

        const Network::PlayerTransform checkpoint{ .x = movement.position.x,
            .y = movement.position.y,
            .z = movement.position.z,
            .heading = movement.heading,
            .pitch = movement.pitch,
            .speed = MovementSync::GetCheckpointSpeed(movement, animationState.movementState),
            .direction = movement.direction,
            .animationState = animationState,
            .handPresentation = handPresentation,
            .dead = dead,
            .mounted = static_cast<bool>(mount),
            .swimming = movement.swimming,
            .levitating = vampireLord.levitating,
            .mistForm = vampireLord.mistForm,
            .bats = vampireLord.bats,
            .grounded = movement.grounded,
            .interiorCell = space.interiorCell,
            .worldspace = space.worldspace,
            .furniture = std::move(furniture),
            .shapeshift = form,
            .idle = std::move(idle),
            .graphEpoch = a_graphEpoch,
            .sequence = sequence,
            .sampleTime = GetSampleTime() };
        Network::PublishLocalPlayerTransform(checkpoint);
        return Publication{ .status = Status::kPublished, .checkpoint = checkpoint };
    }
}
