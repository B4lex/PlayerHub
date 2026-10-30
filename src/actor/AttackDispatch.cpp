#include "actor/AttackDispatch.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorRegistry.h"
#include "actor/HandPresentation.h"
#include "actor/RemoteActor.h"
#include "actor/WeaponTransition.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"

#include <array>
#include <cstddef>
#include <type_traits>

namespace AttackDispatch
{
    namespace
    {
        constexpr float kStartFallbackSeconds = 1.25F;
        constexpr float kPlaybackFallbackSeconds = 4.0F;

        // A swing that has hit can take attackStart as its next combo swing; the graph
        // decides when. A source's vanilla graph accepted it right after the hit and
        // swung after attackWinStart (observed in game), and MCO-style graphs whose clips
        // have no attackWinStart never report kNextAttack or kFollowThrough.
        constexpr bool IsChainWindow(RE::ATTACK_STATE_ENUM a_state) noexcept
        {
            return a_state == RE::ATTACK_STATE_ENUM::kHit || a_state == RE::ATTACK_STATE_ENUM::kNextAttack
                || a_state == RE::ATTACK_STATE_ENUM::kFollowThrough;
        }

        const RE::BSFixedString& GetAttackEvent(const RE::Actor& a_actor, Network::AttackAction a_attack)
        {
            static const RE::BSFixedString attackStartEvent{ "attackStart" };
            static const RE::BSFixedString attackStartLeftHandEvent{ "attackStartLeftHand" };
            static const RE::BSFixedString attackStartH2HLeftEvent{ "AttackStartH2HLeft" };
            static const RE::BSFixedString attackStartDualWieldEvent{ "attackStartDualWield" };
            static const RE::BSFixedString bashStartEvent{ "bashStart" };
            switch (a_attack)
            {
                case Network::AttackAction::kLeft:
                    // An empty left hand punches through the unarmed branch.
                    return a_actor.GetEquippedObject(true) ? attackStartLeftHandEvent : attackStartH2HLeftEvent;
                case Network::AttackAction::kDual:
                    return attackStartDualWieldEvent;
                case Network::AttackAction::kBash:
                    // Plays the whole bash; its bashExit annotation returns to the block or
                    // ready state by iWantBlock.
                    return bashStartEvent;
                default:
                    return attackStartEvent;
            }
        }

        // The vanilla power attack events of the race attack data. A remote actor's
        // graph takes each from its ready state (1hm_behavior.hkx) when its hands
        // match the condition, and plays the NPC variant of the clip.
        const RE::BSFixedString& GetPowerAttackEvent(const RE::Actor& a_actor, const Network::Attack& a_attack)
        {
            static const std::array<RE::BSFixedString, 5> rightHandEvents{ "attackPowerStartInPlace",
                "attackPowerStartForward", "attackPowerStartBackward", "attackPowerStartLeft",
                "attackPowerStartRight" };
            static const std::array<RE::BSFixedString, 5> leftHandEvents{ "attackPowerStartInPlaceLeftHand",
                "attackPowerStartForwardLeftHand", "attackPowerStartBackLeftHand", "attackPowerStartLeftLeftHand",
                "attackPowerStartRightLeftHand" };
            static const RE::BSFixedString unarmedRightEvent{ "attackPowerStartForwardH2HRightHand" };
            static const RE::BSFixedString unarmedLeftEvent{ "attackPowerStartForwardH2HLeftHand" };
            static const RE::BSFixedString unarmedDualEvent{ "attackPowerStartH2HCombo" };
            static const RE::BSFixedString dualWieldEvent{ "attackPowerStartDualWield" };
            static const RE::BSFixedString bashPowerStartEvent{ "bashPowerStart" };
            const auto direction = static_cast<std::size_t>(a_attack.direction);
            // Fists have only a forward power attack per hand.
            const bool rightHandArmed = a_actor.GetEquippedObject(false) != nullptr;
            const bool leftHandArmed = a_actor.GetEquippedObject(true) != nullptr;
            switch (a_attack.action)
            {
                case Network::AttackAction::kLeft:
                    return leftHandArmed ? leftHandEvents[direction] : unarmedLeftEvent;
                case Network::AttackAction::kDual:
                    return rightHandArmed || leftHandArmed ? dualWieldEvent : unarmedDualEvent;
                case Network::AttackAction::kBash:
                    return bashPowerStartEvent;
                default:
                    return rightHandArmed ? rightHandEvents[direction] : unarmedRightEvent;
            }
        }

        // Every new attack owns the graph until it completes, so the locomotion
        // reassert of an earlier release is dropped.
        void BeginPlayback(RemoteActor& a_remote, Network::AttackAction a_action, const RE::Actor& a_actor)
        {
            const auto* actorState = a_actor.AsActorState();
            a_remote.animation.attack = State{ .pending = true,
                .observed = actorState && actorState->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone,
                .bash = a_action == Network::AttackAction::kBash,
                .action = a_action };
            a_remote.animation.dispatch.reassertReason = nullptr;
        }
    }

    bool CanStart(const RemoteActor& a_remote, const Network::Attack& a_attack)
    {
        const auto& state = a_remote.animation;
        if (!GameThread::Require("CanStartAttack") || state.attack.pending || state.dispatch.reassertReason
            || HandPresentation::IsBowOrCastPlaying(state.hands) ||
            // Only a bash plays from a raised block; other attacks wait until it is lowered.
            (a_attack.action != Network::AttackAction::kBash && HandPresentation::IsBlockPlaying(state.hands)) ||
            // The reassert flag stays armed while idle after a completed draw, so only a
            // transition that is still unsettled may hold the attack.
            WeaponTransition::IsUnsettled(a_remote))
        {
            return false;
        }
        const auto actorPtr = a_remote.handle.get();
        const auto* actorState = actorPtr ? ActorAnimation::GetCombatReadyState(a_remote, *actorPtr) : nullptr;
        return actorState && actorState->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone;
    }

    bool CanChain(const RemoteActor& a_remote, const Network::Attack& a_attack)
    {
        const auto& attack = a_remote.animation.attack;
        if (!GameThread::Require("CanChainAttack") || a_attack.action == Network::AttackAction::kBash || !attack.pending
            || !attack.observed || attack.chainDispatched)
        {
            return false;
        }
        const auto actorPtr = a_remote.handle.get();
        const auto* actorState = actorPtr ? ActorAnimation::GetCombatReadyState(a_remote, *actorPtr) : nullptr;
        return actorState && IsChainWindow(actorState->GetAttackState());
    }

    bool CanPowerUp(const RemoteActor& a_remote, const Network::Attack& a_attack)
    {
        const auto& attack = a_remote.animation.attack;
        // The actor's attack state is not waited for: a remote actor reports it well
        // into the swing, which showed most of the plain swing before the power
        // attack. A graph not yet in its attack state rejects the event and the
        // action is retried.
        if (!GameThread::Require("CanPowerUpAttack") || !a_attack.power
            || a_attack.action == Network::AttackAction::kBash || !attack.pending || attack.bash || attack.power
            || attack.action != a_attack.action)
        {
            return false;
        }
        const auto actorPtr = a_remote.handle.get();
        return actorPtr && ActorAnimation::GetCombatReadyState(a_remote, *actorPtr);
    }

    bool Dispatch(RemoteActor& a_remote, const Network::Attack& a_attack)
    {
        const bool poweringUp = CanPowerUp(a_remote, a_attack);
        const bool chaining = !poweringUp && CanChain(a_remote, a_attack);
        if (!poweringUp && !chaining && !CanStart(a_remote, a_attack))
        {
            return false;
        }

        static const RE::BSFixedString attackStartEvent{ "attackStart" };
        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return false;
        }
        auto& attack = a_remote.animation.attack;
        const auto& plainEvent = GetAttackEvent(*actorPtr, a_attack.action);
        const auto& attackEvent = a_attack.power ? GetPowerAttackEvent(*actorPtr, a_attack) : plainEvent;
        const bool startsNewAttack = !chaining && !poweringUp;
        if (startsNewAttack)
        {
            // An exit left over from an earlier bash must not end this attack.
            (void)ActorRegistry::ConsumeBashExits(*actorPtr);
        }
        auto& actorFlags = actorPtr->GetActorRuntimeData().boolFlags;
        // The persistent flag prevents autonomous combat decisions, but it can also
        // make the behavior graph reject a directly replicated attack. Open a
        // game-thread-only dispatch window and restore the policy before returning;
        // no AI update can run inside this synchronous call.
        actorFlags.reset(RE::Actor::BOOL_FLAGS::kAttackingDisabled);
        bool accepted = ActorAnimation::SendNetworkAnimationEvent(a_remote, attackEvent);
        // A rejected chain only means the combo window does not take it.
        const bool powerFellBack = !accepted && a_attack.power && startsNewAttack
            && ActorAnimation::SendNetworkAnimationEvent(a_remote, plainEvent);
        accepted = accepted || powerFellBack;
        // The dual-wield branch requires two one-handed weapons in the actor's hand types.
        const bool dualFellBack = !accepted && a_attack.action == Network::AttackAction::kDual
            && (!a_attack.power || startsNewAttack)
            && ActorAnimation::SendNetworkAnimationEvent(a_remote, attackStartEvent);
        accepted = accepted || dualFellBack;
        actorFlags.set(RE::Actor::BOOL_FLAGS::kAttackingDisabled);
        if (!accepted)
        {
            attack.chainRejections += chaining ? 1 : 0;
            return false;
        }
        if (a_attack.power)
        {
            const auto* actorValueOwner = actorPtr->AsActorValueOwner();
            logs::debug(
                "Animation sync dispatched power attack: actor={:08X} action={} direction={} event={} "
                "presentedAsPlain={} presentedAsRightHand={} poweredUpPlayingAttack={} chained={} stamina={:.1f}",
                a_remote.handle.native_handle(), Network::GetAttackActionName(a_attack.action),
                Network::GetPowerAttackDirectionName(a_attack.direction), attackEvent.c_str(), powerFellBack,
                dualFellBack, poweringUp, chaining,
                actorValueOwner ? actorValueOwner->GetActorValue(RE::ActorValue::kStamina) : 0.0F);
        }
        else if (dualFellBack)
        {
            logs::debug("Animation sync presented dual-wield attack as a right-hand attack: actor={:08X}",
                a_remote.handle.native_handle());
        }

        if (startsNewAttack)
        {
            BeginPlayback(a_remote, a_attack.action, *actorPtr);
            attack.power = a_attack.power && !powerFellBack;
            return true;
        }
        attack.elapsed = 0.0F;
        attack.power = a_attack.power && !powerFellBack;
        if (poweringUp)
        {
            // The power attack plays on in the attack state. Its own attack state is
            // observed again, so the transition cannot end ownership early.
            attack.observed = false;
            return true;
        }
        // The chained swing starts on a later update, or only after attackWinStart;
        // until then the actor state still reports the hit or the combo window.
        attack.action = a_attack.action;
        attack.chainDispatched = true;
        ++attack.chainCount;
        return true;
    }

    void Own(RemoteActor& a_remote, const RE::Actor& a_actor)
    {
        BeginPlayback(a_remote, Network::AttackAction::kNormal, a_actor);
    }

    bool IsPlaying(const RemoteActor& a_remote)
    {
        return a_remote.animation.attack.pending;
    }

    void Advance(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
    {
        auto& state = a_remote.animation.attack;
        if (!state.pending)
        {
            return;
        }

        state.elapsed += FrameTime::Clamp(a_deltaTime);
        const auto* actorState = a_actor.AsActorState();
        const auto attackState = actorState ? actorState->GetAttackState() : RE::ATTACK_STATE_ENUM::kNone;
        const bool attacking = attackState != RE::ATTACK_STATE_ENUM::kNone;
        state.observed = state.observed || attacking;
        if (attackState != state.loggedState)
        {
            logs::debug(
                "Animation sync remote attack state changed: actor={:08X} state={} previousState={} elapsed={:.3f} "
                "chainDispatched={} chainRejections={}",
                a_actor.GetHandle().native_handle(),
                static_cast<std::underlying_type_t<RE::ATTACK_STATE_ENUM>>(attackState),
                static_cast<std::underlying_type_t<RE::ATTACK_STATE_ENUM>>(state.loggedState), state.elapsed,
                state.chainDispatched, state.chainRejections);
            state.loggedState = attackState;
        }
        // A chained swing writes kSwing on its weaponSwing annotation.
        if (state.chainDispatched
            && (attackState == RE::ATTACK_STATE_ENUM::kSwing || attackState == RE::ATTACK_STATE_ENUM::kDraw))
        {
            state.chainDispatched = false;
        }
        const bool bashExited = state.bash && ActorRegistry::ConsumeBashExits(a_actor) != 0;
        const bool completed = (state.observed && !attacking) || bashExited;
        const bool startTimedOut = !completed && !state.observed && state.elapsed >= kStartFallbackSeconds;
        const bool playbackTimedOut = state.elapsed >= kPlaybackFallbackSeconds;
        if (!completed && !startTimedOut && !playbackTimedOut)
        {
            return;
        }

        logs::debug(
            "Animation sync completed attack ownership: actor={:08X} bash={} bashExited={} observedPlayback={} "
            "elapsed={:.3f} startTimedOut={} playbackTimedOut={} chained={} chainRejections={} chainPending={}",
            a_actor.GetHandle().native_handle(), state.bash, bashExited, state.observed, state.elapsed, startTimedOut,
            playbackTimedOut, state.chainCount, state.chainRejections, state.chainDispatched);
        state = {};
        // Native attacks normally return to locomotion themselves. The explicit
        // replay keeps the drawn stance; a full graph reset would start another
        // equip lifecycle.
        a_remote.animation.dispatch.reassertReason = "attack";
    }
}
