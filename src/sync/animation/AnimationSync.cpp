#include "sync/animation/AnimationSync.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorFurniture.h"
#include "actor/ActorMovement.h"
#include "actor/HandPresentation.h"
#include "actor/RemoteActor.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"

#include <cstdint>
#include <limits>

namespace AnimationSync
{
    namespace
    {
        using Multiplayer::IsMoving;
        using Multiplayer::IsSneaking;

        constexpr Multiplayer::MovementState GetNormalMovingState(Multiplayer::MovementState a_state)
        {
            return a_state == Multiplayer::MovementState::kWalking
                    || a_state == Multiplayer::MovementState::kSneakingWalking
                ? Multiplayer::MovementState::kWalking
                : Multiplayer::MovementState::kRunning;
        }

        constexpr Multiplayer::MovementState GetNextMovementStep(
            Multiplayer::MovementState a_current, Multiplayer::MovementState a_target)
        {
            if (a_current == a_target)
            {
                return a_target;
            }

            const bool currentSprinting = a_current == Multiplayer::MovementState::kSprinting;
            const bool targetSprinting = a_target == Multiplayer::MovementState::kSprinting;
            if (currentSprinting && !targetSprinting)
            {
                // Leave sprint while still moving, so a later graph update can stop or enter
                // sneak without racing SprintStop.
                return IsMoving(a_target) && !IsSneaking(a_target) ? GetNormalMovingState(a_target)
                                                                   : Multiplayer::MovementState::kRunning;
            }
            if (!currentSprinting && targetSprinting)
            {
                if (IsSneaking(a_current))
                {
                    return IsMoving(a_current) ? GetNormalMovingState(a_current) : Multiplayer::MovementState::kIdle;
                }
                if (!IsMoving(a_current))
                {
                    return Multiplayer::MovementState::kRunning;
                }
                return a_target;
            }

            const bool currentSneaking = IsSneaking(a_current);
            const bool targetSneaking = IsSneaking(a_target);
            if (currentSneaking != targetSneaking && IsMoving(a_current) != IsMoving(a_target))
            {
                if (IsMoving(a_current))
                {
                    return currentSneaking ? Multiplayer::MovementState::kSneakingIdle
                                           : Multiplayer::MovementState::kIdle;
                }
                return targetSneaking ? Multiplayer::MovementState::kSneakingIdle : Multiplayer::MovementState::kIdle;
            }

            return a_target;
        }

        constexpr Multiplayer::AnimationState GetNextAnimationStep(
            Multiplayer::AnimationState a_current, Multiplayer::AnimationState a_target)
        {
            if (a_current == a_target)
            {
                return a_target;
            }
            if (a_current.jumping && a_target.jumping)
            {
                // Locomotion events are rejected by the graph's airborne branch.
                return a_current;
            }

            if (a_current.jumping != a_target.jumping)
            {
                if (a_current.jumping)
                {
                    return a_target;
                }

                auto groundedTarget = a_target;
                groundedTarget.jumping = false;
                if (a_current != groundedTarget)
                {
                    return GetNextAnimationStep(a_current, groundedTarget);
                }
                return a_target;
            }

            return Multiplayer::AnimationState{ .movementState
                = GetNextMovementStep(a_current.movementState, a_target.movementState),
                .jumping = a_target.jumping,
                .weaponDrawn = a_target.weaponDrawn };
        }

        static_assert(GetNextMovementStep(Multiplayer::MovementState::kIdle, Multiplayer::MovementState::kSprinting)
            == Multiplayer::MovementState::kRunning);
        static_assert(GetNextMovementStep(Multiplayer::MovementState::kSprinting, Multiplayer::MovementState::kIdle)
            == Multiplayer::MovementState::kRunning);
        static_assert(
            GetNextMovementStep(Multiplayer::MovementState::kIdle, Multiplayer::MovementState::kSneakingRunning)
            == Multiplayer::MovementState::kSneakingIdle);
        static_assert(
            GetNextMovementStep(Multiplayer::MovementState::kSneakingRunning, Multiplayer::MovementState::kIdle)
            == Multiplayer::MovementState::kSneakingIdle);
        static_assert(
            GetNextAnimationStep(Multiplayer::AnimationState{},
                Multiplayer::AnimationState{ .movementState = Multiplayer::MovementState::kRunning, .jumping = true })
            == Multiplayer::AnimationState{ .movementState = Multiplayer::MovementState::kRunning, .jumping = false });
        static_assert(
            GetNextAnimationStep(
                Multiplayer::AnimationState{ .movementState = Multiplayer::MovementState::kRunning, .jumping = true },
                Multiplayer::AnimationState{ .movementState = Multiplayer::MovementState::kIdle, .jumping = true })
            == Multiplayer::AnimationState{ .movementState = Multiplayer::MovementState::kRunning, .jumping = true });
        static_assert(
            GetNextAnimationStep(
                Multiplayer::AnimationState{ .movementState = Multiplayer::MovementState::kRunning, .jumping = true },
                Multiplayer::AnimationState{})
            == Multiplayer::AnimationState{});
        static_assert(
            GetNextAnimationStep(Multiplayer::AnimationState{}, Multiplayer::AnimationState{ .weaponDrawn = true })
            == Multiplayer::AnimationState{ .weaponDrawn = true });

        // ActorState::IsWeaponDrawn stays true through kSheathing and false through
        // kDrawing, so it changes only when the clip ends.
        constexpr bool IsWeaponDrawnOrDrawing(RE::WEAPON_STATE a_state) noexcept
        {
            return a_state == RE::WEAPON_STATE::kDrawing || a_state == RE::WEAPON_STATE::kDrawn
                || a_state == RE::WEAPON_STATE::kWantToSheathe;
        }

        static_assert(IsWeaponDrawnOrDrawing(RE::WEAPON_STATE::kDrawing));
        static_assert(!IsWeaponDrawnOrDrawing(RE::WEAPON_STATE::kWantToDraw));
        static_assert(IsWeaponDrawnOrDrawing(RE::WEAPON_STATE::kWantToSheathe));
        static_assert(!IsWeaponDrawnOrDrawing(RE::WEAPON_STATE::kSheathing));

        struct JumpInput
        {
            // bInJumpState, or the physical jump or midair state without it.
            bool requested{ false };
            bool midair{ false };
            bool swimming{ false };
        };

        // bInJumpState can remain set throughout graph-owned landing cleanup, so
        // physical ground contact ends a jump that has been airborne.
        constexpr bool AdvanceLocalJump(LocalJump& a_jump, JumpInput a_input) noexcept
        {
            if (a_input.swimming)
            {
                // The jump input only swims upward, but the remote actor's swim graph
                // still accepts a jump event.
                a_jump = LocalJump{ .waitForGraphReset = a_input.requested };
            }
            else if (a_jump.replicated)
            {
                a_jump.observedMidair = a_jump.observedMidair || a_input.midair;
                if (a_jump.observedMidair && !a_input.midair)
                {
                    // Physical contact is the landing boundary; a stale graph jump state is
                    // suppressed until it has reset once.
                    a_jump = LocalJump{ .waitForGraphReset = a_input.requested };
                }
                else if (!a_jump.observedMidair && !a_input.requested)
                {
                    // Also release a takeoff that was cancelled before leaving the ground.
                    a_jump.replicated = false;
                }
            }
            else if (a_jump.waitForGraphReset)
            {
                if (!a_input.requested)
                {
                    a_jump.waitForGraphReset = false;
                }
                else if (a_input.midair)
                {
                    // Skyrim accepts a new takeoff from its landing branch, so
                    // bInJumpState may never reset between consecutive jumps. Leaving
                    // the ground again after the published landing is that new jump.
                    a_jump = LocalJump{ .replicated = true, .observedMidair = true };
                }
            }
            else if (a_input.requested)
            {
                a_jump = LocalJump{ .replicated = true, .observedMidair = a_input.midair };
            }
            return a_jump.replicated;
        }

        static_assert([] {
            LocalJump jump;
            return AdvanceLocalJump(jump, { .requested = true })
                && AdvanceLocalJump(jump, { .requested = true, .midair = true })
                && !AdvanceLocalJump(jump, { .requested = true }) && !AdvanceLocalJump(jump, { .requested = true })
                && AdvanceLocalJump(jump, { .requested = true, .midair = true });
        }());
        static_assert([] {
            LocalJump jump;
            return AdvanceLocalJump(jump, { .requested = true }) && !AdvanceLocalJump(jump, {});
        }());
        static_assert([] {
            LocalJump jump;
            return !AdvanceLocalJump(jump, { .requested = true, .swimming = true })
                && !AdvanceLocalJump(jump, { .requested = true }) && !AdvanceLocalJump(jump, {})
                && AdvanceLocalJump(jump, { .requested = true });
        }());

        Multiplayer::AnimationState GetAnimationState(LocalJump& a_jump, RE::Actor& a_player)
        {
            const auto* state = a_player.AsActorState();
            if (!state)
            {
                return {};
            }

            const bool isMoving = state->actorState1.movingForward || state->actorState1.movingBack
                || state->actorState1.movingLeft || state->actorState1.movingRight;
            Multiplayer::MovementState movementState;
            if (state->IsSneaking())
            {
                if (!isMoving)
                {
                    movementState = Multiplayer::MovementState::kSneakingIdle;
                }
                else
                {
                    movementState = state->IsRunning() ? Multiplayer::MovementState::kSneakingRunning
                                                       : Multiplayer::MovementState::kSneakingWalking;
                }
            }
            else if (!isMoving)
            {
                movementState = Multiplayer::MovementState::kIdle;
            }
            else if (state->IsSprinting())
            {
                movementState = Multiplayer::MovementState::kSprinting;
            }
            else if (state->IsRunning())
            {
                movementState = Multiplayer::MovementState::kRunning;
            }
            else
            {
                movementState = Multiplayer::MovementState::kWalking;
            }

            static const RE::BSFixedString inJumpStateVariable{ "bInJumpState" };
            bool graphJumping = false;
            const bool graphJumpStateAvailable = a_player.GetGraphVariableBool(inJumpStateVariable, graphJumping);
            const bool physicalMidair = a_player.IsInMidair();
            const bool requestedJumping
                = graphJumpStateAvailable ? graphJumping : (a_player.IsInJumpState() || physicalMidair);
            const bool jumping = AdvanceLocalJump(a_jump,
                { .requested = requestedJumping, .midair = physicalMidair, .swimming = state->IsSwimming() });

            return Multiplayer::AnimationState{ .movementState = movementState,
                .jumping = jumping,
                .weaponDrawn = IsWeaponDrawnOrDrawing(state->GetWeaponState()) };
        }
    }

    namespace
    {
        // Direction-key changes left the move input zero for 6-24 ms, and chained
        // attacks dropped the movement bits for about 6 ms with the input held
        // (observed in game); a 20 Hz sample inside such a gap published a stop that
        // peers played as a stop and restart.
        constexpr float kLocalStopHoldSeconds = 0.06F;
        constexpr float kArrivalMoveDistance = 8.0F;
    }

    void ObserveLocalMovement(LocalState& a_state, const LocalFrame& a_local, float a_delta)
    {
        const auto* state = a_local.player.AsActorState();
        const bool moving = state
            && (state->actorState1.movingForward || state->actorState1.movingBack || state->actorState1.movingLeft
                || state->actorState1.movingRight);
        a_state.notMovingElapsed = moving ? 0.0F : a_state.notMovingElapsed + FrameTime::Clamp(a_delta);
    }

    LocalGraphEpoch ObserveLocalGraphEpoch(LocalState& a_state, const LocalFrame& a_local)
    {
        PROFILE_FUNCTION();
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
        const bool graphAvailable = a_local.player.GetAnimationGraphManager(graphManager) && graphManager;
        if (!graphAvailable)
        {
            a_state.graphContinuityLost = a_state.graphContinuityLost || a_state.graphObserved;
            a_state.observedGraph = nullptr;
            return LocalGraphEpoch{ .epoch = a_state.graphEpoch };
        }

        if (!a_state.graphObserved)
        {
            a_state.graphObserved = true;
            a_state.observedGraph = graphManager.get();
            return LocalGraphEpoch{ .epoch = a_state.graphEpoch };
        }
        if (!a_state.graphContinuityLost && a_state.observedGraph == graphManager.get())
        {
            return LocalGraphEpoch{ .epoch = a_state.graphEpoch };
        }
        a_state.observedGraph = graphManager.get();
        a_state.graphContinuityLost = false;
        if (a_state.graphEpoch >= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        {
            logs::critical("Animation sync cannot advance local graph epoch beyond the protocol limit");
            return LocalGraphEpoch{ .epoch = a_state.graphEpoch };
        }
        ++a_state.graphEpoch;
        logs::debug("Animation sync advanced local graph epoch: graphEpoch={}",
            static_cast<unsigned long long>(a_state.graphEpoch));
        return LocalGraphEpoch{ .epoch = a_state.graphEpoch, .changed = true };
    }

    Multiplayer::AnimationState SampleLocalAnimation(LocalState& a_state, const LocalFrame& a_local,
        std::uint64_t a_sequence, const MovementSync::LocalSample& a_movement)
    {
        auto animationState = GetAnimationState(a_state.jump, a_local.player);
        const bool sampledMoving = IsMoving(animationState.movementState);
        const auto& published = a_state.published;
        if (a_state.hasPublished && IsMoving(published.movementState) && !sampledMoving
            && a_state.notMovingElapsed < kLocalStopHoldSeconds)
        {
            logs::debug("Animation sync held local stop: sequence={} notMovingSeconds={:.3f} held={}",
                static_cast<unsigned long long>(a_sequence), a_state.notMovingElapsed,
                Multiplayer::GetMovementStateName(published.movementState));
            animationState.movementState = published.movementState;
        }
        if (a_movement.relocated)
        {
            a_state.holdingArrival = sampledMoving;
            a_state.arrivalPosition = a_movement.position;
        }
        else if (a_state.holdingArrival)
        {
            const float deltaX = a_movement.position.x - a_state.arrivalPosition.x;
            const float deltaY = a_movement.position.y - a_state.arrivalPosition.y;
            const bool moved = deltaX * deltaX + deltaY * deltaY > kArrivalMoveDistance * kArrivalMoveDistance;
            if (!sampledMoving || moved)
            {
                a_state.holdingArrival = false;
                logs::debug("Animation sync released arrival standing: sequence={} reason={} sampled={}",
                    static_cast<unsigned long long>(a_sequence), moved ? "moved" : "bits-cleared",
                    Multiplayer::GetMovementStateName(animationState.movementState));
            }
        }
        if (a_state.holdingArrival && IsMoving(animationState.movementState))
        {
            animationState.movementState = IsSneaking(animationState.movementState)
                ? Multiplayer::MovementState::kSneakingIdle
                : Multiplayer::MovementState::kIdle;
        }
        if (a_state.hasPublished && animationState == published)
        {
            return animationState;
        }
        logs::debug(
            "Animation sync publishing local state: sequence={} previous={}/{} state={}/{} "
            "position=({:.3f},{:.3f},{:.3f}) speed={:.1f} measuredSpeed={:.1f} direction={:.3f} physicalJump={} "
            "physicalMidair={}",
            static_cast<unsigned long long>(a_sequence),
            a_state.hasPublished ? Multiplayer::GetMovementStateName(published.movementState) : "uninitialized",
            a_state.hasPublished && published.jumping ? "jumping" : "grounded",
            Multiplayer::GetMovementStateName(animationState.movementState),
            animationState.jumping ? "jumping" : "grounded", a_movement.position.x, a_movement.position.y,
            a_movement.position.z, MovementSync::GetCheckpointSpeed(a_movement, animationState.movementState),
            a_movement.measuredSpeed, a_movement.direction, a_local.player.IsInJumpState(),
            a_local.player.IsInMidair());
        a_state.published = animationState;
        a_state.hasPublished = true;
        return animationState;
    }

    void OnCheckpointPlayed(const CheckpointUpdate& a_update)
    {
        const bool animationStateChanged
            = a_update.inserted || a_update.previous.animationState != a_update.checkpoint.animationState;
        if (animationStateChanged)
        {
            logs::debug(
                "Animation sync played target: player={} previousSequence={} previous={}/{} sequence={} "
                "state={}/{} position=({:.3f},{:.3f},{:.3f}) speed={:.1f} direction={:.3f}",
                a_update.playerID, static_cast<unsigned long long>(a_update.previousSequence),
                a_update.inserted ? "uninitialized"
                                  : Multiplayer::GetMovementStateName(a_update.previous.animationState.movementState),
                a_update.inserted || !a_update.previous.animationState.jumping ? "grounded" : "jumping",
                static_cast<unsigned long long>(a_update.checkpoint.sequence),
                Multiplayer::GetMovementStateName(a_update.checkpoint.animationState.movementState),
                a_update.checkpoint.animationState.jumping ? "jumping" : "grounded", a_update.checkpoint.x,
                a_update.checkpoint.y, a_update.checkpoint.z, a_update.checkpoint.speed, a_update.checkpoint.direction);
        }
    }

    void SyncAnimation(const RemoteFrame& a_remote, RemoteActor& a_actor, bool a_readyForWeaponDraw, float a_delta)
    {
        PROFILE_FUNCTION();
        if (ActorFurniture::IsFurnitureInteractionPlaying(a_actor)
            || !ActorAnimation::IsAnimationStateSettledExceptLanding(a_actor))
        {
            return;
        }

        const auto current = ActorAnimation::GetTargetState(a_actor);
        const auto target = GetPresentableState(a_remote, a_actor, a_readyForWeaponDraw);
        if (ActorMovement::ShouldDeferLanding(a_actor, current.jumping && !target.jumping, a_delta))
        {
            return;
        }
        if (current == target)
        {
            return;
        }
        if (!ActorAnimation::IsAnimationStateSettled(a_actor) && !target.jumping)
        {
            // Only a consecutive takeoff may interrupt a landing branch.
            return;
        }

        const auto next = GetNextAnimationStep(current, target);
        if (next == current)
        {
            return;
        }
        logs::debug(
            "Animation sync advancing animation state: player={} actor={:08X} sequence={} current={}/{} step={}/{} "
            "target={}/{}",
            a_remote.playerID, a_actor.handle.native_handle(),
            static_cast<unsigned long long>(a_remote.checkpoint.sequence),
            Multiplayer::GetMovementStateName(current.movementState), current.jumping ? "jumping" : "grounded",
            Multiplayer::GetMovementStateName(next.movementState), next.jumping ? "jumping" : "grounded",
            Multiplayer::GetMovementStateName(target.movementState), target.jumping ? "jumping" : "grounded");
        ActorAnimation::SetAnimationState(a_actor, next);
    }

    Multiplayer::AnimationState GetPresentableState(
        const RemoteFrame& a_remote, const RemoteActor& a_actor, bool a_readyForWeaponDraw)
    {
        auto target = a_remote.checkpoint.animationState;
        const auto current = ActorAnimation::GetTargetState(a_actor);
        if (target.weaponDrawn && !current.weaponDrawn && !a_readyForWeaponDraw)
        {
            target.weaponDrawn = false;
        }
        if (HandPresentation::IsPlaying(a_actor))
        {
            target.weaponDrawn = current.weaponDrawn;
            target.jumping = current.jumping;
        }
        if (target.jumping && !current.jumping && ActorAnimation::IsSwimming(a_actor))
        {
            // A jump at swim depth would play in Swim_State.
            target.jumping = false;
        }
        return target;
    }
}
