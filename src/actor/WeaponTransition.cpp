#include "actor/WeaponTransition.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorRegistry.h"
#include "actor/AnimationFirewall.h"
#include "actor/RemoteActor.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"

#include <type_traits>

namespace WeaponTransition
{
    namespace
    {
        using Multiplayer::IsMoving;

        // A one-handed sword draw takes about 1.4 seconds.
        constexpr float kFallbackSeconds = 2.5F;

        constexpr bool IsAnimating(RE::WEAPON_STATE a_weaponState, bool a_weaponDrawn) noexcept
        {
            return a_weaponDrawn ? a_weaponState == RE::WEAPON_STATE::kDrawing
                                 : a_weaponState == RE::WEAPON_STATE::kSheathing;
        }

        static_assert(IsAnimating(RE::WEAPON_STATE::kDrawing, true));
        static_assert(IsAnimating(RE::WEAPON_STATE::kSheathing, false));
        static_assert(!IsAnimating(RE::WEAPON_STATE::kWantToDraw, true));
        static_assert(!IsAnimating(RE::WEAPON_STATE::kWantToSheathe, false));

        constexpr bool ShouldReassert(bool a_pending, bool a_completed, bool a_moving, bool a_jumping) noexcept
        {
            return a_pending && a_completed && (a_moving || a_jumping);
        }

        static_assert(ShouldReassert(true, true, true, false));
        static_assert(ShouldReassert(true, true, false, true));
        static_assert(!ShouldReassert(true, true, false, false));
        static_assert(!ShouldReassert(true, false, true, false));

        constexpr Multiplayer::AnimationState GetReplayBaseState(Multiplayer::AnimationState a_state) noexcept
        {
            a_state.jumping = false;
            switch (a_state.movementState)
            {
                case Multiplayer::MovementState::kSprinting:
                    a_state.movementState = Multiplayer::MovementState::kRunning;
                    break;
                case Multiplayer::MovementState::kSneakingWalking:
                case Multiplayer::MovementState::kSneakingRunning:
                    a_state.movementState = Multiplayer::MovementState::kSneakingIdle;
                    break;
                default:
                    break;
            }
            return a_state;
        }

        static_assert(GetReplayBaseState(Multiplayer::AnimationState{
                          .movementState = Multiplayer::MovementState::kSprinting, .jumping = true })
            == Multiplayer::AnimationState{ .movementState = Multiplayer::MovementState::kRunning });

        bool IsClipPending(const State& a_state)
        {
            return a_state.reassertPending && !a_state.completionObserved && a_state.waitElapsed < kFallbackSeconds;
        }

        RE::WEAPON_STATE GetWeaponState(const RE::Actor& a_actor)
        {
            const auto* actorState = a_actor.AsActorState();
            return actorState ? actorState->GetWeaponState() : RE::WEAPON_STATE::kSheathed;
        }

        constexpr auto ToLogValue(RE::WEAPON_STATE a_weaponState) noexcept
        {
            return static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(a_weaponState);
        }
    }

    Progress Advance(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
    {
        static const RE::BSFixedString weaponEquipOutEvent{ "WeapEquip_Out" };
        static const RE::BSFixedString weaponEquipOutMovingEvent{ "WeapEquip_OutMoving" };
        static const RE::BSFixedString weaponUnequipOutEvent{ "Unequip_Out" };
        static const RE::BSFixedString weaponUnequipOutMovingEvent{ "Unequip_OutMoving" };

        auto& animation = a_remote.animation;
        auto& state = animation.weapon;
        const auto& target = animation.targetAnimationState;
        const auto& dispatch = animation.dispatch;
        const auto actorHandle = a_remote.handle.native_handle();
        const bool weaponDrawnChanged
            = dispatch.hasLatched ? dispatch.latched.weaponDrawn != target.weaponDrawn : target.weaponDrawn;
        if (weaponDrawnChanged)
        {
            state = State{ .reassertPending = true };
        }
        const auto weaponState = GetWeaponState(a_actor);
        const auto observedEnds = ActorRegistry::ConsumeWeaponTransitionEnds(a_remote);
        if (observedEnds != 0)
        {
            state.completionObserved = true;
            const bool lateCompletion = !state.reassertPending && state.restoredWithoutCompletion;
            if (lateCompletion)
            {
                // The bounded fallback restored locomotion while the clip was still
                // playing, so the clip consumed that replay and its exit leaves the
                // graph in a standing stance. Replay locomotion again from this exit.
                state = State{ .reassertPending = true, .completionObserved = true };
            }
            logs::debug(
                "Animation sync observed weapon transition exit: actor={:08X} weaponDrawn={} weaponState={} markers={} "
                "lateCompletion={}",
                actorHandle, target.weaponDrawn, ToLogValue(weaponState), observedEnds, lateCompletion);
            if (target.weaponDrawn)
            {
                // Resetting a successfully drawn graph to its default state makes its
                // behavior state sheathed while ActorState remains drawn. Preserve the
                // drawn branch and only allow its worker-thread exit one update to settle.
                return { .completed = true, .settling = true };
            }
        }
        if (state.reassertPending && !state.completionObserved && !state.exitForced && !state.animationObserved
            && IsAnimating(weaponState, target.weaponDrawn))
        {
            // A request that flips during an opposite transition (for example a
            // draw started just before a sheathe arrives) spends most of the budget
            // on that clip. Timing out mid-sheathe resets the graph before its
            // weaponSheathe annotation, leaving the weapon in hand. Restart the
            // bounded wait once, when the requested clip actually starts.
            state.animationObserved = true;
            state.waitElapsed = 0.0F;
        }
        const bool beastGraph = a_remote.form != Multiplayer::Shapeshift::kNone;
        if (beastGraph && state.reassertPending && !state.completionObserved
            && weaponState == (target.weaponDrawn ? RE::WEAPON_STATE::kDrawn : RE::WEAPON_STATE::kSheathed))
        {
            // The beast graphs have no equip or unequip clips with exit annotations
            // (WerewolfBehavior.hkx takes weapEquip straight into CombatIdle), so the
            // engine's weapon state completes the transition.
            state.completionObserved = true;
        }
        // A takeoff interrupts a playing draw or sheathe while WEAPON_STATE keeps
        // reporting it, and the landing exit starts the clip over (tailUnequip and
        // BeginWeaponSheathe again, observed in game). A wait that ran through the
        // jump timed out mid-sheathe and left the weapon in hand.
        const bool jumpOwnsGraph = dispatch.latched.jumping || animation.landing.completionPending;
        if (state.reassertPending && state.waitElapsed < kFallbackSeconds)
        {
            state.waitElapsed
                = jumpOwnsGraph && !state.completionObserved ? 0.0F : state.waitElapsed + FrameTime::Clamp(a_deltaTime);
        }
        const bool timedOut = state.waitElapsed >= kFallbackSeconds;
        const bool completed = state.completionObserved || timedOut;
        if (beastGraph && completed)
        {
            // No default state to force after a beast's sheathe.
            state.defaultStateDispatched = true;
        }
        if (state.reassertPending && timedOut && !state.completionObserved && !state.exitForced && !beastGraph)
        {
            const bool moving = IsMoving(target.movementState);
            const auto& exitEvent = target.weaponDrawn ? (moving ? weaponEquipOutMovingEvent : weaponEquipOutEvent)
                                                       : (moving ? weaponUnequipOutMovingEvent : weaponUnequipOutEvent);
            const bool exitAccepted = ActorAnimation::SendNetworkAnimationEvent(a_remote, exitEvent);
            state.exitForced = true;
            logs::warn(
                "Animation sync weapon transition completion timed out; sent bounded exit: actor={:08X} weaponDrawn={} "
                "weaponState={} waitedSeconds={:.3f} event={} accepted={}",
                actorHandle, target.weaponDrawn, ToLogValue(weaponState), state.waitElapsed, exitEvent.c_str(),
                exitAccepted);
            if (exitAccepted)
            {
                // Give an accepted exit edge one graph update before restoring locomotion.
                return { .completed = completed, .settling = true };
            }
        }
        if (state.reassertPending && completed && !target.weaponDrawn && !state.defaultStateDispatched && !beastGraph)
        {
            // The unequip exit annotation finishes the clip, but does not guarantee
            // that the behavior graph is in a state that can consume locomotion. A
            // direct moveStart can report success while being ignored. Once ActorState
            // is also sheathed, force the graph's matching default state first and
            // replay authoritative locomotion on the next update.
            const bool defaultStateAccepted = AnimationFirewall::ForceDefaultGraphState(a_actor);
            state.defaultStateDispatched = true;
            logs::debug(
                "Animation sync reset graph to default after weapon transition: actor={:08X} weaponDrawn={} "
                "weaponState={} completionMarkerObserved={} eventAccepted={}",
                actorHandle, target.weaponDrawn, ToLogValue(weaponState), state.completionObserved,
                defaultStateAccepted);
            return { .completed = completed, .settling = true };
        }
        return { .completed = completed };
    }

    Replay PlanReplay(State& a_state, const Multiplayer::AnimationState& a_target,
        const Multiplayer::AnimationState& a_latched, bool a_completed)
    {
        Replay replay{ .state = a_target,
            .active = ShouldReassert(
                a_state.reassertPending, a_completed, IsMoving(a_target.movementState), a_target.jumping) };
        if (a_state.reassertPending && a_state.replayBaseDispatched && !IsMoving(a_target.movementState)
            && !a_target.jumping)
        {
            // Do not carry a partially replayed base across an idle interval.
            a_state.replayBaseDispatched = false;
        }
        // A weapon branch may consume locomotion events. Replay a valid base edge
        // first, then layer sprint/sneak movement/jump on a later graph update.
        // Sending every edge together can make each one report success while no
        // runnable locomotion branch actually becomes active.
        replay.replayingBase = replay.active && !a_state.replayBaseDispatched;
        if (replay.replayingBase)
        {
            replay.state = GetReplayBaseState(a_target);
        }
        else if (replay.active && a_target.jumping)
        {
            auto groundedState = a_target;
            groundedState.jumping = false;
            if (a_latched != groundedState)
            {
                // Sprint/sneak movement is another graph edge. Establish it before
                // dispatching takeoff rather than stacking both on one update.
                replay.state = groundedState;
            }
        }
        replay.needsFollowup = replay.active && replay.state != a_target;
        return replay;
    }

    bool OnReplayLatched(RE::Actor& a_actor, RemoteActor& a_remote, const Replay& a_replay, bool a_eventTimedOut)
    {
        if (!a_replay.active)
        {
            return false;
        }
        auto& state = a_remote.animation.weapon;
        const auto& target = a_remote.animation.targetAnimationState;
        if (a_replay.needsFollowup)
        {
            state.replayBaseDispatched = true;
            logs::debug(
                "Animation sync staged weapon locomotion replay: actor={:08X} step={}/{} target={}/{} weaponDrawn={} "
                "eventTimedOut={}",
                a_remote.handle.native_handle(), Multiplayer::GetMovementStateName(a_replay.state.movementState),
                a_replay.state.jumping ? "jumping" : "grounded",
                Multiplayer::GetMovementStateName(target.movementState), target.jumping ? "jumping" : "grounded",
                target.weaponDrawn, a_eventTimedOut);
            return false;
        }

        const bool completionObserved = state.completionObserved;
        const float waitElapsed = state.waitElapsed;
        state = State{ .restoredWithoutCompletion = !completionObserved };
        logs::debug(
            "Animation sync restored state after weapon transition: actor={:08X} state={} jumping={} weaponDrawn={} "
            "completionMarkerObserved={} weaponState={} transitionWaitSeconds={:.3f} eventTimedOut={}",
            a_remote.handle.native_handle(), Multiplayer::GetMovementStateName(a_replay.state.movementState),
            a_replay.state.jumping, a_replay.state.weaponDrawn, completionObserved, ToLogValue(GetWeaponState(a_actor)),
            waitElapsed, a_eventTimedOut);
        return true;
    }

    bool IsPlaying(const RemoteActor& a_remote)
    {
        return IsClipPending(a_remote.animation.weapon);
    }

    bool IsUnsettled(const RemoteActor& a_remote)
    {
        const auto& state = a_remote.animation.weapon;
        if (!state.reassertPending)
        {
            return false;
        }
        if (IsClipPending(state))
        {
            return true;
        }
        const auto& target = a_remote.animation.targetAnimationState;
        return IsMoving(target.movementState) || target.jumping
            || (!target.weaponDrawn && !state.defaultStateDispatched);
    }

    void RequestNativeTransition(RE::Actor& a_actor, bool a_weaponDrawn)
    {
        PROFILE_FUNCTION();
        const auto* actorState = a_actor.AsActorState();
        if (!actorState)
        {
            return;
        }
        const auto weaponState = actorState->GetWeaponState();
        const bool transitionPending
            = a_weaponDrawn ? weaponState != RE::WEAPON_STATE::kDrawn : weaponState != RE::WEAPON_STATE::kSheathed;
        // WantToDraw and WantToSheathe only record an outstanding request, so keep
        // retrying until the matching transition is actually animating. The equip
        // branch picks standing or moving from iSyncIdleLocomotion.
        if (transitionPending && !IsAnimating(weaponState, a_weaponDrawn))
        {
            a_actor.DrawWeaponMagicHands(a_weaponDrawn);
        }
    }

    void ApplyRiderWeaponState(RE::Actor& a_actor, RemoteActor& a_remote, bool a_weaponDrawn)
    {
        auto& target = a_remote.animation.targetAnimationState;
        if (target.weaponDrawn != a_weaponDrawn)
        {
            target.weaponDrawn = a_weaponDrawn;
            ActorRegistry::SetAuthoritativeWeaponDrawnState(a_remote, a_weaponDrawn);
            logs::debug("Remote rider weapon state changed: actor={:08X} weaponDrawn={} weaponState={}",
                a_remote.handle.native_handle(), a_weaponDrawn, ToLogValue(GetWeaponState(a_actor)));
        }
        RequestNativeTransition(a_actor, a_weaponDrawn);
    }
}
