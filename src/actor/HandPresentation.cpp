#include "actor/HandPresentation.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorRegistry.h"
#include "actor/GraphStance.h"
#include "actor/RemoteActor.h"
#include "actor/WeaponTransition.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace HandPresentation
{
    namespace
    {
        constexpr float kEventRetrySeconds = 0.1F;
        constexpr float kBowReleaseFallbackSeconds = 1.5F;
        // The release clip's length.
        constexpr float kBowShotSeconds = 0.5F;
        constexpr float kBowShotFallbackSeconds = 2.0F;
        constexpr float kCastStopFallbackSeconds = 1.0F;
        // How long a source release waits for the receiver's own charge, which started
        // later by the start's delay.
        constexpr float kReleaseChargeWaitSeconds = 1.0F;
        constexpr float kReleaseFinishFallbackSeconds = 3.0F;
        // iState read 10 while a cast played and still about 0.2 s after both casters
        // were idle with IsCasting* cleared; spell-ready locomotion reads 9 (observed
        // in game). A moveStart in that window was accepted by the exiting cast lower
        // body, which then left the actor in the standing spell-ready state.
        constexpr std::int32_t kCastGraphStateTag = 10;
        constexpr float kBlockStopFallbackSeconds = 1.5F;

        constexpr bool IsBowAttackState(RE::ATTACK_STATE_ENUM a_state) noexcept
        {
            return a_state >= RE::ATTACK_STATE_ENUM::kBowDraw && a_state <= RE::ATTACK_STATE_ENUM::kBowFollowThrough;
        }

        static_assert(IsBowAttackState(RE::ATTACK_STATE_ENUM::kBowDrawn));
        static_assert(!IsBowAttackState(RE::ATTACK_STATE_ENUM::kFollowThrough));

        void CoolDown(float& a_cooldown, float a_deltaTime)
        {
            a_cooldown = (std::max)(a_cooldown - a_deltaTime, 0.0F);
        }

        // The graph returns to its block state after a bash, recoil, or attack only
        // while iWantBlock is set, which the engine derives from wantBlocking.
        void SetBlockRequest(RE::Actor& a_actor, bool a_blocking)
        {
            static const RE::BSFixedString wantBlockVariable{ "iWantBlock" };
            if (auto* actorState = a_actor.AsActorState())
            {
                actorState->actorState2.wantBlocking = a_blocking;
            }
            (void)a_actor.SetGraphVariableInt(wantBlockVariable, a_blocking ? 1 : 0);
        }

        bool IsHoldingBow(const RE::Actor& a_actor)
        {
            const auto* rightHand = a_actor.GetEquippedObject(false);
            const auto* weapon = rightHand ? rightHand->As<RE::TESObjectWEAP>() : nullptr;
            return weapon && weapon->IsBow();
        }

        bool IsHoldingSpell(const RE::Actor& a_actor, bool a_leftHand)
        {
            const auto* hand = a_actor.GetEquippedObject(a_leftHand);
            return hand && hand->Is(RE::FormType::Spell);
        }

        bool CanStart(const RemoteActor& a_remote, const RE::Actor& a_actor)
        {
            const auto& state = a_remote.animation;
            return !state.attack.pending && !state.dispatch.reassertReason && !state.hands.bow.releasePending
                && !WeaponTransition::IsUnsettled(a_remote) && ActorAnimation::GetCombatReadyState(a_remote, a_actor);
        }

        bool SendEvent(RemoteActor& a_remote, RE::Actor& a_actor, const RE::BSFixedString& a_eventName)
        {
            // The same synchronous dispatch window as an attack: the persistent
            // suppression flags can make the graph reject a replicated bow draw.
            // They are restored before any AI update can run.
            auto& actorFlags = a_actor.GetActorRuntimeData().boolFlags;
            actorFlags.reset(RE::Actor::BOOL_FLAGS::kAttackingDisabled, RE::Actor::BOOL_FLAGS::kCastingDisabled);
            const bool accepted = ActorAnimation::SendNetworkAnimationEvent(a_remote, a_eventName);
            actorFlags.set(RE::Actor::BOOL_FLAGS::kAttackingDisabled, RE::Actor::BOOL_FLAGS::kCastingDisabled);
            return accepted;
        }
    }

    void ApplyBowDraw(RemoteActor& a_remote, bool a_drawn, bool a_mayStart, float a_deltaTime)
    {
        if (!GameThread::Require("ApplyBowDraw"))
        {
            return;
        }
        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return;
        }

        static const RE::BSFixedString bowAttackStartEvent{ "bowAttackStart" };
        static const RE::BSFixedString attackReleaseEvent{ "attackRelease" };
        static const RE::BSFixedString attackStopEvent{ "attackStop" };
        static const RE::BSFixedString bowResetEvent{ "bowReset" };

        auto& actor = *actorPtr;
        auto& state = a_remote.animation;
        auto& bow = state.hands.bow;
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        CoolDown(bow.eventCooldown, deltaTime);

        if (bow.releasePending)
        {
            bow.phaseElapsed += deltaTime;
            // A remote actor's attack state does not follow its graph, so the shot is
            // given a fixed time. A state that does report a bow attack extends it.
            const auto* actorState = actor.AsActorState();
            const bool attackStateBusy = actorState && IsBowAttackState(actorState->GetAttackState());
            const bool completed = bow.phaseElapsed >= kBowShotSeconds && !attackStateBusy;
            const bool timedOut = bow.phaseElapsed >= kBowShotFallbackSeconds;
            if (!completed && !timedOut)
            {
                return;
            }
            logs::debug("Animation sync completed bow shot ownership: actor={:08X} elapsed={:.3f} timedOut={}",
                a_remote.handle.native_handle(), bow.phaseElapsed, timedOut);
            bow.releasePending = false;
            bow.phaseElapsed = 0.0F;
            state.dispatch.reassertReason = "bow-shot";
            return;
        }

        if (bow.drawHeld)
        {
            bow.phaseElapsed += deltaTime;
            if (a_drawn)
            {
                return;
            }

            bow.releaseRequestElapsed += deltaTime;
            if (bow.eventCooldown > 0.0F)
            {
                return;
            }
            bow.eventCooldown = kEventRetrySeconds;
            // An early release is rejected until the draw can take it.
            const bool released = SendEvent(a_remote, actor, attackReleaseEvent);
            const bool releaseTimedOut = bow.releaseRequestElapsed >= kBowReleaseFallbackSeconds;
            if (!released && !releaseTimedOut)
            {
                return;
            }
            const bool cancelled = !released
                && (SendEvent(a_remote, actor, attackStopEvent) || SendEvent(a_remote, actor, bowResetEvent));
            logs::debug(
                "Animation sync released bow draw: actor={:08X} released={} cancelled={} heldSeconds={:.3f} "
                "releaseWaitSeconds={:.3f}",
                a_remote.handle.native_handle(), released, cancelled, bow.phaseElapsed, bow.releaseRequestElapsed);
            bow.drawHeld = false;
            bow.releasePending = true;
            bow.phaseElapsed = 0.0F;
            return;
        }

        if (!a_drawn || !a_mayStart || bow.eventCooldown > 0.0F || IsBlockPlaying(state.hands) || !IsHoldingBow(actor)
            || !CanStart(a_remote, actor))
        {
            return;
        }
        bow.eventCooldown = kEventRetrySeconds;
        if (!SendEvent(a_remote, actor, bowAttackStartEvent))
        {
            return;
        }

        bow.drawHeld = true;
        bow.phaseElapsed = 0.0F;
        bow.releaseRequestElapsed = 0.0F;
        // A release that is already due goes out on the next pass.
        bow.eventCooldown = 0.0F;
        logs::debug("Animation sync started bow draw: actor={:08X}", a_remote.handle.native_handle());
    }

    void ApplySpellCast(RemoteActor& a_remote, const CastRequest& a_request, bool a_mayStart, float a_deltaTime)
    {
        if (!GameThread::Require("ApplySpellCast"))
        {
            return;
        }
        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return;
        }

        auto& actor = *actorPtr;
        auto& state = a_remote.animation;
        auto& cast = state.hands.cast;
        const bool leftHand = a_request.leftHand;
        const std::size_t hand = leftHand ? 1 : 0;
        const char* handName = leftHand ? "left" : "right";
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        static const RE::BSFixedString wantCastLeftVariable{ "bWantCastLeft" };
        static const RE::BSFixedString wantCastRightVariable{ "bWantCastRight" };
        const auto& wantCastVariable = leftHand ? wantCastLeftVariable : wantCastRightVariable;
        static const RE::BSFixedString castingLeftVariable{ "IsCastingLeft" };
        static const RE::BSFixedString castingRightVariable{ "IsCastingRight" };
        const auto& castingVariable = leftHand ? castingLeftVariable : castingRightVariable;
        const auto setCastOwnership = [&a_remote, &state](bool a_owned) {
            if (state.castAnimationOwned != a_owned)
            {
                state.castAnimationOwned = a_owned;
                ActorRegistry::SetCastAnimationOwned(a_remote, a_owned);
            }
        };
        const auto releaseCastOwnership = [&cast, &setCastOwnership]() {
            setCastOwnership(cast.held[0] || cast.held[1] || cast.stopping[0] || cast.stopping[1]);
        };
        const auto beginStop = [&cast, hand]() {
            cast.held[hand] = false;
            cast.stopping[hand] = true;
            cast.stopRequestElapsed[hand] = 0.0F;
            cast.releaseRequested[hand] = false;
            cast.released[hand] = false;
            cast.releaseElapsed[hand] = 0.0F;
        };
        CoolDown(cast.eventCooldown[hand], deltaTime);
        if (!a_request.casting || !a_request.released)
        {
            cast.awaitingNewCast[hand] = false;
        }

        // The caster, not raw graph events, drives the cast: the magic graph
        // interrupts a cast its caster did not request.
        auto* caster = actor.GetMagicCaster(
            leftHand ? RE::MagicSystem::CastingSource::kLeftHand : RE::MagicSystem::CastingSource::kRightHand);
        const auto casterState = caster ? caster->state.get() : RE::MagicCaster::State::kNone;

        if (cast.stopping[hand])
        {
            cast.stopRequestElapsed[hand] += deltaTime;
            // The caster returns to idle as soon as the cast is interrupted, but the
            // graph leaves its casting state later. A new cast waits for both, so it
            // does not enter a concentration loop that is still playing.
            // The last cast to stop also waits for the graph to leave the cast branch,
            // so the locomotion replay below reaches the spell-ready locomotion.
            bool graphCasting = false;
            actor.GetGraphVariableBool(castingVariable, graphCasting);
            const std::size_t otherHand = 1 - hand;
            const bool lastCast = !cast.held[otherHand] && !cast.stopping[otherHand];
            const auto graphStateTag = GraphStance::ReadStateTag(actor);
            const bool graphLeftCast = !lastCast || graphStateTag != kCastGraphStateTag;
            const bool idle = casterState == RE::MagicCaster::State::kNone && !graphCasting && graphLeftCast;
            const bool timedOut = cast.stopRequestElapsed[hand] >= kCastStopFallbackSeconds;
            if (!idle && !timedOut)
            {
                return;
            }
            const float stopSeconds = cast.stopRequestElapsed[hand];
            cast.stopping[hand] = false;
            cast.stopRequestElapsed[hand] = 0.0F;
            releaseCastOwnership();
            // The cast's lower body exits into its standing state, so a moving actor
            // would keep translating under an idle pose and slide, and its recorded
            // moving state would reject the next moveStop. As after an attack, replay
            // locomotion without touching the stance, once the last hand presentation
            // has left the graph.
            const bool presentationEnded = !IsPlaying(a_remote);
            if (presentationEnded)
            {
                state.dispatch.reassertReason = "cast";
            }
            logs::debug(
                "Animation sync stopped spell cast: actor={:08X} hand={} idle={} stopSeconds={:.3f} "
                "reassertedLocomotion={} iState={}",
                a_remote.handle.native_handle(), handName, idle, stopSeconds, presentationEnded, graphStateTag);
            return;
        }

        if (cast.held[hand])
        {
            const bool holdingSpell = IsHoldingSpell(actor, leftHand);
            const auto* spell = caster ? caster->currentSpell : nullptr;
            // A concentration caster releases itself once charged.
            const bool releasable = spell && spell->GetCastingType() != RE::MagicSystem::CastingType::kConcentration;
            if (a_request.released && releasable && !cast.releaseRequested[hand])
            {
                cast.releaseRequested[hand] = true;
                cast.releaseElapsed[hand] = 0.0F;
            }
            // Spell fire finishes a released cast (CombatIsolation), which also clears
            // the want-cast variable, so it is not set again. The engine releases by
            // itself when the ready action is rejected.
            if (releasable && casterState == RE::MagicCaster::State::kNone)
            {
                const bool releasedHere = cast.released[hand];
                const float releaseSeconds = cast.releaseElapsed[hand];
                beginStop();
                cast.awaitingNewCast[hand] = a_request.casting;
                logs::debug(
                    "Animation sync finished released spell cast: actor={:08X} hand={} releasedHere={} "
                    "releaseSeconds={:.3f}",
                    a_remote.handle.native_handle(), handName, releasedHere, releaseSeconds);
                return;
            }
            if (cast.released[hand])
            {
                cast.releaseElapsed[hand] += deltaTime;
                if (holdingSpell && cast.releaseElapsed[hand] < kReleaseFinishFallbackSeconds)
                {
                    return;
                }
            }
            else if (cast.releaseRequested[hand] && holdingSpell)
            {
                cast.releaseElapsed[hand] += deltaTime;
                if (casterState == RE::MagicCaster::State::kReady)
                {
                    // MagicCaster::StartCast (ID 34445), which the player's release of a
                    // charged hold and NPC combat call: enter kUnk04, then StartCastImpl
                    // performs the hand's release action.
                    caster->state = RE::MagicCaster::State::kUnk04;
                    caster->StartCastImpl();
                    logs::debug("Animation sync released spell cast: actor={:08X} hand={} waitSeconds={:.3f}",
                        a_remote.handle.native_handle(), handName, cast.releaseElapsed[hand]);
                    cast.released[hand] = true;
                    cast.releaseElapsed[hand] = 0.0F;
                    return;
                }
                if (cast.releaseElapsed[hand] < kReleaseChargeWaitSeconds)
                {
                    actor.SetGraphVariableBool(wantCastVariable, true);
                    return;
                }
            }
            else if (a_request.casting && holdingSpell)
            {
                // The magic graph stays in its casting state only while this
                // variable is set, and emits CastStop as soon as it clears.
                actor.SetGraphVariableBool(wantCastVariable, true);
                return;
            }
            logs::debug(
                "Animation sync interrupting spell cast: actor={:08X} hand={} casterState={} releaseRequested={} "
                "released={} holdingSpell={}",
                a_remote.handle.native_handle(), handName, static_cast<std::uint32_t>(casterState),
                cast.releaseRequested[hand], cast.released[hand], holdingSpell);
            beginStop();
            // A cast the source already released is not started again.
            cast.awaitingNewCast[hand] = releasable && a_request.casting && a_request.released;
            actor.SetGraphVariableBool(wantCastVariable, false);
            // Only the interrupt performs the hand's interrupt action. FinishCast
            // returns the caster to idle but leaves a concentration clip looping
            // (observed in game). InterruptCast also finishes the cast.
            if (caster)
            {
                caster->InterruptCast(false);
            }
            return;
        }

        releaseCastOwnership();
        if (!a_request.casting || !a_mayStart || !caster || cast.awaitingNewCast[hand]
            || cast.eventCooldown[hand] > 0.0F || state.hands.bow.drawHeld || IsBlockPlaying(state.hands)
            || !IsHoldingSpell(actor, leftHand) || !CanStart(a_remote, actor))
        {
            return;
        }
        cast.eventCooldown[hand] = kEventRetrySeconds;

        auto* spell = actor.GetEquippedObject(leftHand)->As<RE::MagicItem>();
        // The caster sends its graph events synchronously and on later updates, and
        // checks the casting-disabled flag, so both are opened before the request.
        setCastOwnership(true);
        actor.GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kCastingDisabled);
        if (caster->currentSpell != spell)
        {
            caster->SetCurrentSpell(spell);
        }
        // Actor::TryDualCast (ID 38762) sets the high process flag that the
        // IsDualCasting idle condition reads, then RequestCast (ID 34401) copies it to
        // the caster. FinishCastImpl clears both, but a graph reset drops a cast
        // without finishing it, so a left cast always sets them.
        const bool dual = leftHand && a_request.dual && actor.GetEquippedObject(false) == actor.GetEquippedObject(true);
        if (leftHand)
        {
            if (auto* high = actor.GetHighProcess())
            {
                high->isDualCasting = dual;
            }
            caster->SetDualCasting(dual);
        }
        // RequestCastImpl only sends BeginCast. The caster ignores the graph's cast
        // annotations while idle, and the graph leaves its casting state again
        // unless the want-cast variable is set, so both are set as a cast request
        // by the engine would.
        if (caster->state.get() == RE::MagicCaster::State::kNone)
        {
            caster->state = RE::MagicCaster::State::kUnk01;
        }
        actor.SetGraphVariableBool(wantCastVariable, true);
        caster->RequestCastImpl();

        cast.held[hand] = true;
        logs::debug(
            "Animation sync started spell cast: actor={:08X} hand={} spell={:08X} castingType={} delivery={} dual={}",
            a_remote.handle.native_handle(), handName, spell ? spell->GetFormID() : 0,
            spell ? static_cast<std::uint32_t>(spell->GetCastingType()) : 0,
            spell ? static_cast<std::uint32_t>(spell->GetDelivery()) : 0, dual);
    }

    void ApplyBlock(RemoteActor& a_remote, bool a_blocking, bool a_mayStart, float a_deltaTime)
    {
        if (!GameThread::Require("ApplyBlock"))
        {
            return;
        }
        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return;
        }

        static const RE::BSFixedString blockStartEvent{ "blockStart" };
        static const RE::BSFixedString blockStopEvent{ "blockStop" };
        static const RE::BSFixedString blockStopInstantEvent{ "blockStopInstant" };
        static const RE::BSFixedString isBlockingVariable{ "IsBlocking" };

        auto& actor = *actorPtr;
        auto& state = a_remote.animation;
        auto& block = state.hands.block;
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        CoolDown(block.eventCooldown, deltaTime);

        if (block.stopping)
        {
            block.stopElapsed += deltaTime;
            bool graphBlocking = false;
            const bool graphBlockingAvailable = actor.GetGraphVariableBool(isBlockingVariable, graphBlocking);
            const bool lowered = graphBlockingAvailable ? !graphBlocking : block.stopAccepted;
            const bool timedOut = block.stopElapsed >= kBlockStopFallbackSeconds;
            if (!lowered && !timedOut)
            {
                // A playing bash rejects blockStop, and its exit then leaves the block
                // by itself because iWantBlock is clear. Only a graph that stays in the
                // block state is asked again.
                if (!block.stopAccepted && !state.attack.pending && block.eventCooldown <= 0.0F)
                {
                    block.eventCooldown = kEventRetrySeconds;
                    block.stopAccepted = SendEvent(a_remote, actor, blockStopEvent);
                }
                return;
            }
            const bool forced = !lowered && SendEvent(a_remote, actor, blockStopInstantEvent);
            logs::debug("Animation sync lowered block: actor={:08X} lowered={} forcedInstant={} stopSeconds={:.3f}",
                a_remote.handle.native_handle(), lowered, forced, block.stopElapsed);
            block.stopping = false;
            block.stopAccepted = false;
            block.stopElapsed = 0.0F;
            state.dispatch.reassertReason = "block";
            return;
        }

        if (block.held)
        {
            if (a_blocking)
            {
                // Set on every pass, so a bash's bashExit returns to the block.
                SetBlockRequest(actor, true);
                return;
            }

            block.held = false;
            block.stopping = true;
            block.stopElapsed = 0.0F;
            block.eventCooldown = kEventRetrySeconds;
            SetBlockRequest(actor, false);
            block.stopAccepted = SendEvent(a_remote, actor, blockStopEvent);
            logs::debug("Animation sync requested block stop: actor={:08X} accepted={}",
                a_remote.handle.native_handle(), block.stopAccepted);
            return;
        }

        // A graph reset abandons a held block without lowering the actor's request.
        const auto* actorState = actor.AsActorState();
        if (actorState && actorState->actorState2.wantBlocking)
        {
            SetBlockRequest(actor, false);
        }
        if (!a_blocking || !a_mayStart || block.eventCooldown > 0.0F || IsBowOrCastPlaying(state.hands)
            || !CanStart(a_remote, actor))
        {
            return;
        }
        block.eventCooldown = kEventRetrySeconds;
        SetBlockRequest(actor, true);
        if (!SendEvent(a_remote, actor, blockStartEvent))
        {
            SetBlockRequest(actor, false);
            return;
        }

        block.held = true;
        logs::debug("Animation sync raised block: actor={:08X}", a_remote.handle.native_handle());
    }

    bool IsBowOrCastPlaying(const State& a_state)
    {
        return a_state.bow.drawHeld || a_state.bow.releasePending || a_state.cast.held[0] || a_state.cast.held[1]
            || a_state.cast.stopping[0] || a_state.cast.stopping[1];
    }

    bool IsBlockPlaying(const State& a_state)
    {
        return a_state.block.held || a_state.block.stopping;
    }

    bool IsPlaying(const RemoteActor& a_remote)
    {
        return IsBowOrCastPlaying(a_remote.animation.hands) || IsBlockPlaying(a_remote.animation.hands);
    }
}
