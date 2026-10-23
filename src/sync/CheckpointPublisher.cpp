#include "sync/CheckpointPublisher.h"
#include "network/ServerClient.h"
#include "protocol/Quantization.h"
#include "runtime/Profiling.h"
#include "sync/animation/AnimationSync.h"
#include "sync/combat/CombatSync.h"
#include "sync/death/DeathSync.h"
#include "sync/furniture/FurnitureSync.h"
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
        constexpr float kSnapshotIntervalSeconds = 0.05F;

        std::uint32_t GetSampleTime()
        {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch());
            return static_cast<std::uint32_t>(now.count() % Network::kSampleTimeModulus);
        }
    }

    Publication PublishLocalCheckpoint(const LocalFrame& a_local, float a_delta, std::uint64_t a_graphEpoch)
    {
        PROFILE_FUNCTION();
        static float elapsed = kSnapshotIntervalSeconds;
        static float scheduleElapsed = kSnapshotIntervalSeconds;
        static std::uint64_t sequence = 0;

        // Not clamped: the elapsed time divides the distance travelled into the published speed.
        elapsed += a_delta;
        scheduleElapsed += a_delta;
        if (scheduleElapsed < kSnapshotIntervalSeconds || !Network::IsConnected())
        {
            return Publication{ .status = Network::IsConnected() ? Status::kNotDue : Status::kDisconnected };
        }
        // Keeping the remainder holds the average rate; whole missed intervals are dropped,
        // not sent as a burst.
        scheduleElapsed = std::fmod(scheduleElapsed, kSnapshotIntervalSeconds);
        const float snapshotElapsed = elapsed;
        elapsed = 0.0F;

        auto* cell = a_local.player.GetParentCell();
        if (!cell)
        {
            return Publication{ .status = Status::kSkipped };
        }

        if (sequence >= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        {
            sequence = 0;
        }
        ++sequence;

        const auto mount = MountSync::SampleLocalMount(a_local, sequence);
        const auto movement
            = MovementSync::SampleLocalMovement(a_local, mount ? *mount : a_local.player, *cell, snapshotElapsed);
        const auto animationState = AnimationSync::SampleLocalAnimation(a_local, sequence, movement);
        auto furniture = FurnitureSync::SampleLocalFurniture(a_local, sequence);
        const auto handPresentation = CombatSync::SampleLocalHandPresentation();
        const bool dead = DeathSync::SampleLocalDeath(a_local, sequence);
        const auto vampireLord = ShapeshiftSync::SampleLocalVampireLord(a_local);

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
            .cellID = movement.cellID,
            .worldspaceID = movement.worldspaceID,
            .furniture = std::move(furniture),
            .shapeshift = ShapeshiftSync::GetLocalForm(),
            .graphEpoch = a_graphEpoch,
            .sequence = sequence,
            .sampleTime = GetSampleTime() };
        Network::PublishLocalPlayerTransform(checkpoint);
        return Publication{ .status = Status::kPublished, .checkpoint = checkpoint };
    }
}
