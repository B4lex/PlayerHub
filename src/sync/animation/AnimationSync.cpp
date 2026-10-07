#include "sync/animation/AnimationSync.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorFurniture.h"
#include "actor/ActorMovement.h"
#include "actor/RemoteActor.h"
#include "runtime/Profiling.h"

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

        Multiplayer::AnimationState GetAnimationState(RE::Actor& a_player)
        {
            // bInJumpState can remain set throughout graph-owned landing cleanup.
            static bool replicatedJumping = false;
            static bool observedMidair = false;
            static bool waitForGraphJumpReset = false;

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
            const bool physicalJumping = a_player.IsInJumpState();
            const bool physicalMidair = a_player.IsInMidair();
            const bool requestedJumping = graphJumpStateAvailable ? graphJumping : (physicalJumping || physicalMidair);

            if (state->IsSwimming())
            {
                // The jump input only swims upward, but the remote actor's swim graph
                // still accepts a jump event.
                replicatedJumping = false;
                observedMidair = false;
                waitForGraphJumpReset = requestedJumping;
            }
            else if (replicatedJumping)
            {
                observedMidair = observedMidair || physicalMidair;
                if (observedMidair && !physicalMidair)
                {
                    // Physical contact is the landing boundary; a stale graph jump state is
                    // suppressed until it has reset once.
                    replicatedJumping = false;
                    observedMidair = false;
                    waitForGraphJumpReset = requestedJumping;
                }
                else if (!observedMidair && !requestedJumping)
                {
                    // Also release a takeoff that was cancelled before leaving the ground.
                    replicatedJumping = false;
                }
            }
            else if (waitForGraphJumpReset)
            {
                if (!requestedJumping)
                {
                    waitForGraphJumpReset = false;
                }
                else if (physicalMidair)
                {
                    // Skyrim accepts a new takeoff from its landing branch, so
                    // bInJumpState may never reset between consecutive jumps. Leaving
                    // the ground again after the published landing is that new jump.
                    waitForGraphJumpReset = false;
                    replicatedJumping = true;
                    observedMidair = true;
                }
            }
            else if (requestedJumping)
            {
                replicatedJumping = true;
                observedMidair = physicalMidair;
            }

            return Multiplayer::AnimationState{
                .movementState = movementState, .jumping = replicatedJumping, .weaponDrawn = state->IsWeaponDrawn()
            };
        }

        Multiplayer::AnimationState GetPresentableAnimationState(
            const RemoteFrame& a_remote, const RemoteActor& a_actor, bool a_readyForWeaponDraw)
        {
            auto target = a_remote.checkpoint.animationState;
            const auto current = ActorAnimation::GetTargetState(a_actor);
            if (target.weaponDrawn && !current.weaponDrawn && !a_readyForWeaponDraw)
            {
                target.weaponDrawn = false;
            }
            if (ActorAnimation::IsHandPresentationPlaying(a_actor))
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

    LocalGraphEpoch ObserveLocalGraphEpoch(const LocalFrame& a_local)
    {
        PROFILE_FUNCTION();
        static RE::BSAnimationGraphManager* observedGraph = nullptr;
        static std::uint64_t graphEpoch = 1;
        static bool graphWasObserved = false;
        static bool continuityLost = false;
        bool epochChanged = false;

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
        const bool graphAvailable = a_local.player.GetAnimationGraphManager(graphManager) && graphManager;
        if (!graphAvailable)
        {
            continuityLost = continuityLost || graphWasObserved;
            observedGraph = nullptr;
            return LocalGraphEpoch{ .epoch = graphEpoch, .changed = epochChanged };
        }

        if (!graphWasObserved)
        {
            graphWasObserved = true;
            observedGraph = graphManager.get();
            return LocalGraphEpoch{ .epoch = graphEpoch, .changed = epochChanged };
        }
        if (continuityLost || observedGraph != graphManager.get())
        {
            if (graphEpoch >= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
            {
                logs::critical("Animation sync cannot advance local graph epoch beyond the protocol limit");
                observedGraph = graphManager.get();
                continuityLost = false;
                return LocalGraphEpoch{ .epoch = graphEpoch, .changed = epochChanged };
            }
            ++graphEpoch;
            epochChanged = true;
            observedGraph = graphManager.get();
            continuityLost = false;
            logs::debug("Animation sync advanced local graph epoch: graphEpoch={}",
                static_cast<unsigned long long>(graphEpoch));
        }
        return LocalGraphEpoch{ .epoch = graphEpoch, .changed = epochChanged };
    }

    Multiplayer::AnimationState SampleLocalAnimation(
        const LocalFrame& a_local, std::uint64_t a_sequence, const MovementSync::LocalSample& a_movement)
    {
        static Multiplayer::AnimationState previousAnimationState{};
        static bool hasPreviousAnimationState = false;

        const auto animationState = GetAnimationState(a_local.player);
        if (!hasPreviousAnimationState || animationState != previousAnimationState)
        {
            logs::debug(
                "Animation sync publishing local state: sequence={} previous={}/{} state={}/{} "
                "position=({:.3f},{:.3f},{:.3f}) speed={:.1f} direction={:.3f} physicalJump={} physicalMidair={}",
                static_cast<unsigned long long>(a_sequence),
                hasPreviousAnimationState ? Multiplayer::GetMovementStateName(previousAnimationState.movementState)
                                          : "uninitialized",
                hasPreviousAnimationState && previousAnimationState.jumping ? "jumping" : "grounded",
                Multiplayer::GetMovementStateName(animationState.movementState),
                animationState.jumping ? "jumping" : "grounded", a_movement.position.x, a_movement.position.y,
                a_movement.position.z, a_movement.speed, a_movement.direction, a_local.player.IsInJumpState(),
                a_local.player.IsInMidair());
            previousAnimationState = animationState;
            hasPreviousAnimationState = true;
        }
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
        const auto target = GetPresentableAnimationState(a_remote, a_actor, a_readyForWeaponDraw);
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

    std::optional<Multiplayer::AnimationState> GetTranslationState(
        const RemoteFrame& a_remote, const RemoteActor& a_actor, bool a_readyForWeaponDraw, bool a_furniturePlaying)
    {
        PROFILE_FUNCTION();
        const auto presentableState = GetPresentableAnimationState(a_remote, a_actor, a_readyForWeaponDraw);
        std::optional<Multiplayer::AnimationState> translationState;
        if (!a_furniturePlaying)
        {
            const auto targetState = ActorAnimation::GetTargetState(a_actor);
            if (targetState == presentableState)
            {
                translationState = presentableState;
            }
            else if (ActorAnimation::IsAnimationActionPlaying(a_actor) || targetState.jumping)
            {
                translationState = targetState.jumping ? targetState : presentableState;
            }
        }
        return translationState;
    }
}
