#include "sync/CheckpointPublisher.h"
#include "network/SocketIOClient.h"
#include "runtime/Profiling.h"
#include "sync/animation/AnimationSync.h"
#include "sync/combat/CombatSync.h"
#include "sync/death/DeathSync.h"
#include "sync/furniture/FurnitureSync.h"
#include "sync/mount/MountSync.h"
#include "sync/movement/MovementSync.h"

#include <algorithm>
#include <limits>

namespace CheckpointPublisher
{
    namespace
    {
        constexpr float kSnapshotInterval = 0.05F;
    }

    Publication PublishLocalCheckpoint(const LocalFrame& a_local, float a_delta, std::uint64_t a_graphEpoch)
    {
        PROFILE_FUNCTION();
        static float elapsed = kSnapshotInterval;
        static std::uint64_t sequence = 0;

        elapsed += (std::max)(a_delta, 0.0F);
        if (elapsed < kSnapshotInterval || !Network::IsConnected())
        {
            return Publication{ .status = Network::IsConnected() ? Status::kNotDue : Status::kDisconnected };
        }
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
        const auto furnitureRefID = FurnitureSync::SampleLocalFurniture(a_local, sequence);
        const auto handPresentation = CombatSync::SampleLocalHandPresentation();
        const bool dead = DeathSync::SampleLocalDeath(a_local, sequence);

        const Network::PlayerTransform checkpoint{ .x = movement.position.x,
            .y = movement.position.y,
            .z = movement.position.z,
            .heading = movement.heading,
            .speed = movement.speed,
            .direction = movement.direction,
            .animationState = animationState,
            .handPresentation = handPresentation,
            .dead = dead,
            .mounted = static_cast<bool>(mount),
            .swimming = movement.swimming,
            .cellID = movement.cellID,
            .worldspaceID = movement.worldspaceID,
            .furnitureRefID = furnitureRefID,
            .graphEpoch = a_graphEpoch,
            .sequence = sequence };
        Network::PublishLocalPlayerTransform(checkpoint);
        return Publication{ .status = Status::kPublished, .checkpoint = checkpoint };
    }
}
