#include "sync/combat/CombatSync.h"
#include "actor/ActorAnimation.h"
#include "actor/AnimationEvents.h"
#include "actor/RemoteActor.h"
#include "network/SocketIOClient.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace CombatSync
{
    namespace
    {
        // An attack is presentation of what the peer did just now. Replaying it
        // seconds later only extends the swing sequence past the peer's own; this
        // still covers a weapon draw or swing that the attack waits behind.
        constexpr float kAnimationActionTimeoutSeconds = 3.0F;
        constexpr std::uint32_t kMaximumAnimationActionAttempts = 4;
        // Rejected attacks are also retried for at least this long, so a high frame
        // rate cannot spend every attempt inside one short graph blend.
        constexpr float kMinimumAnimationActionRetrySeconds = 0.25F;
        constexpr std::size_t kMaximumQueuedAnimationActions = 32;

        // Captured local attacks waiting for the next published checkpoint.
        std::deque<Network::Attack> s_pendingAttacks;
        // Hand presentation observed since the last published checkpoint.
        Multiplayer::HandPresentation s_currentHandPresentation;
        Multiplayer::HandPresentation s_unpublishedHandPresentation;

        constexpr bool IsMeleeAttackState(RE::ATTACK_STATE_ENUM a_state)
        {
            switch (a_state)
            {
                case RE::ATTACK_STATE_ENUM::kDraw:
                case RE::ATTACK_STATE_ENUM::kSwing:
                case RE::ATTACK_STATE_ENUM::kHit:
                case RE::ATTACK_STATE_ENUM::kNextAttack:
                case RE::ATTACK_STATE_ENUM::kFollowThrough:
                    return true;
                default:
                    return false;
            }
        }

        bool IsHoldingRangedWeapon(const RE::Actor& a_actor)
        {
            const auto* rightHand = a_actor.GetEquippedObject(false);
            const auto* weapon = rightHand ? rightHand->As<RE::TESObjectWEAP>() : nullptr;
            return weapon && (weapon->IsBow() || weapon->IsCrossbow());
        }

        Network::PowerAttackDirection ToPowerAttackDirection(AnimationEvents::PowerAttackEventDirection a_direction)
        {
            switch (a_direction)
            {
                case AnimationEvents::PowerAttackEventDirection::kForward:
                    return Network::PowerAttackDirection::kForward;
                case AnimationEvents::PowerAttackEventDirection::kBackward:
                    return Network::PowerAttackDirection::kBackward;
                case AnimationEvents::PowerAttackEventDirection::kLeft:
                    return Network::PowerAttackDirection::kLeft;
                case AnimationEvents::PowerAttackEventDirection::kRight:
                    return Network::PowerAttackDirection::kRight;
                default:
                    return Network::PowerAttackDirection::kStanding;
            }
        }

        // The hand, or bash, of an attack that just started, and whether it is a
        // power attack in which direction. The attack data is the race's entry for
        // the graph event that started it; without one the attack is presented as
        // a plain right-hand swing.
        Network::Attack ClassifyAttack(const RE::BGSAttackData* a_attackData, RE::ATTACK_STATE_ENUM a_attackState)
        {
            Network::Attack attack{};
            if (a_attackData)
            {
                attack.power = a_attackData->data.flags.any(RE::AttackData::AttackFlag::kPowerAttack);
                if (attack.power)
                {
                    const auto* eventName = a_attackData->event.c_str();
                    attack.direction = ToPowerAttackDirection(
                        AnimationEvents::GetPowerAttackEventDirection(eventName ? eventName : ""));
                }
            }
            const bool bashData = a_attackData && a_attackData->data.flags.any(RE::AttackData::AttackFlag::kBashAttack);
            if (a_attackState == RE::ATTACK_STATE_ENUM::kBash || bashData)
            {
                attack.action = Network::AttackAction::kBash;
                // Attack data left over from a swing does not make the bash a power bash.
                attack.power = attack.power && bashData;
                // A bash has no direction.
                attack.direction = Network::PowerAttackDirection::kStanding;
                return attack;
            }
            if (!a_attackData)
            {
                return attack;
            }
            const auto* eventName = a_attackData->event.c_str();
            switch (AnimationEvents::GetAttackEventHand(eventName ? eventName : ""))
            {
                case AnimationEvents::AttackEventHand::kDual:
                    attack.action = Network::AttackAction::kDual;
                    // Dual-wield and two-fist power attacks have no direction.
                    attack.direction = Network::PowerAttackDirection::kStanding;
                    break;
                case AnimationEvents::AttackEventHand::kLeft:
                    attack.action = Network::AttackAction::kLeft;
                    break;
                default:
                    // CommonLib's IsLeftAttack reads the attack data's left-hand flag.
                    attack.action
                        = a_attackData->IsLeftAttack() ? Network::AttackAction::kLeft : Network::AttackAction::kNormal;
                    break;
            }
            return attack;
        }

        constexpr bool IsBowDrawState(RE::ATTACK_STATE_ENUM a_state)
        {
            // Releasing and later states already let the arrow go.
            return a_state == RE::ATTACK_STATE_ENUM::kBowDraw || a_state == RE::ATTACK_STATE_ENUM::kBowAttached
                || a_state == RE::ATTACK_STATE_ENUM::kBowDrawn;
        }

        constexpr bool IsCastingState(RE::MagicCaster::State a_state)
        {
            // From the cast request through charging, a charged hold, and release.
            // kNone is a readied hand that is not casting; kUnk08 and kUnk09 interrupt.
            switch (a_state)
            {
                case RE::MagicCaster::State::kUnk01:
                case RE::MagicCaster::State::kUnk02:
                case RE::MagicCaster::State::kReady:
                case RE::MagicCaster::State::kUnk04:
                case RE::MagicCaster::State::kCharging:
                case RE::MagicCaster::State::kCasting:
                case RE::MagicCaster::State::kUnk07:
                    return true;
                default:
                    return false;
            }
        }

        bool IsCastingSpell(RE::PlayerCharacter& a_player, bool a_leftHand)
        {
            // Only a hand that peers show as a spell hand can present a cast. A staff
            // uses the same caster but stays a weapon.
            const auto* equipped = a_player.GetEquippedObject(a_leftHand);
            if (!equipped || !(equipped->Is(RE::FormType::Spell) || equipped->Is(RE::FormType::Scroll)))
            {
                return false;
            }
            const auto* caster = a_player.GetMagicCaster(
                a_leftHand ? RE::MagicSystem::CastingSource::kLeftHand : RE::MagicSystem::CastingSource::kRightHand);
            return caster && IsCastingState(caster->state.get());
        }

        // Actor::IsBlocking is false while a bash plays, even when the bash returns
        // to the block. The graph's bashExit chooses the block by iWantBlock, so a
        // bash from a still-held block counts as blocking.
        bool IsBlocking(RE::PlayerCharacter& a_player, RE::ATTACK_STATE_ENUM a_attackState)
        {
            if (IsHoldingRangedWeapon(a_player))
            {
                return false;
            }
            if (a_player.IsBlocking())
            {
                return true;
            }
            if (a_attackState != RE::ATTACK_STATE_ENUM::kBash)
            {
                return false;
            }
            static const RE::BSFixedString wantBlockVariable{ "iWantBlock" };
            std::int32_t wantBlock = 0;
            if (a_player.GetGraphVariableInt(wantBlockVariable, wantBlock))
            {
                return wantBlock != 0;
            }
            const auto* actorState = a_player.AsActorState();
            return actorState && actorState->actorState2.wantBlocking;
        }

        Multiplayer::HandPresentation GetHandPresentation(RE::PlayerCharacter& a_player)
        {
            const auto* actorState = a_player.AsActorState();
            if (!actorState || !actorState->IsWeaponDrawn())
            {
                return {};
            }
            const auto* rightHand = a_player.GetEquippedObject(false);
            const auto* weapon = rightHand ? rightHand->As<RE::TESObjectWEAP>() : nullptr;
            return Multiplayer::HandPresentation{ .bowDrawn
                = weapon && weapon->IsBow() && IsBowDrawState(actorState->GetAttackState()),
                .castingLeft = IsCastingSpell(a_player, true),
                .castingRight = IsCastingSpell(a_player, false),
                // The block button zooms a bow (BowZoom sends blockStart) and bashes
                // with a crossbow, so neither is presented as a block.
                .blocking = IsBlocking(a_player, actorState->GetAttackState()) };
        }
    }

    std::optional<Network::Attack> CaptureLocalAttack(const LocalFrame& a_local)
    {
        static RE::ATTACK_STATE_ENUM previousAttackState = RE::ATTACK_STATE_ENUM::kNone;
        static bool attackWindowActive = false;
        // The latest captured attack of the window is already presented as a power
        // attack or bash, so it can no longer become one.
        static bool attackPowerResolved = false;

        const auto* actorState = a_local.player.AsActorState();
        const auto attackState = actorState ? actorState->GetAttackState() : RE::ATTACK_STATE_ENUM::kNone;
        const bool bashing = attackState == RE::ATTACK_STATE_ENUM::kBash;
        // A bow or crossbow shot passes through kNextAttack and kFollowThrough too.
        // Bow draws are presented by the checkpoint instead, but any weapon bashes.
        const bool attackActive
            = bashing || (!IsHoldingRangedWeapon(a_local.player) && IsMeleeAttackState(attackState));
        const bool priorAttackFinishing = previousAttackState == RE::ATTACK_STATE_ENUM::kNextAttack
            || previousAttackState == RE::ATTACK_STATE_ENUM::kFollowThrough ||
            // The bash clip takes attackStart as a follow-up swing.
            previousAttackState == RE::ATTACK_STATE_ENUM::kBash;
        const bool nextSwingStarted = attackWindowActive && priorAttackFinishing
            && (attackState == RE::ATTACK_STATE_ENUM::kDraw || attackState == RE::ATTACK_STATE_ENUM::kSwing);
        const bool bashStarted = attackWindowActive && bashing && previousAttackState != RE::ATTACK_STATE_ENUM::kBash;
        const bool attackStarted = attackActive && (!attackWindowActive || nextSwingStarted || bashStarted);

        attackWindowActive = attackActive;
        previousAttackState = attackState;
        if (!attackActive)
        {
            attackPowerResolved = false;
            return std::nullopt;
        }

        const auto* process = a_local.player.GetActorRuntimeData().currentProcess;
        const auto* attackData = process && process->high ? process->high->attackData.get() : nullptr;
        const auto* attackEvent = attackData ? attackData->event.c_str() : nullptr;
        if (!attackStarted)
        {
            // A held attack button starts the plain swing, and the engine replaces its
            // attack data with the power attack once the power attack delay passes
            // (observed in game). No attack state edge marks it.
            if (attackPowerResolved || bashing || !attackData
                || !attackData->data.flags.any(RE::AttackData::AttackFlag::kPowerAttack))
            {
                return std::nullopt;
            }
            attackPowerResolved = true;
            const auto attack = ClassifyAttack(attackData, attackState);
            logs::info(
                "Animation sync observed local power attack of the playing swing: state={} "
                "normalizedAction=attack/{} direction={} attackEvent={}",
                static_cast<std::underlying_type_t<RE::ATTACK_STATE_ENUM>>(attackState),
                Network::GetAttackActionName(attack.action), Network::GetPowerAttackDirectionName(attack.direction),
                attackEvent ? attackEvent : "<none>");
            return attack;
        }

        const auto attack = ClassifyAttack(attackData, attackState);
        attackPowerResolved = attack.power || attack.action == Network::AttackAction::kBash;
        logs::info(
            "Animation sync observed local attack state edge: state={} normalizedAction=attack/{} power={} "
            "direction={} attackEvent={} powerAttacking={}",
            static_cast<std::underlying_type_t<RE::ATTACK_STATE_ENUM>>(attackState),
            Network::GetAttackActionName(attack.action), attack.power,
            Network::GetPowerAttackDirectionName(attack.direction), attackEvent ? attackEvent : "<none>",
            a_local.player.IsPowerAttacking());
        return attack;
    }

    bool CaptureLocalHandPresentation(const LocalFrame& a_local)
    {
        const auto previous = s_currentHandPresentation;
        const auto current = GetHandPresentation(a_local.player);
        s_currentHandPresentation = current;
        s_unpublishedHandPresentation.bowDrawn |= current.bowDrawn;
        s_unpublishedHandPresentation.castingLeft |= current.castingLeft;
        s_unpublishedHandPresentation.castingRight |= current.castingRight;
        s_unpublishedHandPresentation.blocking |= current.blocking;
        if (current == previous)
        {
            return false;
        }

        logs::info(
            "Animation sync observed local hand presentation: bowDrawn={} castingLeft={} castingRight={} blocking={} "
            "attackState={}",
            current.bowDrawn, current.castingLeft, current.castingRight, current.blocking,
            static_cast<std::underlying_type_t<RE::ATTACK_STATE_ENUM>>(a_local.player.AsActorState()
                    ? a_local.player.AsActorState()->GetAttackState()
                    : RE::ATTACK_STATE_ENUM::kNone));
        return (current.bowDrawn && !previous.bowDrawn) || (current.castingLeft && !previous.castingLeft)
            || (current.castingRight && !previous.castingRight) || (current.blocking && !previous.blocking);
    }

    Multiplayer::HandPresentation SampleLocalHandPresentation()
    {
        // Capture runs earlier in the same pass, so this always includes the
        // current presentation.
        return std::exchange(s_unpublishedHandPresentation, {});
    }

    void QueueLocalAttacks(std::optional<Network::Attack> a_capturedAttack, bool a_graphEpochChanged)
    {
        if (a_graphEpochChanged)
        {
            // An accepted input from the previous graph cannot be attached to a
            // checkpoint from this new epoch. Historical attacks are not replayed.
            s_pendingAttacks.clear();
        }
        if (a_capturedAttack && s_pendingAttacks.size() < kMaximumQueuedAnimationActions)
        {
            s_pendingAttacks.push_back(*a_capturedAttack);
        }
    }

    void DiscardLocalAttacks()
    {
        s_pendingAttacks.clear();
    }

    void PublishLocalAttacks(const Network::PlayerTransform& a_checkpoint)
    {
        if (a_checkpoint.furnitureRefID != 0)
        {
            s_pendingAttacks.clear();
        }
        else if (a_checkpoint.animationState.weaponDrawn)
        {
            for (const auto& attack : s_pendingAttacks)
            {
                Network::PublishLocalAttack(attack, a_checkpoint.graphEpoch, a_checkpoint.sequence);
            }
            s_pendingAttacks.clear();
        }
    }

    void OnCheckpointAccepted(RemoteState& a_state, const CheckpointUpdate& a_update)
    {
        if (!a_update.inserted && a_update.previous.graphEpoch != a_update.checkpoint.graphEpoch)
        {
            const auto newEpoch = a_update.checkpoint.graphEpoch;
            const auto removed
                = std::erase_if(a_state.pendingAnimationActions, [newEpoch](const PendingAnimationAction& a_pending) {
                      return a_pending.action.graphEpoch != newEpoch;
                  });
            if (removed != 0)
            {
                logs::info(
                    "Animation sync rejected queued actions at graph-epoch boundary: player={} graphEpoch={} "
                    "rejected={}",
                    a_update.playerID, static_cast<unsigned long long>(newEpoch), removed);
            }
        }
        if (a_update.checkpoint.furnitureRefID != 0 && !a_state.pendingAnimationActions.empty())
        {
            logs::info(
                "Animation sync discarded queued attacks at furniture checkpoint: player={} furniture={:08X} "
                "rejected={}",
                a_update.playerID, a_update.checkpoint.furnitureRefID, a_state.pendingAnimationActions.size());
            a_state.pendingAnimationActions.clear();
        }
    }

    void AcceptRemoteAction(
        RemoteState* a_state, const Network::PlayerTransform* a_checkpoint, Network::AnimationAction&& a_action)
    {
        if (!a_state || !a_checkpoint || a_action.graphEpoch < a_checkpoint->graphEpoch
            || a_checkpoint->furnitureRefID != 0)
        {
            logs::info(
                "Animation sync rejected action without a compatible checkpoint: player={} actionSequence={} "
                "graphEpoch={}",
                a_action.playerID, static_cast<unsigned long long>(a_action.actionSequence),
                static_cast<unsigned long long>(a_action.graphEpoch));
            return;
        }
        if (a_state->pendingAnimationActions.size() >= kMaximumQueuedAnimationActions)
        {
            logs::warn("Animation sync rejected action because the bounded queue is full: player={} actionSequence={}",
                a_action.playerID, static_cast<unsigned long long>(a_action.actionSequence));
            return;
        }
        a_state->pendingAnimationActions.push_back(PendingAnimationAction{ .action = std::move(a_action) });
    }

    void AgeRemoteActions(const RemoteFrame& a_remote, RemoteState& a_state, float a_delta)
    {
        for (auto& pending : a_state.pendingAnimationActions)
        {
            pending.age += (std::max)(a_delta, 0.0F);
            if (pending.dispatchAttempts != 0)
            {
                pending.retryElapsed += (std::max)(a_delta, 0.0F);
            }
        }
        while (!a_state.pendingAnimationActions.empty()
            && a_state.pendingAnimationActions.front().age >= kAnimationActionTimeoutSeconds)
        {
            logs::warn("Animation sync timed out queued attack: player={} actionSequence={} graphEpoch={}",
                a_remote.playerID,
                static_cast<unsigned long long>(a_state.pendingAnimationActions.front().action.actionSequence),
                static_cast<unsigned long long>(a_state.pendingAnimationActions.front().action.graphEpoch));
            a_state.pendingAnimationActions.pop_front();
        }
    }

    void ClearRemoteActions(RemoteState& a_state)
    {
        a_state.pendingAnimationActions.clear();
    }

    void SyncCombat(const RemoteFrame& a_remote, RemoteState& a_state, RemoteActor& a_actor, bool a_handsSettled)
    {
        if (a_state.pendingAnimationActions.empty())
        {
            return;
        }
        const bool playing = ActorAnimation::IsAnimationActionPlaying(a_actor);
        // A held attack button publishes the plain swing and then its power attack.
        // When both are queued before the swing starts, the power attack starts
        // directly instead of first showing the beginning of the plain swing.
        if (!playing && a_state.pendingAnimationActions.size() >= 2)
        {
            const auto& plain = a_state.pendingAnimationActions[0].action;
            const auto& power = a_state.pendingAnimationActions[1].action;
            if (!plain.attack.power && plain.attack.action != Network::AttackAction::kBash && power.attack.power
                && power.attack.action == plain.attack.action && power.graphEpoch == plain.graphEpoch)
            {
                logs::info(
                    "Animation sync merged queued attack into its power attack: player={} action={} "
                    "actionSequence={} powerActionSequence={}",
                    a_remote.playerID, Network::GetAttackActionName(plain.attack.action),
                    static_cast<unsigned long long>(plain.actionSequence),
                    static_cast<unsigned long long>(power.actionSequence));
                a_state.pendingAnimationActions.pop_front();
            }
        }
        auto& pending = a_state.pendingAnimationActions.front();
        const auto& action = pending.action;
        // A playing attack only takes the next one as a chained swing in its combo
        // window. Waiting for the whole recovery instead makes every replayed swing
        // slower than the local chain, so spammed attacks pile up behind it. A bash
        // never chains. A power attack of the playing plain swing's hand turns that
        // swing into the power attack instead, as the source's held button did.
        const bool poweringUp = playing && ActorAnimation::CanPowerUpAttack(a_actor, action.attack);
        const bool chaining = playing && !poweringUp;
        if (chaining && !ActorAnimation::CanChainAttack(a_actor, action.attack))
        {
            return;
        }

        if (action.graphEpoch != a_remote.checkpoint.graphEpoch || action.checkpointSequence > a_remote.lastSequence
            || a_remote.checkpoint.animationState.jumping || a_remote.checkpoint.furnitureRefID != 0
            || !a_remote.checkpoint.animationState.weaponDrawn ||
            // A weapon swap published before the attack must reach the hands first.
            !a_handsSettled || (!playing && !ActorAnimation::CanDispatchAttack(a_actor, action.attack)))
        {
            return;
        }

        const bool accepted = ActorAnimation::DispatchAttack(a_actor, action.attack);
        if (!accepted && chaining)
        {
            // A closed combo window is not a rejection of the attack itself; it
            // starts normally once the playing attack completes.
            return;
        }
        ++pending.dispatchAttempts;
        if (accepted)
        {
            logs::info(
                "Animation sync processed attack: player={} actor={:08X} action={} power={} direction={} "
                "actionSequence={} graphEpoch={} checkpointSequence={} poweredUp={} chained={} queued={}",
                a_remote.playerID, a_actor.handle.native_handle(), Network::GetAttackActionName(action.attack.action),
                action.attack.power, Network::GetPowerAttackDirectionName(action.attack.direction),
                static_cast<unsigned long long>(action.actionSequence),
                static_cast<unsigned long long>(action.graphEpoch),
                static_cast<unsigned long long>(action.checkpointSequence), poweringUp, chaining,
                a_state.pendingAnimationActions.size() - 1);
            a_state.pendingAnimationActions.pop_front();
        }
        else if (pending.dispatchAttempts >= kMaximumAnimationActionAttempts
            && pending.retryElapsed >= kMinimumAnimationActionRetrySeconds)
        {
            logs::warn(
                "Animation sync rejected attack after bounded retries: player={} actor={:08X} action={} power={} "
                "actionSequence={} attempts={} retrySeconds={:.3f}",
                a_remote.playerID, a_actor.handle.native_handle(), Network::GetAttackActionName(action.attack.action),
                action.attack.power, static_cast<unsigned long long>(action.actionSequence), pending.dispatchAttempts,
                pending.retryElapsed);
            a_state.pendingAnimationActions.pop_front();
        }
    }

    void SyncHandPresentation(const RemoteFrame& a_remote, RemoteActor& a_actor, bool a_handsSettled, float a_delta)
    {
        const auto& checkpoint = a_remote.checkpoint;
        const auto& target = checkpoint.handPresentation;
        // The stance change waits for the presentation to end (AnimationSync holds
        // it), so a sheathe or jump first releases the draw, stops channelling, and
        // lowers the block.
        const bool stanceAllows = checkpoint.animationState.weaponDrawn && !checkpoint.animationState.jumping
            && checkpoint.furnitureRefID == 0;
        // A spell or bow swap published before the presentation reaches the hands
        // first, so the draw does not start on the previous weapon.
        const bool mayStart = a_handsSettled && !ActorAnimation::IsAnimationActionPlaying(a_actor);

        // A bash is an attack action and plays from the held block, so the block
        // only waits for other attacks to start, never to be held.
        ActorAnimation::ApplyBlock(a_actor, stanceAllows && target.blocking, mayStart, a_delta);
        ActorAnimation::ApplyBowDraw(a_actor, stanceAllows && target.bowDrawn, mayStart, a_delta);
        for (const bool leftHand : { true, false })
        {
            ActorAnimation::ApplySpellCast(
                a_actor, leftHand, stanceAllows && target.IsCasting(leftHand), mayStart, a_delta);
        }
        ActorAnimation::AdvancePresentationLocomotion(a_actor, a_delta);
    }
}
