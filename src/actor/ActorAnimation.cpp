#include "actor/ActorAnimation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "actor/AnimationFirewall.h"
#include "actor/RemoteActor.h"
#include "runtime/GameThread.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace ActorAnimation
{
    namespace
    {
        using ActorConstants::kMaximumFrameTime;
        using Multiplayer::IsMoving;
        using Multiplayer::IsSneaking;
        using Multiplayer::IsSprinting;

        constexpr std::uint32_t kMaximumEventDispatchAttempts = 4;
        // Rejected graph events are also retried for at least this long, so a high
        // frame rate cannot spend every attempt inside one short graph blend.
        constexpr float kMinimumEventRetrySeconds = 0.1F;
        // Locomotion edges need a far longer budget than the presentation events
        // above. The graph refuses locomotion for as long as it is still leaving an
        // attack, and a power attack's exit was measured holding it past 0.1
        // seconds every time, so the old budget gave up exactly one frame early and
        // recorded an edge the graph never took.
        constexpr float kLocomotionEventRetrySeconds = 0.75F;
        // Skyrim cross-blends directional locomotion (CyclicFreeze, then
        // CyclicCrossBlend) instead of sweeping through intermediate directions.
        // The source's Direction ramps over several samples, so only a change of
        // at least 1/8 of a turn starts one, and never before the previous
        // 0.2-second cross-blend has finished. Smaller changes follow Direction.
        constexpr float kDirectionCrossBlendThreshold = 0.125F;
        constexpr float kDirectionCrossBlendIntervalSeconds = 0.25F;
        constexpr float kHitReactionObservationSeconds = 0.1F;
        // The idle turn state machines of the vanilla behaviors (MT_Idle_TurnBehavior,
        // 1HM_Idle_Behavior, Bow_Idle_Behavior, MagicReadyIdleTurnBehavior, ...) number
        // their states 0 turn right, 1 standing, and 2 turn left.
        constexpr std::int32_t kStandingTurnState = 1;
        constexpr std::uint32_t kInitialTurnStateOverrideLogs = 8;
        constexpr std::uint32_t kTurnStateOverrideLogInterval = 120;
        // A locomotion dispatch the graph keeps rejecting retries for much longer
        // than an actor may visibly stand still. Holding the transform for the whole
        // retry leaves the actor running on the spot and then applies the collected
        // backlog in one jump when the state finally settles, so the hold gives up
        // after this and follows the network path with the graph state reached so far.
        constexpr float kUnsettledTranslationHoldSeconds = 0.15F;
        constexpr std::uint32_t kGraphRecoveryStableFrames = 2;
        constexpr float kGraphRecoveryFallbackSeconds = 2.0F;
        constexpr std::uint32_t kGraphRecoveryWaitLogInterval = 120;
        constexpr float kLandingCompletionFallbackSeconds = 2.0F;
        // Measured in time, not frames: a one-handed sword draw takes about 1.4
        // seconds, which a frame budget cuts short at high frame rates.
        constexpr float kWeaponTransitionFallbackSeconds = 2.5F;
        constexpr float kAttackStartFallbackSeconds = 1.25F;
        constexpr float kAttackPlaybackFallbackSeconds = 4.0F;
        // A graph that keeps rejecting attackRelease this long is cancelled instead.
        constexpr float kBowReleaseFallbackSeconds = 1.5F;
        // The release clip's length. The next draw and locomotion replay wait for it.
        constexpr float kBowShotSeconds = 0.5F;
        // Upper bound for the shot while the attack state still reports a bow attack.
        constexpr float kBowShotFallbackSeconds = 2.0F;
        // A cast still playing this long after its interrupt is abandoned.
        constexpr float kCastStopFallbackSeconds = 1.0F;
        // A block still raised this long after blockStop is dropped instantly. It
        // covers a bash that was already playing when the block was lowered.
        constexpr float kBlockStopFallbackSeconds = 1.5F;
        // A cast enters its graph state some updates after the caster's request. A
        // graph without the casting variables takes moveStart after this long.
        constexpr float kCastLowerBodyEntrySeconds = 0.5F;
        // SwimIdle rejects moveStart until the graph has taken SwimStart. A moving
        // actor that is still rejected after this long stays in SwimIdle.
        constexpr float kSwimMoveStartFallbackSeconds = 0.5F;

        // A graph that rejects a locomotion edge is asked again on every pass until
        // its retry budget runs out, which is a whole second of identical lines at a
        // high frame rate. Report the first pass of such a burst and a sample of the
        // wait; the dispatch that ends it carries the attempt count. Counting passes
        // rather than seconds is intended: this only throttles the log.
        constexpr bool ShouldReportEventRetry(std::uint32_t a_attempt) noexcept
        {
            constexpr std::uint32_t kEventRetryReportInterval = 60;
            return a_attempt == 1 || a_attempt % kEventRetryReportInterval == 0;
        }

        static_assert(ShouldReportEventRetry(1));
        static_assert(!ShouldReportEventRetry(2));
        static_assert(ShouldReportEventRetry(60));

        // The native attack takes attackStart as its next combo swing in these
        // states; CombatSync captures the local player's chained swings from them.
        constexpr bool IsAttackChainWindow(RE::ATTACK_STATE_ENUM a_state) noexcept
        {
            return a_state == RE::ATTACK_STATE_ENUM::kNextAttack || a_state == RE::ATTACK_STATE_ENUM::kFollowThrough;
        }

        constexpr bool IsWeaponTransitionAnimating(RE::WEAPON_STATE a_weaponState, bool a_weaponDrawn) noexcept
        {
            return a_weaponDrawn ? a_weaponState == RE::WEAPON_STATE::kDrawing
                                 : a_weaponState == RE::WEAPON_STATE::kSheathing;
        }

        static_assert(IsWeaponTransitionAnimating(RE::WEAPON_STATE::kDrawing, true));
        static_assert(IsWeaponTransitionAnimating(RE::WEAPON_STATE::kSheathing, false));
        static_assert(!IsWeaponTransitionAnimating(RE::WEAPON_STATE::kWantToDraw, true));
        static_assert(!IsWeaponTransitionAnimating(RE::WEAPON_STATE::kWantToSheathe, false));
        constexpr bool ShouldReassertAfterWeaponTransition(
            bool a_pending, bool a_completed, bool a_moving, bool a_jumping) noexcept
        {
            return a_pending && a_completed && (a_moving || a_jumping);
        }

        static_assert(ShouldReassertAfterWeaponTransition(true, true, true, false));
        static_assert(ShouldReassertAfterWeaponTransition(true, true, false, true));
        static_assert(!ShouldReassertAfterWeaponTransition(true, true, false, false));
        static_assert(!ShouldReassertAfterWeaponTransition(true, false, true, false));
        constexpr Multiplayer::AnimationState GetWeaponReplayBaseState(Multiplayer::AnimationState a_state) noexcept
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

        static_assert(GetWeaponReplayBaseState(Multiplayer::AnimationState{
                          .movementState = Multiplayer::MovementState::kSprinting, .jumping = true })
            == Multiplayer::AnimationState{ .movementState = Multiplayer::MovementState::kRunning });

        bool RevertAnimationGraphManager(RE::Actor& a_actor)
        {
            // CommonLibSSE-NG does not currently expose this wrapper. Skyrim Together
            // uses the same engine function before authoritative action replay. Its
            // Address Library IDs sit immediately before the graph-variable accessors:
            // Revert=32140/32883, SetBool=32141/32885.
            using func_t = bool(RE::IAnimationGraphManagerHolder*);
            static REL::Relocation<func_t> func{ RELOCATION_ID(32140, 32883) };
            auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(std::addressof(a_actor));
            return func(holder);
        }

        bool IsActorInHitReaction(RE::Actor& a_actor)
        {
            static const RE::BSFixedString isRecoilingVariable{ "IsRecoiling" };
            bool isRecoiling = false;
            const bool recoilAvailable = a_actor.GetGraphVariableBool(isRecoilingVariable, isRecoiling);
            const auto* actorState = a_actor.AsActorState();
            return a_actor.IsStaggering() || (recoilAvailable && isRecoiling) || a_actor.IsInRagdollState()
                || (actorState && actorState->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal)
                || a_actor.GetActorRuntimeData().boolBits.all(RE::Actor::BOOL_BITS::kParalyzed);
        }

        void BeginAnimationGraphRecovery(
            RE::Actor& a_actor, RemoteActor& a_remote, std::uint32_t a_observedHitEvents, bool a_detectedHitReaction)
        {
            const auto animationState = a_remote.animation.get();
            if (!animationState)
            {
                return;
            }

            RE::BSAnimationGraphManager* graphBeforeRecovery = nullptr;
            {
                RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
                if (a_actor.GetAnimationGraphManager(graphManager) && graphManager)
                {
                    graphBeforeRecovery = graphManager.get();
                }
            }

            // TESHitEvent can be delivered while animation work is running elsewhere.
            // The event sink only increments a protected counter. This function is
            // reached from the authoritative main-loop hook, so the engine-owned
            // graph is reverted at a safe lifecycle boundary instead of being patched
            // from the event callback.
            AnimationFirewall::Disable(a_remote);
            animationState->localInterruptionLatched = true;
            animationState->graphRecoveryInProgress = true;
            animationState->graphBeforeRecovery = graphBeforeRecovery;
            animationState->recoveryWaitFrames = 0;
            animationState->recoveryWaitElapsed = 0.0F;
            animationState->recoveryStableFrames = 0;
            InvalidateAnimationDispatch(*animationState);
            a_remote.ready = false;
            a_remote.initializationWaitFrames = 0;

            // Hit processing can enter the behavior graph below the public event hook
            // and can leave variables, active transitions, and pose state inconsistent
            // with replicated locomotion. Revert the complete graph to its initial
            // state. If the engine cannot do that, rebuilding 3D is the heavier fallback
            // which also reconstructs the graph.
            auto& runtimeData = a_actor.GetActorRuntimeData();
            const auto* actorState = a_actor.AsActorState();
            const bool requiresPhysicalReset = a_actor.IsInRagdollState()
                || (actorState && actorState->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal)
                || runtimeData.boolBits.all(RE::Actor::BOOL_BITS::kParalyzed);
            runtimeData.boolBits.reset(RE::Actor::BOOL_BITS::kParalyzed);
            if (requiresPhysicalReset)
            {
                a_actor.PotentiallyFixRagdollState();
            }
            if (auto* root = a_actor.Get3D())
            {
                root->RemoveDecals();
            }
            const bool graphReverted
                = !requiresPhysicalReset && graphBeforeRecovery && RevertAnimationGraphManager(a_actor);
            animationState->graphRecoveryUsed3DReset = !graphReverted;
            if (!graphReverted)
            {
                a_actor.DoReset3D(false);
            }
            const auto recoveryCount = ++animationState->recoveryCount;
            logs::info(
                "Started actor animation-graph recovery: actor={:08X} desired={} detectedReaction={} observedHits={} "
                "graphReverted={} fallback3DReset={} previousGraph={} recovery={}",
                a_remote.handle.native_handle(),
                Multiplayer::GetMovementStateName(a_remote.animation->targetAnimationState.movementState),
                a_detectedHitReaction, a_observedHitEvents, graphReverted, !graphReverted,
                static_cast<const void*>(graphBeforeRecovery), recoveryCount);
        }

        void ApplyAuthoritativeWeaponPresentation(
            RE::Actor& a_actor, RemoteActor& a_remote, bool a_moving, bool a_moveStartAlreadyDispatched)
        {
            const auto* actorState = a_actor.AsActorState();
            if (!actorState)
            {
                return;
            }

            const auto weaponState = actorState->GetWeaponState();
            const bool authoritativeWeaponDrawn
                = ActorRegistry::GetAuthoritativeWeaponDrawnState(a_actor).value_or(false);
            const bool weaponTransitionPending = authoritativeWeaponDrawn ? weaponState != RE::WEAPON_STATE::kDrawn
                                                                          : weaponState != RE::WEAPON_STATE::kSheathed;
            const bool transitionWasAnimating = IsWeaponTransitionAnimating(weaponState, authoritativeWeaponDrawn);
            // Queue (and retry) the native transition before selecting its moving
            // branch. Draw samples that edge as it enters the equip graph; waiting for
            // ActorState to report kDrawing costs a graph update and leaves the standing
            // equip clip visible while network translation continues. WantToDraw and
            // WantToSheathe only record an outstanding request, so keep retrying until
            // the matching transition is actually animating.
            if (weaponTransitionPending && !transitionWasAnimating)
            {
                a_actor.DrawWeaponMagicHands(authoritativeWeaponDrawn);
            }
            if (weaponTransitionPending && a_moving && a_remote.animation
                && !a_remote.animation->weaponTransitionMoveStartDispatched)
            {
                static const RE::BSFixedString moveStartEvent{ "moveStart" };
                // Enter the equip graph through its normal standing state, then take its
                // explicit moveStart edge. Forcing iSyncIdleLocomotion before entry makes
                // the graph emit transition annotations while masking the visible pose.
                // A moveStart dispatched earlier in this update only counts if the
                // transition was already active; otherwise it preceded graph entry.
                const bool moveStartAccepted = (transitionWasAnimating && a_moveStartAlreadyDispatched)
                    || SendNetworkAnimationEvent(a_remote, moveStartEvent);
                if (moveStartAccepted)
                {
                    a_remote.animation->weaponTransitionMoveStartDispatched = true;
                    logs::info("Animation sync selected moving weapon-transition branch: actor={:08X} weaponDrawn={}",
                        a_remote.handle.native_handle(), authoritativeWeaponDrawn);
                }
            }
        }

        float GetGraphSpeedForState(RE::Actor& a_actor, float a_sampleSpeed, Multiplayer::MovementState a_state)
        {
            if (a_state == Multiplayer::MovementState::kIdle || a_state == Multiplayer::MovementState::kSneakingIdle)
            {
                return 0.0F;
            }

            // Speed and locomotion state must describe the same network sample. The
            // actor's base speed is only a fallback for a zero/invalid first sample.
            if (std::isfinite(a_sampleSpeed) && a_sampleSpeed > 0.0F)
            {
                return a_sampleSpeed;
            }

            float graphSpeed = 0.0F;
            switch (a_state)
            {
                case Multiplayer::MovementState::kWalking:
                case Multiplayer::MovementState::kSneakingWalking:
                    graphSpeed = a_actor.GetWalkSpeed();
                    break;
                case Multiplayer::MovementState::kRunning:
                case Multiplayer::MovementState::kSprinting:
                case Multiplayer::MovementState::kSneakingRunning:
                    graphSpeed = a_actor.GetRunSpeed();
                    break;
                case Multiplayer::MovementState::kIdle:
                case Multiplayer::MovementState::kSneakingIdle:
                    break;
            }

            return std::isfinite(graphSpeed) && graphSpeed > 0.0F ? graphSpeed : 1.0F;
        }

        // The draw/sheathe clip itself is still running. The reassert flag alone
        // stays armed long after the clip for a later locomotion replay.
        bool IsWeaponTransitionClipPending(const State& a_state)
        {
            return a_state.weaponLocomotionReassertPending && !a_state.weaponTransitionCompletionObserved
                && a_state.weaponTransitionWaitElapsed < kWeaponTransitionFallbackSeconds;
        }

        bool IsWeaponTransitionUnsettled(const State& a_state)
        {
            if (!a_state.weaponLocomotionReassertPending)
            {
                return false;
            }
            if (IsWeaponTransitionClipPending(a_state))
            {
                return true;
            }
            // The reassert flag stays armed across an idle interval so that later
            // movement replays locomotion after the weapon clip. An idle target has
            // nothing left to replay once the clip is complete and, for a sheathe, the
            // default state has been forced.
            const bool moving = IsMoving(a_state.desired.load(std::memory_order_acquire));
            const bool jumping = a_state.desiredJumping.load(std::memory_order_acquire);
            const bool weaponDrawn = a_state.desiredWeaponDrawn.load(std::memory_order_acquire);
            return moving || jumping || (!weaponDrawn && !a_state.weaponDefaultStateDispatched);
        }

        constexpr bool IsBowAttackState(RE::ATTACK_STATE_ENUM a_state) noexcept
        {
            return a_state >= RE::ATTACK_STATE_ENUM::kBowDraw && a_state <= RE::ATTACK_STATE_ENUM::kBowFollowThrough;
        }

        static_assert(IsBowAttackState(RE::ATTACK_STATE_ENUM::kBowDrawn));
        static_assert(!IsBowAttackState(RE::ATTACK_STATE_ENUM::kFollowThrough));

        void ResetHandPresentation(State& a_state)
        {
            a_state.bowDrawHeld = false;
            a_state.bowReleasePending = false;
            a_state.bowPhaseElapsed = 0.0F;
            a_state.bowReleaseRequestElapsed = 0.0F;
            a_state.bowEventRetryElapsed = kMinimumEventRetrySeconds;
            a_state.castHeld = {};
            a_state.castStopping = {};
            a_state.castStopRequestElapsed = {};
            a_state.castEventRetryElapsed.fill(kMinimumEventRetrySeconds);
            a_state.blockHeld = false;
            a_state.blockStopping = false;
            a_state.blockStopAccepted = false;
            a_state.blockStopElapsed = 0.0F;
            a_state.blockEventRetryElapsed = kMinimumEventRetrySeconds;
            a_state.presentationMoveStartPending = false;
            a_state.presentationMoveStartWaitPasses = 0;
            a_state.presentationMoveStartElapsed = 0.0F;
        }

        void RequestPresentationMoveStart(State& a_state)
        {
            a_state.presentationMoveStartPending = true;
            a_state.presentationMoveStartWaitPasses = 0;
            a_state.presentationMoveStartElapsed = 0.0F;
        }

        bool IsBowOrCastPlaying(const State& a_state)
        {
            return a_state.bowDrawHeld || a_state.bowReleasePending || a_state.castHeld[0] || a_state.castHeld[1]
                || a_state.castStopping[0] || a_state.castStopping[1];
        }

        bool IsBlockPlaying(const State& a_state)
        {
            return a_state.blockHeld || a_state.blockStopping;
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
            // Indexed by Network::PowerAttackDirection.
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

        // A bow draw or cast may only enter a stable, grounded, fully drawn graph
        // that no attack, weapon transition, shot, or recovery owns.
        bool CanStartHandPresentation(const RemoteActor& a_remote, RE::Actor& a_actor)
        {
            const auto& state = *a_remote.animation;
            if (!a_remote.ready || !a_remote.animationGraphHolder || !IsAnimationStateSettled(a_remote)
                || state.attackPlaybackPending || state.attackLocomotionReassertPending || state.bowReleasePending
                || IsWeaponTransitionUnsettled(state) || state.graphRecoveryInProgress
                || state.targetAnimationState.jumping || !state.targetAnimationState.weaponDrawn)
            {
                return false;
            }

            auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(std::addressof(a_actor));
            const auto* actorState = a_actor.AsActorState();
            if (holder != a_remote.animationGraphHolder || !actorState
                || actorState->GetWeaponState() != RE::WEAPON_STATE::kDrawn)
            {
                return false;
            }

            RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
            return a_actor.Is3DLoaded() && a_actor.GetAnimationGraphManager(graphManager) && graphManager
                && state.graphManager == graphManager.get();
        }

        bool SendHandPresentationEvent(RemoteActor& a_remote, RE::Actor& a_actor, const RE::BSFixedString& a_eventName)
        {
            // The same synchronous dispatch window as DispatchAttack: the
            // persistent suppression flags can make the graph reject a replicated
            // bow draw. They are restored before any AI update can run.
            auto& actorFlags = a_actor.GetActorRuntimeData().boolFlags;
            actorFlags.reset(RE::Actor::BOOL_FLAGS::kAttackingDisabled, RE::Actor::BOOL_FLAGS::kCastingDisabled);
            const bool accepted = SendNetworkAnimationEvent(a_remote, a_eventName);
            actorFlags.set(RE::Actor::BOOL_FLAGS::kAttackingDisabled, RE::Actor::BOOL_FLAGS::kCastingDisabled);
            return accepted;
        }

        // Moves a remote actor that entered the water while moving from SwimIdle into
        // SwimLocomotion. A swim started while standing needs nothing: the network
        // idle-to-moving dispatch sends its own moveStart into SwimIdle.
        void AdvanceSwimMoveStart(
            RemoteActor& a_remote, State& a_state, const RE::ActorState* a_actorState, float a_deltaTime)
        {
            if (!a_state.swimMoveStartPending)
            {
                return;
            }
            const bool movingDispatched = a_state.hasDispatched && IsMoving(a_state.lastDispatched.movementState)
                && IsMoving(a_state.targetAnimationState.movementState);
            if (!movingDispatched || !a_actorState || !a_actorState->IsSwimming())
            {
                a_state.swimMoveStartPending = false;
                return;
            }

            a_state.swimMoveStartElapsed += std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
            // SwimStart is evaluated on the graph's next update. Counting passes
            // rather than time is intended: the swim branch must have been entered.
            if (++a_state.swimMoveStartWaitPasses < 2)
            {
                return;
            }

            static const RE::BSFixedString moveStartEvent{ "moveStart" };
            ++a_state.swimMoveStartAttempts;
            const bool accepted = SendNetworkAnimationEvent(a_remote, moveStartEvent);
            if (!accepted && a_state.swimMoveStartElapsed < kSwimMoveStartFallbackSeconds)
            {
                return;
            }
            a_state.swimMoveStartPending = false;
            logs::info(
                "Animation sync selected moving swim state: actor={:08X} state={} accepted={} attempts={} "
                "elapsed={:.3f}",
                a_remote.handle.native_handle(),
                Multiplayer::GetMovementStateName(a_state.targetAnimationState.movementState), accepted,
                a_state.swimMoveStartAttempts, a_state.swimMoveStartElapsed);
        }
    }

    Multiplayer::AnimationState GetTargetState(const RemoteActor& a_remote)
    {
        return a_remote.animation->targetAnimationState;
    }

    void SetAnimationState(RemoteActor& a_remote, Multiplayer::AnimationState a_animationState)
    {
        const auto previousAnimationState = a_remote.animation->targetAnimationState;
        a_remote.animation->targetAnimationState = a_animationState;
        ActorRegistry::SetAuthoritativeJumpState(a_remote, a_animationState.jumping);
        ActorRegistry::SetAuthoritativeDirection(a_remote);
        ActorRegistry::SetAuthoritativeWeaponDrawnState(a_remote, a_animationState.weaponDrawn);
        RequestAnimationUpdate(a_remote.animation.get(), a_animationState, a_remote.movement.animationSpeed,
            a_remote.movement.animationDirection);
        if (previousAnimationState != a_animationState && a_remote.animation)
        {
            logs::info("Animation sync updated actor request: actor={:08X} revision={} previous={}/{} desired={}/{}",
                a_remote.handle.native_handle(),
                static_cast<unsigned long long>(a_remote.animation->desiredRevision.load(std::memory_order_acquire)),
                Multiplayer::GetMovementStateName(previousAnimationState.movementState),
                previousAnimationState.jumping ? "jumping" : "grounded",
                Multiplayer::GetMovementStateName(a_animationState.movementState),
                a_animationState.jumping ? "jumping" : "grounded");
        }
    }

    bool SendNetworkAnimationEvent(RemoteActor& a_remote, const RE::BSFixedString& a_eventName)
    {
        if (!GameThread::Require("SendNetworkAnimationEvent"))
        {
            return false;
        }

        auto actorPtr = a_remote.handle.get();
        if (!actorPtr || !a_remote.animationGraphHolder)
        {
            return false;
        }

        auto* actor = actorPtr.get();
        auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(actor);
        if (holder != a_remote.animationGraphHolder)
        {
            return false;
        }

        return AnimationFirewall::NotifyNetworkAnimationGraph(*actor, a_eventName);
    }

    bool IsAnimationStateSettled(const RemoteActor& a_remote)
    {
        if (!a_remote.animation)
        {
            return true;
        }

        return !a_remote.animation->landingCompletionPending && IsAnimationStateSettledExceptLanding(a_remote);
    }

    bool IsAnimationStateSettledExceptLanding(const RemoteActor& a_remote)
    {
        if (!a_remote.animation)
        {
            return true;
        }

        return a_remote.animation->processedRevision.load(std::memory_order_acquire)
            == a_remote.animation->desiredRevision.load(std::memory_order_acquire);
    }

    bool ShouldHoldTranslation(RemoteActor& a_remote, float a_deltaTime)
    {
        if (!a_remote.animation)
        {
            return false;
        }

        auto& state = *a_remote.animation;
        if (IsAnimationStateSettledExceptLanding(a_remote))
        {
            state.unsettledTranslationHoldElapsed = 0.0F;
            state.unsettledTranslationHoldReported = false;
            return false;
        }

        state.unsettledTranslationHoldElapsed += std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        if (state.unsettledTranslationHoldElapsed < kUnsettledTranslationHoldSeconds)
        {
            return true;
        }
        if (!state.unsettledTranslationHoldReported)
        {
            state.unsettledTranslationHoldReported = true;
            logs::info(
                "Animation sync resumed translation while its locomotion dispatch is unsettled: actor={:08X} "
                "dispatched={} heldSeconds={:.3f} rejectedEventAttempts={}",
                a_remote.handle.native_handle(), Multiplayer::GetMovementStateName(state.lastDispatched.movementState),
                state.unsettledTranslationHoldElapsed, state.rejectedEventAttempts);
        }
        return false;
    }

    bool CanDispatchAttack(const RemoteActor& a_remote, const Network::Attack& a_attack)
    {
        if (!GameThread::Require("CanDispatchAttack") || !a_remote.ready || !a_remote.animation
            || !a_remote.animationGraphHolder || !IsAnimationStateSettled(a_remote)
            || a_remote.animation->attackPlaybackPending || a_remote.animation->attackLocomotionReassertPending
            || IsBowOrCastPlaying(*a_remote.animation) ||
            // The source bashes from a raised block, so only a bash plays from it.
            // Other attacks wait until the block is lowered.
            (a_attack.action != Network::AttackAction::kBash && IsBlockPlaying(*a_remote.animation)) ||
            // The reassert flag stays armed while idle after a completed draw, so
            // only a transition that is still unsettled may hold the attack.
            IsWeaponTransitionUnsettled(*a_remote.animation) || a_remote.animation->graphRecoveryInProgress
            || a_remote.animation->targetAnimationState.jumping
            || !a_remote.animation->targetAnimationState.weaponDrawn)
        {
            return false;
        }

        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return false;
        }
        auto* actor = actorPtr.get();
        auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(actor);
        const auto* actorState = actor->AsActorState();
        if (holder != a_remote.animationGraphHolder || !actorState
            || actorState->GetWeaponState() != RE::WEAPON_STATE::kDrawn
            || actorState->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone)
        {
            return false;
        }

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
        return actor->Is3DLoaded() && actor->GetAnimationGraphManager(graphManager) && graphManager
            && a_remote.animation->graphManager == graphManager.get();
    }

    bool CanChainAttack(const RemoteActor& a_remote, const Network::Attack& a_attack)
    {
        if (!GameThread::Require("CanChainAttack") || !a_remote.ready || a_attack.action == Network::AttackAction::kBash
            || !a_remote.animation || !a_remote.animationGraphHolder || !IsAnimationStateSettled(a_remote)
            || !a_remote.animation->attackPlaybackPending || !a_remote.animation->attackPlaybackObserved
            || a_remote.animation->attackChainDispatched || a_remote.animation->graphRecoveryInProgress
            || a_remote.animation->targetAnimationState.jumping
            || !a_remote.animation->targetAnimationState.weaponDrawn)
        {
            return false;
        }

        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return false;
        }
        auto* actor = actorPtr.get();
        auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(actor);
        const auto* actorState = actor->AsActorState();
        if (holder != a_remote.animationGraphHolder || !actorState
            || actorState->GetWeaponState() != RE::WEAPON_STATE::kDrawn
            || !IsAttackChainWindow(actorState->GetAttackState()))
        {
            return false;
        }

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
        return actor->Is3DLoaded() && actor->GetAnimationGraphManager(graphManager) && graphManager
            && a_remote.animation->graphManager == graphManager.get();
    }

    bool CanPowerUpAttack(const RemoteActor& a_remote, const Network::Attack& a_attack)
    {
        if (!GameThread::Require("CanPowerUpAttack") || !a_remote.ready || !a_attack.power
            || a_attack.action == Network::AttackAction::kBash || !a_remote.animation || !a_remote.animationGraphHolder
            || !IsAnimationStateSettled(a_remote)
            || !a_remote.animation->attackPlaybackPending
            // The actor's attack state is not waited for: a remote actor reports it
            // well into the swing, which showed most of the plain swing before the
            // power attack. A graph not yet in its attack state rejects the event
            // and the action is retried.
            || a_remote.animation->attackPlaybackBash || a_remote.animation->attackPlaybackPower
            || a_remote.animation->attackPlaybackAction != a_attack.action
            || a_remote.animation->graphRecoveryInProgress || a_remote.animation->targetAnimationState.jumping
            || !a_remote.animation->targetAnimationState.weaponDrawn)
        {
            return false;
        }

        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return false;
        }
        auto* actor = actorPtr.get();
        auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(actor);
        const auto* actorState = actor->AsActorState();
        if (holder != a_remote.animationGraphHolder || !actorState
            || actorState->GetWeaponState() != RE::WEAPON_STATE::kDrawn)
        {
            return false;
        }

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
        return actor->Is3DLoaded() && actor->GetAnimationGraphManager(graphManager) && graphManager
            && a_remote.animation->graphManager == graphManager.get();
    }

    bool DispatchAttack(RemoteActor& a_remote, const Network::Attack& a_attack)
    {
        const bool poweringUp = CanPowerUpAttack(a_remote, a_attack);
        const bool chaining = !poweringUp && CanChainAttack(a_remote, a_attack);
        if (!poweringUp && !chaining && !CanDispatchAttack(a_remote, a_attack))
        {
            return false;
        }

        static const RE::BSFixedString attackStartEvent{ "attackStart" };
        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return false;
        }
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
        bool accepted = SendNetworkAnimationEvent(a_remote, attackEvent);
        // A graph without the power attack of these hands or this direction, for
        // example from a mod, shows the plain attack of the same hand instead. A
        // rejected chain only means the combo window does not take it, so the
        // power attack waits for the playing attack to finish. A playing plain
        // attack that rejects its power attack already shows the plain one.
        const bool powerFellBack
            = !accepted && a_attack.power && startsNewAttack && SendNetworkAnimationEvent(a_remote, plainEvent);
        accepted = accepted || powerFellBack;
        // The dual-wield branch requires two one-handed weapons in the actor's hand
        // types. When a mod or an unsettled hand disagrees, a right-hand swing is
        // the vanilla presentation of the same input.
        const bool dualFellBack = !accepted && a_attack.action == Network::AttackAction::kDual
            && (!a_attack.power || startsNewAttack) && SendNetworkAnimationEvent(a_remote, attackStartEvent);
        accepted = accepted || dualFellBack;
        actorFlags.set(RE::Actor::BOOL_FLAGS::kAttackingDisabled);
        if (!accepted)
        {
            return false;
        }
        if (a_attack.power)
        {
            // Stamina is not managed on the actor. Logged to check that an exhausted
            // actor still plays power attacks.
            const auto* actorValueOwner = actorPtr->AsActorValueOwner();
            logs::info(
                "Animation sync dispatched power attack: actor={:08X} action={} direction={} event={} "
                "presentedAsPlain={} presentedAsRightHand={} poweredUpPlayingAttack={} chained={} stamina={:.1f}",
                a_remote.handle.native_handle(), Network::GetAttackActionName(a_attack.action),
                Network::GetPowerAttackDirectionName(a_attack.direction), attackEvent.c_str(), powerFellBack,
                dualFellBack, poweringUp, chaining,
                actorValueOwner ? actorValueOwner->GetActorValue(RE::ActorValue::kStamina) : 0.0F);
        }
        else if (dualFellBack)
        {
            logs::info("Animation sync presented dual-wield attack as a right-hand attack: actor={:08X}",
                a_remote.handle.native_handle());
        }

        a_remote.animation->attackPlaybackElapsed = 0.0F;
        // Only an attack that did not fall back to the plain swing is a power attack.
        a_remote.animation->attackPlaybackPower = a_attack.power && !powerFellBack;
        if (poweringUp)
        {
            // The power attack plays on in the attack state. Its own attack state is
            // observed again, so the transition cannot end ownership early.
            a_remote.animation->attackPlaybackObserved = false;
            return true;
        }
        a_remote.animation->attackPlaybackAction = a_attack.action;
        if (chaining)
        {
            // The graph leaves the combo window on a later update. Until then the
            // actor state still reports it and must not consume another attack.
            a_remote.animation->attackChainDispatched = true;
            return true;
        }
        a_remote.animation->attackPlaybackPending = true;
        a_remote.animation->attackPlaybackBash = a_attack.action == Network::AttackAction::kBash;
        a_remote.animation->attackPlaybackObserved = false;
        a_remote.animation->attackChainDispatched = false;
        a_remote.animation->attackLocomotionReassertPending = false;
        const auto* actorState = actorPtr->AsActorState();
        a_remote.animation->attackPlaybackObserved
            = actorState && actorState->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
        return true;
    }

    bool IsAnimationActionPlaying(const RemoteActor& a_remote)
    {
        return a_remote.animation && a_remote.animation->attackPlaybackPending;
    }

    bool IsSwimming(const RemoteActor& a_remote)
    {
        if (!GameThread::Require("IsSwimming"))
        {
            return false;
        }
        const auto actorPtr = a_remote.handle.get();
        const auto* actorState = actorPtr ? actorPtr->AsActorState() : nullptr;
        return actorState && actorState->IsSwimming();
    }

    void ApplyBowDraw(RemoteActor& a_remote, bool a_drawn, bool a_mayStart, float a_deltaTime)
    {
        if (!GameThread::Require("ApplyBowDraw") || !a_remote.animation)
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
        auto& state = *a_remote.animation;
        const float deltaTime = std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        state.bowEventRetryElapsed = (std::min)(state.bowEventRetryElapsed + deltaTime, kMinimumEventRetrySeconds);

        if (state.bowReleasePending)
        {
            state.bowPhaseElapsed += deltaTime;
            // A remote actor's attack state does not follow its graph, so the shot is
            // given a fixed time. A state that does report a bow attack extends it.
            const auto* actorState = actor.AsActorState();
            const bool attackStateBusy = actorState && IsBowAttackState(actorState->GetAttackState());
            const bool completed = state.bowPhaseElapsed >= kBowShotSeconds && !attackStateBusy;
            const bool timedOut = state.bowPhaseElapsed >= kBowShotFallbackSeconds;
            if (!completed && !timedOut)
            {
                return;
            }
            logs::info("Animation sync completed bow shot ownership: actor={:08X} elapsed={:.3f} timedOut={}",
                a_remote.handle.native_handle(), state.bowPhaseElapsed, timedOut);
            state.bowReleasePending = false;
            state.bowPhaseElapsed = 0.0F;
            // As after a normal attack, replay locomotion without touching the stance.
            state.attackLocomotionReassertPending = true;
            return;
        }

        if (state.bowDrawHeld)
        {
            state.bowPhaseElapsed += deltaTime;
            if (a_drawn)
            {
                return;
            }

            state.bowReleaseRequestElapsed += deltaTime;
            if (state.bowEventRetryElapsed < kMinimumEventRetrySeconds)
            {
                return;
            }
            state.bowEventRetryElapsed = 0.0F;
            // An early release is rejected until the draw can take it, as the local
            // player's early release still waits for the arrow to be drawn. A graph
            // that keeps rejecting it is cancelled instead, so the draw never stays
            // held without its source.
            const bool released = SendHandPresentationEvent(a_remote, actor, attackReleaseEvent);
            const bool releaseTimedOut = state.bowReleaseRequestElapsed >= kBowReleaseFallbackSeconds;
            if (!released && !releaseTimedOut)
            {
                return;
            }
            const bool cancelled = !released
                && (SendHandPresentationEvent(a_remote, actor, attackStopEvent)
                    || SendHandPresentationEvent(a_remote, actor, bowResetEvent));
            logs::info(
                "Animation sync released bow draw: actor={:08X} released={} cancelled={} heldSeconds={:.3f} "
                "releaseWaitSeconds={:.3f}",
                a_remote.handle.native_handle(), released, cancelled, state.bowPhaseElapsed,
                state.bowReleaseRequestElapsed);
            state.bowDrawHeld = false;
            state.bowReleasePending = true;
            state.bowPhaseElapsed = 0.0F;
            return;
        }

        if (!a_drawn || !a_mayStart || state.bowEventRetryElapsed < kMinimumEventRetrySeconds || IsBlockPlaying(state)
            || !IsHoldingBow(actor) || !CanStartHandPresentation(a_remote, actor))
        {
            return;
        }
        state.bowEventRetryElapsed = 0.0F;
        if (!SendHandPresentationEvent(a_remote, actor, bowAttackStartEvent))
        {
            return;
        }

        state.bowDrawHeld = true;
        RequestPresentationMoveStart(state);
        state.bowPhaseElapsed = 0.0F;
        state.bowReleaseRequestElapsed = 0.0F;
        // A release that is already due goes out on the next pass.
        state.bowEventRetryElapsed = kMinimumEventRetrySeconds;
        logs::info("Animation sync started bow draw: actor={:08X}", a_remote.handle.native_handle());
    }

    void ApplySpellCast(RemoteActor& a_remote, bool a_leftHand, bool a_casting, bool a_mayStart, float a_deltaTime)
    {
        if (!GameThread::Require("ApplySpellCast") || !a_remote.animation)
        {
            return;
        }
        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return;
        }

        auto& actor = *actorPtr;
        auto& state = *a_remote.animation;
        const std::size_t hand = a_leftHand ? 1 : 0;
        const float deltaTime = std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        static const RE::BSFixedString wantCastLeftVariable{ "bWantCastLeft" };
        static const RE::BSFixedString wantCastRightVariable{ "bWantCastRight" };
        const auto& wantCastVariable = a_leftHand ? wantCastLeftVariable : wantCastRightVariable;
        static const RE::BSFixedString castingLeftVariable{ "IsCastingLeft" };
        static const RE::BSFixedString castingRightVariable{ "IsCastingRight" };
        const auto& castingVariable = a_leftHand ? castingLeftVariable : castingRightVariable;
        const auto setCastOwnership = [&a_remote, &state](bool a_owned) {
            if (state.castAnimationOwned != a_owned)
            {
                state.castAnimationOwned = a_owned;
                ActorRegistry::SetCastAnimationOwned(a_remote, a_owned);
            }
        };
        const auto releaseCastOwnership = [&state, &setCastOwnership]() {
            setCastOwnership(state.castHeld[0] || state.castHeld[1] || state.castStopping[0] || state.castStopping[1]);
        };
        state.castEventRetryElapsed[hand]
            = (std::min)(state.castEventRetryElapsed[hand] + deltaTime, kMinimumEventRetrySeconds);

        // The caster, not raw graph events, drives the cast: the magic graph
        // interrupts a cast its caster did not request.
        auto* caster = actor.GetMagicCaster(
            a_leftHand ? RE::MagicSystem::CastingSource::kLeftHand : RE::MagicSystem::CastingSource::kRightHand);
        const auto casterState = caster ? caster->state.get() : RE::MagicCaster::State::kNone;

        if (state.castStopping[hand])
        {
            state.castStopRequestElapsed[hand] += deltaTime;
            // The caster returns to idle as soon as the cast is interrupted, but the
            // graph leaves its casting state later. A new cast waits for both, so it
            // does not enter a concentration loop that is still playing.
            bool graphCasting = false;
            actor.GetGraphVariableBool(castingVariable, graphCasting);
            const bool idle = casterState == RE::MagicCaster::State::kNone && !graphCasting;
            const bool timedOut = state.castStopRequestElapsed[hand] >= kCastStopFallbackSeconds;
            if (!idle && !timedOut)
            {
                return;
            }
            const float stopSeconds = state.castStopRequestElapsed[hand];
            state.castStopping[hand] = false;
            state.castStopRequestElapsed[hand] = 0.0F;
            releaseCastOwnership();
            // The cast's lower body exits into its standing state, so a moving actor
            // would keep translating under an idle pose and slide, and its recorded
            // moving state would reject the next moveStop. As after an attack, replay
            // locomotion without touching the stance, once the last hand presentation
            // has left the graph.
            const bool presentationEnded = !IsBowOrCastPlaying(state) && !IsBlockPlaying(state);
            if (presentationEnded)
            {
                state.attackLocomotionReassertPending = true;
            }
            logs::info(
                "Animation sync stopped spell cast: actor={:08X} hand={} idle={} stopSeconds={:.3f} "
                "reassertedLocomotion={}",
                a_remote.handle.native_handle(), a_leftHand ? "left" : "right", idle, stopSeconds, presentationEnded);
            return;
        }

        if (state.castHeld[hand])
        {
            // An equipment swap that takes the spell away also ends the cast.
            if (a_casting && IsHoldingSpell(actor, a_leftHand))
            {
                // The magic graph stays in its casting state only while this
                // variable is set, and emits CastStop as soon as it clears. The
                // caster's own release action selects the cast clip.
                actor.SetGraphVariableBool(wantCastVariable, true);
                return;
            }
            state.castHeld[hand] = false;
            state.castStopping[hand] = true;
            state.castStopRequestElapsed[hand] = 0.0F;
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
        if (!a_casting || !a_mayStart || !caster || state.castEventRetryElapsed[hand] < kMinimumEventRetrySeconds
            || state.bowDrawHeld || IsBlockPlaying(state) || !IsHoldingSpell(actor, a_leftHand)
            || !CanStartHandPresentation(a_remote, actor))
        {
            return;
        }
        state.castEventRetryElapsed[hand] = 0.0F;

        auto* spell = actor.GetEquippedObject(a_leftHand)->As<RE::MagicItem>();
        // The caster sends its graph events synchronously and on later updates, and
        // checks the casting-disabled flag, so both are opened before the request.
        setCastOwnership(true);
        actor.GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kCastingDisabled);
        if (caster->currentSpell != spell)
        {
            caster->SetCurrentSpell(spell);
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

        state.castHeld[hand] = true;
        RequestPresentationMoveStart(state);
        state.castStopRequestElapsed[hand] = 0.0F;
        logs::info("Animation sync started spell cast: actor={:08X} hand={} spell={:08X}",
            a_remote.handle.native_handle(), a_leftHand ? "left" : "right", spell ? spell->GetFormID() : 0);
    }

    void ApplyBlock(RemoteActor& a_remote, bool a_blocking, bool a_mayStart, float a_deltaTime)
    {
        if (!GameThread::Require("ApplyBlock") || !a_remote.animation)
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
        auto& state = *a_remote.animation;
        const float deltaTime = std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        state.blockEventRetryElapsed = (std::min)(state.blockEventRetryElapsed + deltaTime, kMinimumEventRetrySeconds);

        if (state.blockStopping)
        {
            state.blockStopElapsed += deltaTime;
            bool graphBlocking = false;
            const bool graphBlockingAvailable = actor.GetGraphVariableBool(isBlockingVariable, graphBlocking);
            // A graph without the variable only has the accepted event as evidence.
            const bool lowered = graphBlockingAvailable ? !graphBlocking : state.blockStopAccepted;
            const bool timedOut = state.blockStopElapsed >= kBlockStopFallbackSeconds;
            if (!lowered && !timedOut)
            {
                // A playing bash rejects blockStop, and its exit then leaves the block
                // by itself because iWantBlock is clear. Only a graph that stays in the
                // block state is asked again.
                if (!state.blockStopAccepted && !state.attackPlaybackPending
                    && state.blockEventRetryElapsed >= kMinimumEventRetrySeconds)
                {
                    state.blockEventRetryElapsed = 0.0F;
                    state.blockStopAccepted = SendHandPresentationEvent(a_remote, actor, blockStopEvent);
                }
                return;
            }
            const bool forced = !lowered && SendHandPresentationEvent(a_remote, actor, blockStopInstantEvent);
            logs::info("Animation sync lowered block: actor={:08X} lowered={} forcedInstant={} stopSeconds={:.3f}",
                a_remote.handle.native_handle(), lowered, forced, state.blockStopElapsed);
            state.blockStopping = false;
            state.blockStopAccepted = false;
            state.blockStopElapsed = 0.0F;
            // As after an attack, replay locomotion without touching the stance.
            state.attackLocomotionReassertPending = true;
            return;
        }

        if (state.blockHeld)
        {
            if (a_blocking)
            {
                // Set on every pass, so a bash's bashExit returns to the block.
                SetBlockRequest(actor, true);
                return;
            }

            state.blockHeld = false;
            state.blockStopping = true;
            state.blockStopElapsed = 0.0F;
            state.blockEventRetryElapsed = 0.0F;
            SetBlockRequest(actor, false);
            state.blockStopAccepted = SendHandPresentationEvent(a_remote, actor, blockStopEvent);
            logs::info("Animation sync requested block stop: actor={:08X} accepted={}", a_remote.handle.native_handle(),
                state.blockStopAccepted);
            return;
        }

        // A graph reset abandons a held block without lowering the actor's request.
        const auto* actorState = actor.AsActorState();
        if (actorState && actorState->actorState2.wantBlocking)
        {
            SetBlockRequest(actor, false);
        }
        if (!a_blocking || !a_mayStart || state.blockEventRetryElapsed < kMinimumEventRetrySeconds
            || IsBowOrCastPlaying(state) || !CanStartHandPresentation(a_remote, actor))
        {
            return;
        }
        state.blockEventRetryElapsed = 0.0F;
        SetBlockRequest(actor, true);
        if (!SendHandPresentationEvent(a_remote, actor, blockStartEvent))
        {
            SetBlockRequest(actor, false);
            return;
        }

        state.blockHeld = true;
        RequestPresentationMoveStart(state);
        logs::info("Animation sync raised block: actor={:08X}", a_remote.handle.native_handle());
    }

    void AdvancePresentationLocomotion(RemoteActor& a_remote, float a_deltaTime)
    {
        if (!GameThread::Require("AdvancePresentationLocomotion") || !a_remote.animation
            || !a_remote.animation->presentationMoveStartPending)
        {
            return;
        }
        auto& state = *a_remote.animation;
        const bool castHeld = state.castHeld[0] || state.castHeld[1];
        const bool presentationHeld = state.blockHeld || state.bowDrawHeld || castHeld;
        auto actorPtr = a_remote.handle.get();
        if (!actorPtr || !presentationHeld || !IsMoving(state.targetAnimationState.movementState))
        {
            // A standing start needs nothing, and movement that starts later sends
            // its own moveStart into the presentation's lower body.
            state.presentationMoveStartPending = false;
            return;
        }
        if (state.attackPlaybackPending)
        {
            // A bash owns the graph; the locomotion replay after it sends moveStart.
            return;
        }

        state.presentationMoveStartElapsed += std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        // The start event is evaluated on the graph's next update. Counting passes
        // rather than time is intended: the branch must have been entered once.
        if (++state.presentationMoveStartWaitPasses < 2)
        {
            return;
        }
        if (castHeld && !state.blockHeld && !state.bowDrawHeld
            && state.presentationMoveStartElapsed < kCastLowerBodyEntrySeconds)
        {
            static const RE::BSFixedString castingLeftVariable{ "IsCastingLeft" };
            static const RE::BSFixedString castingRightVariable{ "IsCastingRight" };
            bool castingLeft = false;
            bool castingRight = false;
            (void)actorPtr->GetGraphVariableBool(castingLeftVariable, castingLeft);
            (void)actorPtr->GetGraphVariableBool(castingRightVariable, castingRight);
            if (!castingLeft && !castingRight)
            {
                return;
            }
        }

        // Sent once: the lower body takes it on the graph's first update in the
        // branch, and a rejection means it is not standing, so a retry never helps.
        static const RE::BSFixedString moveStartEvent{ "moveStart" };
        const bool accepted = SendNetworkAnimationEvent(a_remote, moveStartEvent);
        logs::info(
            "Animation sync selected moving presentation lower body: actor={:08X} state={} block={} bow={} cast={} "
            "accepted={} elapsed={:.3f}",
            a_remote.handle.native_handle(),
            Multiplayer::GetMovementStateName(state.targetAnimationState.movementState), state.blockHeld,
            state.bowDrawHeld, castHeld, accepted, state.presentationMoveStartElapsed);
        state.presentationMoveStartPending = false;
    }

    bool IsHandPresentationPlaying(const RemoteActor& a_remote)
    {
        return a_remote.animation && (IsBowOrCastPlaying(*a_remote.animation) || IsBlockPlaying(*a_remote.animation));
    }

    bool IsWeaponTransitionPlaying(const RemoteActor& a_remote)
    {
        return a_remote.animation && IsWeaponTransitionClipPending(*a_remote.animation);
    }

    void RequestAnimationUpdate(
        State* a_animationState, Multiplayer::AnimationState a_state, float a_speed, float a_direction)
    {
        if (!a_animationState)
        {
            return;
        }

        a_animationState->desired.store(a_state.movementState, std::memory_order_relaxed);
        a_animationState->desiredJumping.store(a_state.jumping, std::memory_order_relaxed);
        a_animationState->desiredWeaponDrawn.store(a_state.weaponDrawn, std::memory_order_relaxed);
        a_animationState->desiredSpeed.store(a_speed, std::memory_order_relaxed);
        a_animationState->desiredDirection.store(a_direction, std::memory_order_relaxed);
        a_animationState->desiredRevision.fetch_add(1, std::memory_order_release);
    }

    void InvalidateAnimationDispatch(State& a_state)
    {
        a_state.hasDispatched = false;
        a_state.hasAttempted = false;
        a_state.hasVariableUpdateResult = false;
        a_state.lastVariableUpdateAccepted = false;
        a_state.hasCrossBlendedDirection = false;
        a_state.directionCrossBlendPending = false;
        a_state.failedDispatchAttempts = 0;
        a_state.rejectedEventAttempts = 0;
        a_state.rejectedEventElapsed = 0.0F;
        a_state.landingCompletionPending = false;
        a_state.landingCompletionWaitFrames = 0;
        a_state.landingCompletionWaitElapsed = 0.0F;
        a_state.landingLocomotionReassertPending = false;
        a_state.landingSprintStopPending = false;
        a_state.weaponLocomotionReassertPending = false;
        a_state.weaponTransitionCompletionObserved = false;
        a_state.weaponTransitionWaitElapsed = 0.0F;
        a_state.weaponTransitionAnimationObserved = false;
        a_state.weaponTransitionExitForced = false;
        a_state.weaponDefaultStateDispatched = false;
        a_state.weaponReplayBaseDispatched = false;
        a_state.weaponTransitionMoveStartDispatched = false;
        a_state.weaponRestoredWithoutCompletion = false;
        a_state.attackPlaybackPending = false;
        a_state.attackPlaybackObserved = false;
        a_state.attackChainDispatched = false;
        a_state.attackPlaybackPower = false;
        a_state.attackPlaybackElapsed = 0.0F;
        a_state.attackLocomotionReassertPending = false;
        ResetHandPresentation(a_state);
        a_state.processedRevision.store(0, std::memory_order_release);
        a_state.graphManager = nullptr;
        a_state.landingEventSinkGraph = nullptr;
    }

    void AdvanceAttackPlayback(RE::Actor& a_actor, State& a_state, float a_deltaTime)
    {
        if (!a_state.attackPlaybackPending)
        {
            return;
        }

        a_state.attackPlaybackElapsed += std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        const auto* actorState = a_actor.AsActorState();
        const bool attacking = actorState && actorState->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
        a_state.attackPlaybackObserved = a_state.attackPlaybackObserved || attacking;
        if (a_state.attackChainDispatched && actorState && !IsAttackChainWindow(actorState->GetAttackState()))
        {
            a_state.attackChainDispatched = false;
        }
        const bool bashExited = a_state.attackPlaybackBash && ActorRegistry::ConsumeBashExits(a_actor) != 0;
        const bool completed = (a_state.attackPlaybackObserved && !attacking) || bashExited;
        const bool startTimedOut = !completed && !a_state.attackPlaybackObserved
            && a_state.attackPlaybackElapsed >= kAttackStartFallbackSeconds;
        const bool playbackTimedOut = a_state.attackPlaybackElapsed >= kAttackPlaybackFallbackSeconds;
        if (!completed && !startTimedOut && !playbackTimedOut)
        {
            return;
        }

        logs::info(
            "Animation sync completed attack ownership: actor={:08X} bash={} bashExited={} observedPlayback={} "
            "elapsed={:.3f} startTimedOut={} playbackTimedOut={}",
            a_actor.GetHandle().native_handle(), a_state.attackPlaybackBash, bashExited, a_state.attackPlaybackObserved,
            a_state.attackPlaybackElapsed, startTimedOut, playbackTimedOut);
        a_state.attackPlaybackPending = false;
        a_state.attackPlaybackObserved = false;
        a_state.attackChainDispatched = false;
        a_state.attackPlaybackPower = false;
        a_state.attackPlaybackElapsed = 0.0F;
        // Native attacks normally return to locomotion themselves. Also request an
        // explicit replay without invalidating the drawn stance; treating this as a
        // full graph reset would incorrectly start another equip lifecycle.
        a_state.attackLocomotionReassertPending = true;
    }

    bool RecoverAuthoritativeAnimation(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
    {
        const auto animationState = a_remote.animation.get();
        if (!animationState)
        {
            return false;
        }

        const auto pendingHitObservations = ActorRegistry::ConsumeHitObservations(a_remote);
        const bool isInterrupted = a_actor.Is3DLoaded() && IsActorInHitReaction(a_actor);

        if (pendingHitObservations != 0)
        {
            animationState->observedHitEvents += pendingHitObservations;
            animationState->hitReactionObservationRemaining = kHitReactionObservationSeconds;
        }

        // TESHitEvent is emitted even when the damage firewall successfully makes
        // the hit harmless. Rebuild only after the graph or actor state confirms a
        // real reaction; the short observation window catches reactions that begin
        // a frame or two after their event.
        if (isInterrupted && !animationState->localInterruptionLatched)
        {
            const auto observedHitEvents = std::exchange(animationState->observedHitEvents, 0);
            animationState->hitReactionObservationRemaining = 0.0F;
            BeginAnimationGraphRecovery(a_actor, a_remote, observedHitEvents, isInterrupted);
            return true;
        }

        const float deltaTime = std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        if (animationState->hitReactionObservationRemaining > 0.0F)
        {
            animationState->hitReactionObservationRemaining -= deltaTime;
            if (animationState->hitReactionObservationRemaining <= 0.0F)
            {
                animationState->hitReactionObservationRemaining = 0.0F;
                animationState->observedHitEvents = 0;
            }
        }

        if (!animationState->graphRecoveryInProgress)
        {
            if (!isInterrupted)
            {
                animationState->localInterruptionLatched = false;
            }
            return false;
        }

        ++animationState->recoveryWaitFrames;
        animationState->recoveryWaitElapsed += deltaTime;
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
        const bool graphReady = a_actor.Is3DLoaded() && a_actor.GetCharController()
            && a_actor.GetAnimationGraphManager(graphManager) && graphManager;
        if (!graphReady)
        {
            animationState->recoveryStableFrames = 0;
            if (!animationState->graphRecoveryUsed3DReset
                && animationState->recoveryWaitElapsed >= kGraphRecoveryFallbackSeconds)
            {
                animationState->graphRecoveryUsed3DReset = true;
                animationState->recoveryWaitFrames = 0;
                animationState->recoveryWaitElapsed = 0.0F;
                a_actor.DoReset3D(false);
                logs::warn("Actor graph revert did not become ready; escalating to 3D reset: actor={:08X} recovery={}",
                    a_remote.handle.native_handle(), animationState->recoveryCount);
                return true;
            }
            if (animationState->recoveryWaitFrames == 1
                || animationState->recoveryWaitFrames % kGraphRecoveryWaitLogInterval == 0)
            {
                logs::info(
                    "Waiting for recovered actor animation graph: actor={:08X} loaded3D={} controller={} graph={} "
                    "waitedFrames={} recovery={}",
                    a_remote.handle.native_handle(), a_actor.Is3DLoaded(), a_actor.GetCharController() != nullptr,
                    graphManager != nullptr, animationState->recoveryWaitFrames, animationState->recoveryCount);
            }
            return true;
        }

        ++animationState->recoveryStableFrames;
        if (animationState->recoveryStableFrames < kGraphRecoveryStableFrames)
        {
            return true;
        }

        const bool graphWasReplaced = graphManager.get() != animationState->graphBeforeRecovery;
        const bool used3DReset = animationState->graphRecoveryUsed3DReset;
        animationState->graphRecoveryInProgress = false;
        animationState->graphRecoveryUsed3DReset = false;
        animationState->graphBeforeRecovery = nullptr;
        animationState->recoveryWaitFrames = 0;
        animationState->recoveryWaitElapsed = 0.0F;
        animationState->recoveryStableFrames = 0;
        // A hit event can arrive just before a shout transitions the actor into
        // ragdoll/paralysis. Allow the next frame to start physical recovery if
        // that state appeared after this recovery began.
        animationState->localInterruptionLatched = false;
        logs::info(
            "Recovered actor animation graph is stable; replaying authoritative locomotion: actor={:08X} desired={} "
            "graphReplaced={} fallback3DReset={} recovery={}",
            a_remote.handle.native_handle(),
            Multiplayer::GetMovementStateName(a_remote.animation->targetAnimationState.movementState), graphWasReplaced,
            used3DReset, animationState->recoveryCount);
        return false;
    }

    void HoldIdleTurnState(RE::Actor& a_actor, RemoteActor& a_remote)
    {
        if (!a_remote.animation)
        {
            return;
        }

        // Skyrim records the local AI's turn-in-place request in iSyncTurnState along
        // with its TurnLeft/TurnRight/turnStop events. The remote actor's AI keeps asking
        // to face its own target because the network owns the heading. The firewall
        // blocks those events, but every idle turn state machine takes this variable
        // as its start state. Each time one reactivates (after a weapon draw or sheathe,
        // IdleForceDefaultState, or stopping), it would enter a turn clip that it can
        // leave only on the blocked turnStop, looping until a network event leaves idle.
        static const RE::BSFixedString syncTurnStateVariable{ "iSyncTurnState" };
        std::int32_t syncTurnState = kStandingTurnState;
        if (!a_actor.GetGraphVariableInt(syncTurnStateVariable, syncTurnState)
            || syncTurnState == kStandingTurnState)
        {
            return;
        }

        const bool accepted = a_actor.SetGraphVariableInt(syncTurnStateVariable, kStandingTurnState);
        // The AI may have written the variable after the previous pass, just as a
        // machine reactivated. turnStop returns any turn state to standing, and a
        // standing state has no turnStop transition.
        static const RE::BSFixedString turnStopEvent{ "turnStop" };
        const bool turnStopAccepted = SendNetworkAnimationEvent(a_remote, turnStopEvent);
        auto& state = *a_remote.animation;
        ++state.turnStateOverrides;
        if (state.turnStateOverrides <= kInitialTurnStateOverrideLogs
            || state.turnStateOverrides % kTurnStateOverrideLogInterval == 0)
        {
            logs::info(
                "Animation sync held idle turn state: actor={:08X} localTurnState={} accepted={} turnStop={} count={}",
                a_remote.handle.native_handle(), syncTurnState, accepted, turnStopAccepted, state.turnStateOverrides);
        }
    }

    void ApplyAnimationUpdate(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
    {
        const auto animationState = a_remote.animation.get();
        if (!animationState)
        {
            return;
        }

        static const RE::BSFixedString moveStartEvent{ "moveStart" };
        static const RE::BSFixedString moveStopEvent{ "moveStop" };
        static const RE::BSFixedString sneakStartEvent{ "sneakStart" };
        static const RE::BSFixedString sneakStopEvent{ "sneakStop" };
        static const RE::BSFixedString sprintStartEvent{ "SprintStart" };
        static const RE::BSFixedString sprintStopEvent{ "SprintStop" };
        static const RE::BSFixedString jumpStandingStartEvent{ "JumpStandingStart" };
        static const RE::BSFixedString jumpDirectionalStartEvent{ "JumpDirectionalStart" };
        static const RE::BSFixedString jumpLandEvent{ "JumpLand" };
        static const RE::BSFixedString cyclicFreezeEvent{ "CyclicFreeze" };
        static const RE::BSFixedString cyclicCrossBlendEvent{ "CyclicCrossBlend" };
        static const RE::BSFixedString jumpLandEndEvent{ "JumpLandEnd" };
        static const RE::BSFixedString weaponEquipOutEvent{ "WeapEquip_Out" };
        static const RE::BSFixedString weaponEquipOutMovingEvent{ "WeapEquip_OutMoving" };
        static const RE::BSFixedString weaponUnequipOutEvent{ "Unequip_Out" };
        static const RE::BSFixedString weaponUnequipOutMovingEvent{ "Unequip_OutMoving" };
        static const RE::BSFixedString inJumpStateVariable{ "bInJumpState" };
        static const RE::BSFixedString speedDampedVariable{ "SpeedDamped" };

        // The caller is already running in the authoritative main-loop hook.
        // Apply graph state before direct translation so a single network sample
        // cannot advance the transform with stale locomotion variables.
        const auto actorHandle = a_actor.GetHandle();
        if (!a_actor.Is3DLoaded())
        {
            AnimationFirewall::Disable(a_remote);
            ActorRegistry::SetLandingCompletionObservation(a_actor, false);
            if (animationState->hasDispatched || animationState->graphManager)
            {
                logs::info("Animation sync reset graph state: actor={:08X} reason=actor-or-3d-unavailable",
                    actorHandle.native_handle());
            }
            animationState->hasDispatched = false;
            animationState->hasAttempted = false;
            animationState->hasVariableUpdateResult = false;
            animationState->failedDispatchAttempts = 0;
            animationState->rejectedEventAttempts = 0;
            animationState->rejectedEventElapsed = 0.0F;
            animationState->landingCompletionPending = false;
            animationState->landingCompletionWaitFrames = 0;
            animationState->landingCompletionWaitElapsed = 0.0F;
            animationState->landingLocomotionReassertPending = false;
            animationState->landingSprintStopPending = false;
            animationState->weaponLocomotionReassertPending = false;
            animationState->weaponTransitionCompletionObserved = false;
            animationState->weaponTransitionWaitElapsed = 0.0F;
            animationState->weaponTransitionAnimationObserved = false;
            animationState->weaponTransitionExitForced = false;
            animationState->weaponDefaultStateDispatched = false;
            animationState->weaponReplayBaseDispatched = false;
            animationState->weaponTransitionMoveStartDispatched = false;
            animationState->attackPlaybackPending = false;
            animationState->attackPlaybackObserved = false;
            animationState->attackChainDispatched = false;
            animationState->attackPlaybackPower = false;
            animationState->attackPlaybackElapsed = 0.0F;
            animationState->attackLocomotionReassertPending = false;
            ResetHandPresentation(*animationState);
            animationState->processedRevision.store(0, std::memory_order_release);
            animationState->graphManager = nullptr;
            animationState->landingEventSinkGraph = nullptr;
            return;
        }

        auto* actor = &a_actor;
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
        if (!actor->GetAnimationGraphManager(graphManager) || !graphManager)
        {
            AnimationFirewall::Disable(a_remote);
            ActorRegistry::SetLandingCompletionObservation(*actor, false);
            if (animationState->hasDispatched || animationState->graphManager)
            {
                logs::info("Animation sync reset graph state: actor={:08X} reason=graph-unavailable",
                    actorHandle.native_handle());
            }
            animationState->hasDispatched = false;
            animationState->hasAttempted = false;
            animationState->hasVariableUpdateResult = false;
            animationState->failedDispatchAttempts = 0;
            animationState->rejectedEventAttempts = 0;
            animationState->rejectedEventElapsed = 0.0F;
            animationState->landingCompletionPending = false;
            animationState->landingCompletionWaitFrames = 0;
            animationState->landingCompletionWaitElapsed = 0.0F;
            animationState->landingLocomotionReassertPending = false;
            animationState->landingSprintStopPending = false;
            animationState->weaponLocomotionReassertPending = false;
            animationState->weaponTransitionCompletionObserved = false;
            animationState->weaponTransitionWaitElapsed = 0.0F;
            animationState->weaponTransitionAnimationObserved = false;
            animationState->weaponTransitionExitForced = false;
            animationState->weaponReplayBaseDispatched = false;
            animationState->weaponTransitionMoveStartDispatched = false;
            animationState->attackPlaybackPending = false;
            animationState->attackPlaybackObserved = false;
            animationState->attackChainDispatched = false;
            animationState->attackPlaybackPower = false;
            animationState->attackPlaybackElapsed = 0.0F;
            animationState->attackLocomotionReassertPending = false;
            ResetHandPresentation(*animationState);
            animationState->processedRevision.store(0, std::memory_order_release);
            animationState->graphManager = nullptr;
            animationState->landingEventSinkGraph = nullptr;
            return;
        }
        if (animationState->graphManager != graphManager.get())
        {
            AnimationFirewall::Disable(a_remote);
            ActorRegistry::SetLandingCompletionObservation(*actor, false);
            logs::info("Animation sync detected {} graph: actor={:08X}; next state will be fully asserted",
                animationState->graphManager ? "replacement" : "initial", actorHandle.native_handle());
            animationState->graphManager = graphManager.get();
            animationState->hasDispatched = false;
            animationState->hasAttempted = false;
            animationState->hasVariableUpdateResult = false;
            animationState->processedRevision.store(0, std::memory_order_release);
            animationState->failedDispatchAttempts = 0;
            animationState->rejectedEventAttempts = 0;
            animationState->rejectedEventElapsed = 0.0F;
            animationState->landingCompletionPending = false;
            animationState->landingCompletionWaitFrames = 0;
            animationState->landingCompletionWaitElapsed = 0.0F;
            animationState->landingLocomotionReassertPending = false;
            animationState->landingSprintStopPending = false;
            animationState->weaponLocomotionReassertPending = false;
            animationState->weaponTransitionCompletionObserved = false;
            animationState->weaponTransitionWaitElapsed = 0.0F;
            animationState->weaponTransitionAnimationObserved = false;
            animationState->weaponTransitionExitForced = false;
            animationState->weaponDefaultStateDispatched = false;
            animationState->weaponReplayBaseDispatched = false;
            animationState->weaponTransitionMoveStartDispatched = false;
            animationState->attackPlaybackPending = false;
            animationState->attackPlaybackObserved = false;
            animationState->attackChainDispatched = false;
            animationState->attackPlaybackPower = false;
            animationState->attackPlaybackElapsed = 0.0F;
            animationState->attackLocomotionReassertPending = false;
            ResetHandPresentation(*animationState);
        }
        bool landingEventSinkRegistered = animationState->landingEventSinkGraph == graphManager.get();
        if (!landingEventSinkRegistered)
        {
            landingEventSinkRegistered = AnimationFirewall::RegisterGraphOutputEventSink(*graphManager);
            if (landingEventSinkRegistered)
            {
                animationState->landingEventSinkGraph = graphManager.get();
                logs::info("Registered actor animation output-event sink: actor={:08X}", actorHandle.native_handle());
            }
        }

        if (animationState->landingCompletionPending && animationState->desiredJumping.load(std::memory_order_acquire))
        {
            // A consecutive takeoff leaves the landing branch directly, as the
            // local player's graph does. Waiting for JumpLandEnd would hold the
            // takeoff until the remote jump is already over. The jump dispatch
            // below closes the landing observation window; grounded locomotion is
            // reasserted after that jump's own landing.
            animationState->landingCompletionPending = false;
            animationState->landingCompletionWaitFrames = 0;
            animationState->landingCompletionWaitElapsed = 0.0F;
            logs::info("Animation sync takeoff preempted pending landing completion: actor={:08X}",
                actorHandle.native_handle());
        }
        const auto* actorState = actor->AsActorState();
        if (animationState->landingCompletionPending && actorState && actorState->IsSwimming())
        {
            // SwimStart replaced the landing branch, so its JumpLandEnd marker never
            // arrives. JumpLandEnd is a root wildcard transition: forcing it would pull
            // the graph from Swim_State into land locomotion while Skyrim keeps the
            // actor swimming. Swimming owns locomotion until swimStop.
            (void)ActorRegistry::ConsumeJumpLandEnds(a_remote);
            animationState->landingCompletionPending = false;
            animationState->landingCompletionWaitFrames = 0;
            animationState->landingCompletionWaitElapsed = 0.0F;
            ActorRegistry::SetLandingCompletionObservation(*actor, false);
            logs::info("Animation sync landing branch replaced by swimming: actor={:08X}", actorHandle.native_handle());
        }
        if (animationState->landingCompletionPending)
        {
            const auto completedLandings = ActorRegistry::ConsumeJumpLandEnds(a_remote);
            bool landingCompleted = completedLandings != 0;
            bool forcedCompletion = false;
            bool forcedEventAccepted = false;
            const auto landingWaitFrames = ++animationState->landingCompletionWaitFrames;
            animationState->landingCompletionWaitElapsed += std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
            if (!landingCompleted && (landingWaitFrames == 1 || landingWaitFrames % 15 == 0))
            {
                const auto currentPosition = actor->GetPosition();
                bool graphJumping = false;
                const bool graphJumpAvailable = actor->GetGraphVariableBool(inJumpStateVariable, graphJumping);
                logs::info(
                    "Animation sync landing timeline: actor={:08X} waitedFrames={} current=({:.3f},{:.3f},{:.3f}) "
                    "target=({:.3f},{:.3f},{:.3f}) zError={:.3f} physicalJump={} physicalMidair={} "
                    "graphJumpAvailable={} graphJump={}",
                    actorHandle.native_handle(), landingWaitFrames, currentPosition.x, currentPosition.y,
                    currentPosition.z, a_remote.movement.targetPosition.x, a_remote.movement.targetPosition.y,
                    a_remote.movement.targetPosition.z, a_remote.movement.targetPosition.z - currentPosition.z,
                    actor->IsInJumpState(), actor->IsInMidair(), graphJumpAvailable, graphJumping);
            }
            if (!landingCompleted && animationState->landingCompletionWaitElapsed >= kLandingCompletionFallbackSeconds)
            {
                // A custom behavior graph may omit its normal annotation. Bound the
                // wait and drive the same lifecycle edge explicitly so the remote actor
                // cannot remain in a landing branch indefinitely.
                forcedEventAccepted = SendNetworkAnimationEvent(a_remote, jumpLandEndEvent);
                landingCompleted = true;
                forcedCompletion = true;
            }

            if (landingCompleted)
            {
                const auto completedAfterFrames = landingWaitFrames;
                animationState->landingCompletionPending = false;
                animationState->landingCompletionWaitFrames = 0;
                animationState->landingCompletionWaitElapsed = 0.0F;
                ActorRegistry::SetLandingCompletionObservation(*actor, false);
                // JumpLandEnd selects the graph's exit branch. Reassert every active
                // grounded axis afterward—even if the network movement enum did not
                // change—so the actor cannot exit into idle and slide. Keep the
                // dispatch history: forgetting it reads a drawn weapon as a new draw
                // and arms a weapon-transition replay that later fires mid-jump.
                animationState->landingLocomotionReassertPending = true;
                animationState->failedDispatchAttempts = 0;
                animationState->rejectedEventAttempts = 0;
                animationState->rejectedEventElapsed = 0.0F;
                logs::info(
                    "Animation sync completing landing branch and reasserting grounded locomotion: actor={:08X} "
                    "observedMarkers={} waitedFrames={} forced={} forcedEventAccepted={}",
                    actorHandle.native_handle(), completedLandings, completedAfterFrames, forcedCompletion,
                    forcedEventAccepted);
            }
        }
        else
        {
            // Drain a duplicate graph annotation while no landing transition owns
            // it. The next takeoff also closes and clears this observation window.
            (void)ActorRegistry::ConsumeJumpLandEnds(a_remote);
        }

        const auto desiredRevision = animationState->desiredRevision.load(std::memory_order_acquire);
        const Multiplayer::AnimationState authoritativeState{ .movementState
            = animationState->desired.load(std::memory_order_acquire),
            .jumping = animationState->desiredJumping.load(std::memory_order_acquire),
            .weaponDrawn = animationState->desiredWeaponDrawn.load(std::memory_order_acquire) };
        const auto sampleSpeed = animationState->desiredSpeed.load(std::memory_order_acquire);
        const auto sampleDirection = animationState->desiredDirection.load(std::memory_order_acquire);
        const bool authoritativeDispatchRequired
            = !animationState->hasDispatched || animationState->lastDispatched != authoritativeState;
        const auto previousState = animationState->lastDispatched;
        const bool hadPreviouslyDispatched = animationState->hasDispatched;
        const bool weaponDrawnChanged = authoritativeDispatchRequired
            && (hadPreviouslyDispatched ? previousState.weaponDrawn != authoritativeState.weaponDrawn
                                        : authoritativeState.weaponDrawn);
        if (weaponDrawnChanged)
        {
            animationState->weaponLocomotionReassertPending = true;
            animationState->weaponTransitionCompletionObserved = false;
            animationState->weaponTransitionWaitElapsed = 0.0F;
            animationState->weaponTransitionAnimationObserved = false;
            animationState->weaponTransitionExitForced = false;
            animationState->weaponDefaultStateDispatched = false;
            animationState->weaponReplayBaseDispatched = false;
            animationState->weaponTransitionMoveStartDispatched = false;
            animationState->weaponRestoredWithoutCompletion = false;
        }
        const auto weaponState = actorState ? actorState->GetWeaponState() : RE::WEAPON_STATE::kSheathed;
        const auto observedWeaponTransitionEnds = ActorRegistry::ConsumeWeaponTransitionEnds(a_remote);
        if (observedWeaponTransitionEnds != 0)
        {
            animationState->weaponTransitionCompletionObserved = true;
            const bool lateCompletion
                = !animationState->weaponLocomotionReassertPending && animationState->weaponRestoredWithoutCompletion;
            if (lateCompletion)
            {
                // The bounded fallback restored locomotion while the clip was still
                // playing, so the clip consumed that replay and its exit leaves the
                // graph in a standing stance. Replay locomotion again from this exit.
                animationState->weaponRestoredWithoutCompletion = false;
                animationState->weaponLocomotionReassertPending = true;
                animationState->weaponTransitionWaitElapsed = 0.0F;
                animationState->weaponTransitionAnimationObserved = false;
                animationState->weaponTransitionExitForced = false;
                animationState->weaponDefaultStateDispatched = false;
                animationState->weaponReplayBaseDispatched = false;
                animationState->weaponTransitionMoveStartDispatched = false;
            }
            logs::info(
                "Animation sync observed weapon transition exit: actor={:08X} weaponDrawn={} weaponState={} markers={} "
                "lateCompletion={}",
                actorHandle.native_handle(), authoritativeState.weaponDrawn,
                static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(weaponState), observedWeaponTransitionEnds,
                lateCompletion);
            if (authoritativeState.weaponDrawn)
            {
                // Resetting a successfully drawn graph to its default state makes its
                // behavior state sheathed while ActorState remains drawn. Preserve the
                // drawn branch and only allow its worker-thread exit one update to settle.
                return;
            }
        }
        if (animationState->weaponLocomotionReassertPending && !animationState->weaponTransitionCompletionObserved
            && !animationState->weaponTransitionExitForced && !animationState->weaponTransitionAnimationObserved
            && IsWeaponTransitionAnimating(weaponState, authoritativeState.weaponDrawn))
        {
            // A request that flips during an opposite transition (for example a
            // draw started just before a sheathe arrives) spends most of the budget
            // on that clip. Timing out mid-sheathe resets the graph before its
            // weaponSheathe annotation, leaving the weapon in hand. Restart the
            // bounded wait once, when the requested clip actually starts.
            animationState->weaponTransitionAnimationObserved = true;
            animationState->weaponTransitionWaitElapsed = 0.0F;
        }
        if (animationState->weaponLocomotionReassertPending
            && animationState->weaponTransitionWaitElapsed < kWeaponTransitionFallbackSeconds)
        {
            animationState->weaponTransitionWaitElapsed += std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        }
        const bool weaponTransitionTimedOut
            = animationState->weaponTransitionWaitElapsed >= kWeaponTransitionFallbackSeconds;
        const bool weaponTransitionCompleted
            = animationState->weaponTransitionCompletionObserved || weaponTransitionTimedOut;
        if (animationState->weaponLocomotionReassertPending && weaponTransitionTimedOut
            && !animationState->weaponTransitionCompletionObserved && !animationState->weaponTransitionExitForced)
        {
            const bool authoritativeIsMoving = IsMoving(authoritativeState.movementState);
            const auto& transitionEndEvent = authoritativeState.weaponDrawn
                ? (authoritativeIsMoving ? weaponEquipOutMovingEvent : weaponEquipOutEvent)
                : (authoritativeIsMoving ? weaponUnequipOutMovingEvent : weaponUnequipOutEvent);
            const bool forcedExitAccepted = SendNetworkAnimationEvent(a_remote, transitionEndEvent);
            animationState->weaponTransitionExitForced = true;
            logs::warn(
                "Animation sync weapon transition completion timed out; sent bounded exit: actor={:08X} weaponDrawn={} "
                "weaponState={} waitedSeconds={:.3f} event={} accepted={}",
                actorHandle.native_handle(), authoritativeState.weaponDrawn,
                static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(weaponState),
                animationState->weaponTransitionWaitElapsed, transitionEndEvent.c_str(), forcedExitAccepted);
            if (forcedExitAccepted)
            {
                // Give an accepted exit edge one graph update before restoring
                // locomotion. No longer-lived recovery gate is introduced.
                return;
            }
        }
        if (animationState->weaponLocomotionReassertPending && weaponTransitionCompleted
            && !authoritativeState.weaponDrawn && !animationState->weaponDefaultStateDispatched)
        {
            // The unequip exit annotation finishes the clip, but does not guarantee
            // that the behavior graph is in a state that can consume locomotion. A
            // direct moveStart can report success while being ignored. Once ActorState
            // is also sheathed, force the graph's matching default state first and
            // replay authoritative locomotion on the next update.
            const bool defaultStateAccepted = AnimationFirewall::ForceDefaultGraphState(a_actor);
            animationState->weaponDefaultStateDispatched = true;
            logs::info(
                "Animation sync reset graph to default after weapon transition: actor={:08X} weaponDrawn={} "
                "weaponState={} completionMarkerObserved={} eventAccepted={}",
                actorHandle.native_handle(), authoritativeState.weaponDrawn,
                static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(weaponState),
                animationState->weaponTransitionCompletionObserved, defaultStateAccepted);
            return;
        }
        if (ActorRegistry::ConsumeSwimStarts(a_remote) != 0)
        {
            animationState->swimMoveStartPending = true;
            animationState->swimMoveStartWaitPasses = 0;
            animationState->swimMoveStartAttempts = 0;
            animationState->swimMoveStartElapsed = 0.0F;
        }
        const auto observedSwimStops = ActorRegistry::ConsumeSwimStops(a_remote);
        if (observedSwimStops != 0)
        {
            animationState->swimLocomotionReassertPending = true;
            animationState->swimMoveStartPending = false;
        }
        AdvanceSwimMoveStart(a_remote, *animationState, actorState, a_deltaTime);
        const bool authoritativeLocomotionActive
            = IsMoving(authoritativeState.movementState) || authoritativeState.jumping;
        const bool reassertLocomotionAfterWeaponTransition
            = ShouldReassertAfterWeaponTransition(animationState->weaponLocomotionReassertPending,
                weaponTransitionCompleted, IsMoving(authoritativeState.movementState), authoritativeState.jumping);
        const bool reassertLocomotionAfterSwimStop
            = animationState->swimLocomotionReassertPending && actorState && !actorState->IsSwimming();
        const bool reassertLocomotionAfterAttack
            = animationState->attackLocomotionReassertPending && !animationState->attackPlaybackPending;
        const bool reassertLocomotionAfterLanding = animationState->landingLocomotionReassertPending;
        // A weapon branch may consume locomotion events. Replay a valid base edge
        // first, then layer sprint/sneak movement/jump on a later graph update.
        // Sending every edge together can make each one report success while no
        // runnable locomotion branch actually becomes active.
        const auto replayBaseState = GetWeaponReplayBaseState(authoritativeState);
        if (animationState->weaponLocomotionReassertPending && animationState->weaponReplayBaseDispatched
            && !authoritativeLocomotionActive)
        {
            // Do not carry a partially replayed base across an idle interval.
            animationState->weaponReplayBaseDispatched = false;
        }
        const bool replayingWeaponBase
            = reassertLocomotionAfterWeaponTransition && !animationState->weaponReplayBaseDispatched;
        Multiplayer::AnimationState state = authoritativeState;
        if (replayingWeaponBase)
        {
            state = replayBaseState;
        }
        else if (reassertLocomotionAfterWeaponTransition && authoritativeState.jumping)
        {
            auto groundedState = authoritativeState;
            groundedState.jumping = false;
            if (animationState->lastDispatched != groundedState)
            {
                // Sprint/sneak movement is another graph edge. Establish it before
                // dispatching takeoff rather than stacking both on one update.
                state = groundedState;
            }
        }
        const bool weaponReplayNeedsFollowup = reassertLocomotionAfterWeaponTransition && state != authoritativeState;
        const bool isMoving = IsMoving(state.movementState);
        const bool isSneaking = IsSneaking(state.movementState);
        const bool isSprinting = IsSprinting(state.movementState);
        const float graphSpeed = GetGraphSpeedForState(*actor, sampleSpeed, state.movementState);
        const bool hadPreviousState = hadPreviouslyDispatched && !replayingWeaponBase;
        const bool dispatchRequired = reassertLocomotionAfterWeaponTransition || !animationState->hasDispatched
            || animationState->lastDispatched != state;
        const char* const previousStateLabel
            = hadPreviousState ? Multiplayer::GetMovementStateName(previousState.movementState) : "uninitialized";
        const bool jumpRequired
            = dispatchRequired && (hadPreviousState ? previousState.jumping != state.jumping : state.jumping);
        // A landing in water sends no JumpLand. The root graph takes it from any state,
        // including Swim_State, and the landing branch then exits into land locomotion.
        const bool landingInWater = jumpRequired && !state.jumping && actorState && actorState->IsSwimming();
        const bool jumpLandRequired = jumpRequired && !state.jumping && !landingInWater;
        if (jumpLandRequired && !landingEventSinkRegistered)
        {
            // Do not enter a landing branch whose completion marker cannot yet be
            // observed. Graph construction can expose the manager one pass before
            // its first animation graph becomes available.
            return;
        }
        // The landing checkpoint's variables must represent current movement,
        // not takeoff movement. Ordinary locomotion events are deferred until
        // JumpLandEnd, when the full grounded state is reasserted.
        const bool moveRequired = !jumpLandRequired
            && ((dispatchRequired && (hadPreviousState ? IsMoving(previousState.movementState) != isMoving : isMoving))
                || ((replayingWeaponBase || reassertLocomotionAfterSwimStop || reassertLocomotionAfterAttack
                        || reassertLocomotionAfterLanding)
                    && isMoving));
        const bool sneakRequired = !jumpLandRequired
            && ((dispatchRequired
                    && (hadPreviousState ? IsSneaking(previousState.movementState) != isSneaking : isSneaking))
                || ((replayingWeaponBase || reassertLocomotionAfterSwimStop || reassertLocomotionAfterAttack
                        || reassertLocomotionAfterLanding)
                    && isSneaking));
        const bool eventDispatchRequired = dispatchRequired || reassertLocomotionAfterSwimStop
            || reassertLocomotionAfterAttack || reassertLocomotionAfterLanding;
        const bool sprintRequired = !jumpLandRequired
            && ((dispatchRequired
                    && (hadPreviousState ? IsSprinting(previousState.movementState) != isSprinting : isSprinting))
                || ((replayingWeaponBase || reassertLocomotionAfterSwimStop || reassertLocomotionAfterAttack
                        || reassertLocomotionAfterLanding)
                    && isSprinting)
                || (eventDispatchRequired && animationState->landingSprintStopPending && !isSprinting));
        // A landing from sprint into a non-sprint state defers SprintStop to the
        // landing reassert. The stop edge is evaluated against IsSprinting, so the
        // variable keeps its sprint value until that stop has been sent.
        const bool deferSprintStop
            = jumpLandRequired && !isSprinting && hadPreviousState && IsSprinting(previousState.movementState);
        if (dispatchRequired && (!animationState->hasAttempted || animationState->lastAttempted != state))
        {
            animationState->lastAttempted = state;
            animationState->hasAttempted = true;
            animationState->failedDispatchAttempts = 0;
            animationState->rejectedEventAttempts = 0;
            animationState->rejectedEventElapsed = 0.0F;
        }

        auto* strings = RE::FixedStrings::GetSingleton();
        const bool stringsAvailable = strings != nullptr;
        // Stop edges are evaluated against the old graph variables. Clearing
        // Speed or IsSprinting first made the recorded sprint/idle exits reject
        // deterministically and left the graph in its previous branch.
        const bool moveStopRequired = moveRequired && !isMoving;
        const bool sneakStopRequired = sneakRequired && !isSneaking;
        const bool sprintStopRequired = sprintRequired && !isSprinting;
        const bool sprintVariableValue
            = isSprinting || deferSprintStop || (animationState->landingSprintStopPending && !sprintStopRequired);
        if (jumpRequired)
        {
            // JumpLand starts a timed landing branch whose graph output emits
            // JumpLandEnd later, often from an animation worker thread. Observe that
            // output only while the network-authoritative state is grounded.
            ActorRegistry::SetLandingCompletionObservation(*actor, jumpLandRequired);
        }
        bool moveAccepted
            = moveStopRequired && stringsAvailable ? SendNetworkAnimationEvent(a_remote, moveStopEvent) : false;
        bool sneakAccepted
            = sneakStopRequired && stringsAvailable ? SendNetworkAnimationEvent(a_remote, sneakStopEvent) : false;
        bool sprintAccepted
            = sprintStopRequired && stringsAvailable ? SendNetworkAnimationEvent(a_remote, sprintStopEvent) : false;
        bool jumpAccepted = false;
        const bool stopEventsAccepted = (!moveStopRequired || moveAccepted) && (!sneakStopRequired || sneakAccepted)
            && (!sprintStopRequired || sprintAccepted);
        const auto stopEventAttempt = animationState->rejectedEventAttempts + 1;
        if (!stopEventsAccepted
            && (stopEventAttempt < kMaximumEventDispatchAttempts
                || animationState->rejectedEventElapsed < kLocomotionEventRetrySeconds))
        {
            if (ShouldReportEventRetry(stopEventAttempt))
            {
                logs::info(
                    "Animation sync graph dispatch: actor={:08X} revision={} previous={}/{} desired={}/{} "
                    "result=retrying-stop-events strings={} moveEvent={}/{} sneakEvent={}/{} sprintEvent={}/{} "
                    "jumpEvent=deferred/{} eventAttempt={}/{}",
                    actorHandle.native_handle(), static_cast<unsigned long long>(desiredRevision), previousStateLabel,
                    hadPreviousState && previousState.jumping ? "jumping" : "grounded",
                    Multiplayer::GetMovementStateName(state.movementState), state.jumping ? "jumping" : "grounded",
                    stringsAvailable, moveAccepted, moveStopRequired, sneakAccepted, sneakStopRequired, sprintAccepted,
                    sprintStopRequired, jumpLandRequired, stopEventAttempt, kMaximumEventDispatchAttempts);
            }
            ++animationState->rejectedEventAttempts;
            animationState->rejectedEventElapsed += std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
            return;
        }
        if (jumpLandRequired)
        {
            const auto positionBeforeLanding = actor->GetPosition();
            bool graphJumpingBeforeLanding = false;
            const bool graphJumpAvailableBeforeLanding
                = actor->GetGraphVariableBool(inJumpStateVariable, graphJumpingBeforeLanding);
            // The grounded checkpoint and JumpLand describe one boundary. Clear
            // interpolation backlog before changing the persistent graph state,
            // otherwise the landing branch can begin while the remote actor is aloft.
            actor->SetPosition(a_remote.movement.targetPosition, true);
            if (animationState->rejectedEventAttempts == 0)
            {
                logs::info(
                    "Animation sync aligned landing transform: actor={:08X} current=({:.3f},{:.3f},{:.3f}) "
                    "target=({:.3f},{:.3f},{:.3f}) zError={:.3f} physicalJump={} physicalMidair={} "
                    "graphJumpAvailable={} graphJump={}",
                    actorHandle.native_handle(), positionBeforeLanding.x, positionBeforeLanding.y,
                    positionBeforeLanding.z, a_remote.movement.targetPosition.x, a_remote.movement.targetPosition.y,
                    a_remote.movement.targetPosition.z, a_remote.movement.targetPosition.z - positionBeforeLanding.z,
                    actor->IsInJumpState(), actor->IsInMidair(), graphJumpAvailableBeforeLanding,
                    graphJumpingBeforeLanding);
            }
        }

        bool speedAccepted = false;
        bool sampledSpeedAccepted = false;
        bool dampedSpeedAccepted = false;
        bool directionAccepted = false;
        bool turnAccepted = false;
        bool sprintVariableAccepted = false;
        bool jumpVariableAccepted = false;
        if (strings)
        {
            speedAccepted = actor->SetGraphVariableFloat(strings->speed, graphSpeed);
            sampledSpeedAccepted = actor->SetGraphVariableFloat(strings->speedSampled, graphSpeed);
            dampedSpeedAccepted = actor->SetGraphVariableFloat(speedDampedVariable, graphSpeed);
            directionAccepted = actor->SetGraphVariableFloat(strings->direction, isMoving ? sampleDirection : 0.0F);
            turnAccepted = actor->SetGraphVariableFloat(strings->turnDelta, 0.0F);
            sprintVariableAccepted = actor->SetGraphVariableBool(strings->isSprinting, sprintVariableValue);
            jumpVariableAccepted = actor->SetGraphVariableBool(inJumpStateVariable, state.jumping);
        }
        const bool variablesAccepted = stringsAvailable && speedAccepted && sampledSpeedAccepted && dampedSpeedAccepted
            && directionAccepted && turnAccepted && sprintVariableAccepted && jumpVariableAccepted;
        if (animationState->hasDispatched && animationState->hasVariableUpdateResult
            && animationState->lastVariableUpdateAccepted != variablesAccepted)
        {
            logs::info(
                "Animation sync graph variable refresh changed: actor={:08X} revision={} state={} result={} strings={} "
                "speed={} sampledSpeed={} dampedSpeed={} direction={} turn={} sprint={} jump={}",
                actorHandle.native_handle(), static_cast<unsigned long long>(desiredRevision),
                Multiplayer::GetMovementStateName(state.movementState), variablesAccepted ? "accepted" : "rejected",
                stringsAvailable, speedAccepted, sampledSpeedAccepted, dampedSpeedAccepted, directionAccepted,
                turnAccepted, sprintVariableAccepted, jumpVariableAccepted);
        }
        animationState->lastVariableUpdateAccepted = variablesAccepted;
        animationState->hasVariableUpdateResult = true;
        if (!eventDispatchRequired && variablesAccepted)
        {
            animationState->processedRevision.store(desiredRevision, std::memory_order_release);
        }
        if (eventDispatchRequired)
        {
            // Takeoff, landing, and locomotion start edges are evaluated against
            // the new persistent graph variables.
            if (moveRequired && isMoving && variablesAccepted)
            {
                moveAccepted = SendNetworkAnimationEvent(a_remote, moveStartEvent);
            }
            if (sneakRequired && isSneaking && variablesAccepted)
            {
                sneakAccepted = SendNetworkAnimationEvent(a_remote, sneakStartEvent);
            }
            if (sprintRequired && isSprinting && variablesAccepted)
            {
                sprintAccepted = SendNetworkAnimationEvent(a_remote, sprintStartEvent);
            }
            if (jumpRequired && !landingInWater && variablesAccepted)
            {
                if (jumpLandRequired)
                {
                    ActorRegistry::ArmJumpLandEndObservation(*actor);
                }
                jumpAccepted = SendNetworkAnimationEvent(a_remote,
                    state.jumping ? (isMoving ? jumpDirectionalStartEvent : jumpStandingStartEvent) : jumpLandEvent);
            }
            const bool eventsAccepted = (!moveRequired || moveAccepted) && (!sneakRequired || sneakAccepted)
                && (!sprintRequired || sprintAccepted) && (!jumpRequired || landingInWater || jumpAccepted);
            const auto variableAttempt = animationState->failedDispatchAttempts + 1;
            const auto eventAttempt = animationState->rejectedEventAttempts + 1;
            const bool eventAttemptsExhausted = !eventsAccepted && eventAttempt >= kMaximumEventDispatchAttempts
                && animationState->rejectedEventElapsed >= kLocomotionEventRetrySeconds;
            const bool stateProcessed = variablesAccepted && (eventsAccepted || eventAttemptsExhausted);
            // A rejection burst repeats on every pass; only its outcome and a
            // sample of the wait are worth a line.
            const bool retryingEvents = variablesAccepted && !stateProcessed;
            const bool reportDispatch = retryingEvents ? ShouldReportEventRetry(eventAttempt)
                                                       : variablesAccepted || ShouldReportEventRetry(variableAttempt);
            if (reportDispatch)
            {
                logs::info(
                    "Animation sync graph dispatch: actor={:08X} revision={} previous={}/{} desired={}/{} result={} "
                    "eventsAccepted={} strings={} speedVariable={} sampledSpeedVariable={} dampedSpeedVariable={} "
                    "directionVariable={} turnVariable={} sprintVariable={} jumpVariable={} moveEvent={}/{} "
                    "sneakEvent={}/{} sprintEvent={}/{} jumpEvent={}/{} graphSpeed={:.1f} variableAttempt={} "
                    "eventAttempt={}/{}",
                    actorHandle.native_handle(), static_cast<unsigned long long>(desiredRevision), previousStateLabel,
                    hadPreviousState && previousState.jumping ? "jumping" : "grounded",
                    Multiplayer::GetMovementStateName(state.movementState), state.jumping ? "jumping" : "grounded",
                    !variablesAccepted           ? "variables-rejected"
                        : eventsAccepted         ? "processed"
                        : eventAttemptsExhausted ? "processed-event-timeout"
                                                 : "retrying-events",
                    eventsAccepted, stringsAvailable, speedAccepted, sampledSpeedAccepted, dampedSpeedAccepted,
                    directionAccepted, turnAccepted, sprintVariableAccepted, jumpVariableAccepted, moveAccepted,
                    moveRequired, sneakAccepted, sneakRequired, sprintAccepted, sprintRequired, jumpAccepted,
                    jumpRequired, graphSpeed, variableAttempt, eventAttempt, kMaximumEventDispatchAttempts);
            }

            if (stateProcessed)
            {
                animationState->lastDispatched = state;
                animationState->hasDispatched = true;
                if (reassertLocomotionAfterWeaponTransition)
                {
                    if (weaponReplayNeedsFollowup)
                    {
                        animationState->weaponReplayBaseDispatched = true;
                        logs::info(
                            "Animation sync staged weapon locomotion replay: actor={:08X} step={}/{} target={}/{} "
                            "weaponDrawn={} eventTimedOut={}",
                            actorHandle.native_handle(), Multiplayer::GetMovementStateName(state.movementState),
                            state.jumping ? "jumping" : "grounded",
                            Multiplayer::GetMovementStateName(authoritativeState.movementState),
                            authoritativeState.jumping ? "jumping" : "grounded", authoritativeState.weaponDrawn,
                            eventAttemptsExhausted);
                    }
                    else
                    {
                        const bool weaponCompletionWasObserved = animationState->weaponTransitionCompletionObserved;
                        const auto weaponTransitionWaitElapsed = animationState->weaponTransitionWaitElapsed;
                        animationState->weaponLocomotionReassertPending = false;
                        animationState->weaponTransitionCompletionObserved = false;
                        animationState->weaponTransitionWaitElapsed = 0.0F;
                        animationState->weaponTransitionAnimationObserved = false;
                        animationState->weaponTransitionExitForced = false;
                        animationState->weaponDefaultStateDispatched = false;
                        animationState->weaponReplayBaseDispatched = false;
                        animationState->weaponTransitionMoveStartDispatched = false;
                        animationState->weaponRestoredWithoutCompletion = !weaponCompletionWasObserved;
                        logs::info(
                            "Animation sync restored state after weapon transition: actor={:08X} state={} jumping={} "
                            "weaponDrawn={} completionMarkerObserved={} weaponState={} transitionWaitSeconds={:.3f} "
                            "eventTimedOut={}",
                            actorHandle.native_handle(), Multiplayer::GetMovementStateName(state.movementState),
                            state.jumping, state.weaponDrawn, weaponCompletionWasObserved,
                            static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(weaponState),
                            weaponTransitionWaitElapsed, eventAttemptsExhausted);
                    }
                }
                if (reassertLocomotionAfterSwimStop)
                {
                    animationState->swimLocomotionReassertPending = false;
                    logs::info("Animation sync reasserted locomotion after swim stop: actor={:08X} state={}",
                        actorHandle.native_handle(), Multiplayer::GetMovementStateName(state.movementState));
                }
                if (reassertLocomotionAfterLanding)
                {
                    animationState->landingLocomotionReassertPending = false;
                }
                if (reassertLocomotionAfterAttack)
                {
                    animationState->attackLocomotionReassertPending = false;
                    logs::info(
                        "Animation sync reasserted locomotion after attack or hand presentation: actor={:08X} state={}",
                        actorHandle.native_handle(), Multiplayer::GetMovementStateName(state.movementState));
                }
                animationState->failedDispatchAttempts = 0;
                animationState->rejectedEventAttempts = 0;
                animationState->rejectedEventElapsed = 0.0F;
                // Any other processed dispatch has sent the owed stop.
                animationState->landingSprintStopPending = deferSprintStop;
                if (animationState->landingSprintStopPending)
                {
                    logs::info("Animation sync deferred sprint stop past landing: actor={:08X} revision={}",
                        actorHandle.native_handle(), static_cast<unsigned long long>(desiredRevision));
                }
                if (jumpLandRequired)
                {
                    animationState->landingCompletionPending = true;
                    animationState->landingCompletionWaitFrames = 0;
                    animationState->landingCompletionWaitElapsed = 0.0F;
                    logs::info("Animation sync waiting for landing completion marker: actor={:08X} revision={}",
                        actorHandle.native_handle(), static_cast<unsigned long long>(desiredRevision));
                }
                else if (landingInWater)
                {
                    logs::info("Animation sync skipped landing branch while swimming: actor={:08X} revision={}",
                        actorHandle.native_handle(), static_cast<unsigned long long>(desiredRevision));
                }
                animationState->processedRevision.store(desiredRevision, std::memory_order_release);
            }
            else if (!variablesAccepted)
            {
                ++animationState->failedDispatchAttempts;
            }
            else
            {
                ++animationState->rejectedEventAttempts;
                animationState->rejectedEventElapsed += std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
            }
        }

        animationState->directionCrossBlendElapsed += std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        if (animationState->directionCrossBlendPending)
        {
            // One graph update after CyclicFreeze, matching Skyrim's own spacing.
            animationState->directionCrossBlendPending = false;
            (void)SendNetworkAnimationEvent(a_remote, cyclicCrossBlendEvent);
        }
        else if (!isMoving || state.jumping || !variablesAccepted)
        {
            animationState->hasCrossBlendedDirection = false;
        }
        else
        {
            const float directionChange = std::abs(sampleDirection - animationState->crossBlendedDirection);
            const float cyclicDirectionChange = (std::min)(directionChange, 1.0F - directionChange);
            if (!animationState->hasCrossBlendedDirection
                || (cyclicDirectionChange >= kDirectionCrossBlendThreshold
                    && animationState->directionCrossBlendElapsed >= kDirectionCrossBlendIntervalSeconds))
            {
                (void)SendNetworkAnimationEvent(a_remote, cyclicFreezeEvent);
                animationState->crossBlendedDirection = sampleDirection;
                animationState->hasCrossBlendedDirection = true;
                animationState->directionCrossBlendPending = true;
                animationState->directionCrossBlendElapsed = 0.0F;
            }
        }

        // Let the native transition start only after this checkpoint's persistent
        // speed and locomotion events have settled. Once active, moveStart selects
        // the equip graph's built-in locomotion blend without overriding its sync
        // state variable.
        if (IsAnimationStateSettled(a_remote))
        {
            ApplyAuthoritativeWeaponPresentation(*actor, a_remote, isMoving, moveRequired && isMoving && moveAccepted);
        }
    }

    bool IsFurnitureEntryBlocked(const RemoteActor& a_remote)
    {
        return a_remote.animation
            && (a_remote.animation->attackPlaybackPending || IsHandPresentationPlaying(a_remote)
                || IsWeaponTransitionUnsettled(*a_remote.animation) || a_remote.animation->graphRecoveryInProgress);
    }
}
