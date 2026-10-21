#include "actor/ActorAnimation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "actor/AnimationFirewall.h"
#include "actor/GraphStance.h"
#include "actor/RemoteActor.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>
#include <utility>

namespace ActorAnimation
{
    namespace
    {
        using Multiplayer::IsMoving;
        using Multiplayer::IsSneaking;
        using Multiplayer::IsSprinting;

        constexpr std::uint32_t kMaximumEventDispatchAttempts = 4;
        constexpr float kMinimumEventRetrySeconds = 0.1F;
        // The graph refuses locomotion while it is still leaving an attack, and a
        // power attack's exit was measured holding it past 0.1 seconds.
        constexpr float kLocomotionEventRetrySeconds = 0.75F;
        // Skyrim cross-blends directional locomotion (CyclicFreeze, then
        // CyclicCrossBlend) instead of sweeping through intermediate directions.
        constexpr float kDirectionCrossBlendThreshold = 0.125F;
        constexpr float kDirectionCrossBlendIntervalSeconds = 0.25F;
        // Longer than a held stop's zero-speed tail (the stop hold plus one checkpoint),
        // shorter than running in place against a wall or through a load.
        constexpr float kZeroSpeedHoldSeconds = 0.25F;
        constexpr float kHitReactionObservationSeconds = 0.1F;
        // The idle turn state machines of the vanilla behaviors (MT_Idle_TurnBehavior,
        // 1HM_Idle_Behavior, Bow_Idle_Behavior, MagicReadyIdleTurnBehavior, ...) number
        // their states 0 turn right, 1 standing, and 2 turn left.
        constexpr std::int32_t kStandingTurnState = 1;
        constexpr float kUnsettledTranslationHoldSeconds = 0.15F;
        // Main-loop passes: the reverted graph and controller stay available across consecutive passes.
        constexpr std::uint32_t kGraphRecoveryStableFrames = 2;
        constexpr float kGraphRecoveryFallbackSeconds = 2.0F;
        constexpr std::uint32_t kGraphRecoveryWaitLogInterval = 120;
        constexpr float kLandingCompletionFallbackSeconds = 2.0F;
        // A one-handed sword draw takes about 1.4 seconds.
        constexpr float kWeaponTransitionFallbackSeconds = 2.5F;
        constexpr float kAttackStartFallbackSeconds = 1.25F;
        constexpr float kAttackPlaybackFallbackSeconds = 4.0F;
        constexpr float kBowReleaseFallbackSeconds = 1.5F;
        // The release clip's length.
        constexpr float kBowShotSeconds = 0.5F;
        constexpr float kBowShotFallbackSeconds = 2.0F;
        constexpr float kCastStopFallbackSeconds = 1.0F;
        // iState read 10 while a cast played and still about 0.2 s after both casters
        // were idle with IsCasting* cleared; spell-ready locomotion reads 9 (observed
        // in game). A moveStart in that window was accepted by the exiting cast lower
        // body, which then left the actor in the standing spell-ready state.
        constexpr std::int32_t kCastGraphStateTag = 10;
        constexpr float kBlockStopFallbackSeconds = 1.5F;
        // SwimIdle rejects moveStart until the graph has taken SwimStart.
        constexpr float kSwimMoveStartFallbackSeconds = 0.5F;
        // The vanilla sneak branches tag iState 2 (BSiStateTaggingGenerator) and set
        // IsSneaking (BSIsActiveModifier, which clears it on deactivation) while
        // active. Neither covers every branch: MagicBehavior's sneak branches only
        // tag iState, and the unarmed, weapon, and bow sneak idles only set IsSneaking.
        constexpr std::int32_t kSneakGraphStateTag = 2;
        constexpr float kGraphStanceSampleSeconds = 0.1F;
        // A sneakStop keeps the sneak branch active while it blends out.
        constexpr float kGraphSneakMismatchSeconds = 0.5F;
        // A standing/moving selector switches as the edge is taken, with no blend to
        // wait out; two samples rule out a single update between edge and switch.
        constexpr float kGraphStandingMismatchSeconds = 0.2F;
        constexpr std::uint32_t kMaximumGraphStanceCorrections = 3;
#ifndef NDEBUG
        constexpr float kGraphProbeSettleSeconds = 0.5F;
#endif

        // Counting passes rather than seconds is intended: this only throttles the log.
        constexpr bool ShouldReportEventRetry(std::uint32_t a_attempt) noexcept
        {
            constexpr std::uint32_t kEventRetryReportInterval = 60;
            return a_attempt == 1 || a_attempt % kEventRetryReportInterval == 0;
        }

        static_assert(ShouldReportEventRetry(1));
        static_assert(!ShouldReportEventRetry(2));
        static_assert(ShouldReportEventRetry(60));

        // A swing that has hit can take attackStart as its next combo swing; the graph
        // decides when. A source's vanilla graph accepted it right after the hit and
        // swung after attackWinStart (observed in game), and MCO-style graphs whose clips
        // have no attackWinStart never report kNextAttack or kFollowThrough.
        constexpr bool IsAttackChainWindow(RE::ATTACK_STATE_ENUM a_state) noexcept
        {
            return a_state == RE::ATTACK_STATE_ENUM::kHit || a_state == RE::ATTACK_STATE_ENUM::kNextAttack
                || a_state == RE::ATTACK_STATE_ENUM::kFollowThrough;
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

        constexpr Multiplayer::MovementState ToggleSneak(Multiplayer::MovementState a_state) noexcept
        {
            using Multiplayer::MovementState;
            switch (a_state)
            {
                case MovementState::kIdle:
                    return MovementState::kSneakingIdle;
                case MovementState::kWalking:
                    return MovementState::kSneakingWalking;
                case MovementState::kRunning:
                    return MovementState::kSneakingRunning;
                case MovementState::kSneakingIdle:
                    return MovementState::kIdle;
                case MovementState::kSneakingWalking:
                    return MovementState::kWalking;
                case MovementState::kSneakingRunning:
                    return MovementState::kRunning;
                default:
                    return a_state;
            }
        }

        static_assert(
            ToggleSneak(Multiplayer::MovementState::kRunning) == Multiplayer::MovementState::kSneakingRunning);
        static_assert(ToggleSneak(Multiplayer::MovementState::kSneakingIdle) == Multiplayer::MovementState::kIdle);
        static_assert(ToggleSneak(Multiplayer::MovementState::kSprinting) == Multiplayer::MovementState::kSprinting);

        constexpr Multiplayer::MovementState ToStanding(Multiplayer::MovementState a_state) noexcept
        {
            return IsSneaking(a_state) ? Multiplayer::MovementState::kSneakingIdle : Multiplayer::MovementState::kIdle;
        }

        static_assert(
            ToStanding(Multiplayer::MovementState::kSneakingRunning) == Multiplayer::MovementState::kSneakingIdle);
        static_assert(ToStanding(Multiplayer::MovementState::kRunning) == Multiplayer::MovementState::kIdle);

        static_assert(GetWeaponReplayBaseState(Multiplayer::AnimationState{
                          .movementState = Multiplayer::MovementState::kSprinting, .jumping = true })
            == Multiplayer::AnimationState{ .movementState = Multiplayer::MovementState::kRunning });

        bool RevertAnimationGraphManager(RE::Actor& a_actor)
        {
            using func_t = bool(RE::IAnimationGraphManagerHolder*);
            static REL::Relocation<func_t> func{ RELOCATION_ID(32139, 32883) };
            auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(std::addressof(a_actor));
            return func(holder);
        }

        std::int32_t GetGraphStateTag(const RE::Actor& a_actor)
        {
            static const RE::BSFixedString stateVariable{ "iState" };
            std::int32_t value = -1;
            return a_actor.GetGraphVariableInt(stateVariable, value) ? value : -1;
        }

        enum class GraphSneak
        {
            kUnknown,
            kNotSneaking,
            kSneaking
        };

        struct GraphSneakReading
        {
            GraphSneak sneak{ GraphSneak::kUnknown };
            // Selector states, when any is active; otherwise the sneak variables.
            GraphStance::SelectorState selectors{ GraphStance::SelectorState::kNone };
        };

        constexpr const char* GetGraphSneakName(const GraphSneakReading& a_reading) noexcept
        {
            using GraphStance::SelectorState;
            switch (a_reading.selectors)
            {
                case SelectorState::kOff:
                    return "notSneaking/states";
                case SelectorState::kOn:
                    return "sneaking/states";
                case SelectorState::kMixed:
                    return "mixed/states";
                default:
                    break;
            }
            switch (a_reading.sneak)
            {
                case GraphSneak::kNotSneaking:
                    return "notSneaking/variables";
                case GraphSneak::kSneaking:
                    return "sneaking/variables";
                default:
                    return "unknown";
            }
        }

        constexpr const char* GetGraphMovingName(GraphStance::SelectorState a_selectors) noexcept
        {
            using GraphStance::SelectorState;
            switch (a_selectors)
            {
                case SelectorState::kOff:
                    return "standing";
                case SelectorState::kOn:
                    return "moving";
                case SelectorState::kMixed:
                    return "mixed";
                default:
                    return "unknown";
            }
        }

        GraphSneakReading ReadGraphSneak(RE::Actor& a_actor, GraphStance::SelectorState a_selectors)
        {
            using GraphStance::SelectorState;
            switch (a_selectors)
            {
                case SelectorState::kOff:
                    return { .sneak = GraphSneak::kNotSneaking, .selectors = a_selectors };
                case SelectorState::kOn:
                    return { .sneak = GraphSneak::kSneaking, .selectors = a_selectors };
                case SelectorState::kMixed:
                    // Selectors disagree while one stance's branch blends into another's.
                    return { .sneak = GraphSneak::kUnknown, .selectors = a_selectors };
                case SelectorState::kNone:
                    break;
            }

            // Fallback for graphs without the vanilla selectors. A sneak branch keeps
            // these set while it blends out after SneakStop.
            static const RE::BSFixedString isSneakingVariable{ "IsSneaking" };
            bool isSneaking = false;
            const bool isSneakingAvailable = a_actor.GetGraphVariableBool(isSneakingVariable, isSneaking);
            const auto stateTag = GetGraphStateTag(a_actor);
            if ((isSneakingAvailable && isSneaking) || stateTag == kSneakGraphStateTag)
            {
                return { .sneak = GraphSneak::kSneaking };
            }
            return { .sneak = isSneakingAvailable && stateTag >= 0 ? GraphSneak::kNotSneaking : GraphSneak::kUnknown };
        }

        GraphSneakReading ReadGraphSneak(RE::Actor& a_actor)
        {
            return ReadGraphSneak(a_actor, GraphStance::ReadStanceSelectors(a_actor).sneak);
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
            auto* const animationState = &a_remote.animation;
            RE::BSAnimationGraphManager* graphBeforeRecovery = nullptr;
            {
                RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
                if (a_actor.GetAnimationGraphManager(graphManager) && graphManager)
                {
                    graphBeforeRecovery = graphManager.get();
                }
            }

            // TESHitEvent can arrive while animation work runs elsewhere.
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
            logs::debug(
                "Started actor animation-graph recovery: actor={:08X} desired={} detectedReaction={} observedHits={} "
                "graphReverted={} fallback3DReset={} previousGraph={} recovery={}",
                a_remote.handle.native_handle(),
                Multiplayer::GetMovementStateName(a_remote.animation.targetAnimationState.movementState),
                a_detectedHitReaction, a_observedHitEvents, graphReverted, !graphReverted,
                static_cast<const void*>(graphBeforeRecovery), recoveryCount);
        }

        void ApplyAuthoritativeWeaponPresentation(RE::Actor& a_actor)
        {
            PROFILE_FUNCTION();
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
            // WantToDraw and WantToSheathe only record an outstanding request, so keep
            // retrying until the matching transition is actually animating. The equip
            // branch picks standing or moving from iSyncIdleLocomotion.
            if (weaponTransitionPending && !transitionWasAnimating)
            {
                a_actor.DrawWeaponMagicHands(authoritativeWeaponDrawn);
            }
        }

        float GetGraphSpeedForState(
            RE::Actor& a_actor, float a_sampleSpeed, float a_heldSpeed, Multiplayer::MovementState a_state)
        {
            if (a_state == Multiplayer::MovementState::kIdle || a_state == Multiplayer::MovementState::kSneakingIdle)
            {
                return 0.0F;
            }

            if (std::isfinite(a_sampleSpeed) && a_sampleSpeed > 0.0F)
            {
                return a_sampleSpeed;
            }
            if (a_heldSpeed > 0.0F)
            {
                return a_heldSpeed;
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
            const auto& target = a_state.targetAnimationState;
            return IsMoving(target.movementState) || target.jumping
                || (!target.weaponDrawn && !a_state.weaponDefaultStateDispatched);
        }

        // The graph probe dumps every active node and variable, so release builds
        // compile it out.
#ifndef NDEBUG
        void LogGraphProbe(RE::Actor& a_actor, RemoteActor& a_remote, const char* a_reason)
        {
            auto& state = a_remote.animation;
            logs::debug("Animation graph probe: actor={:08X} reason={} latched={}/{} weaponDrawn={} iState={} {}",
                a_remote.handle.native_handle(), a_reason,
                Multiplayer::GetMovementStateName(state.lastDispatched.movementState),
                state.lastDispatched.jumping ? "jumping" : "grounded", state.lastDispatched.weaponDrawn,
                GetGraphStateTag(a_actor), GraphProbe::Describe(a_actor, state.graphProbeSnapshot));
        }

        void ArmGraphProbe(State& a_state, const char* a_reason)
        {
            a_state.graphProbeReason = a_reason;
            a_state.graphProbeElapsed = 0.0F;
        }

        void AdvanceGraphProbe(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
        {
            auto& state = a_remote.animation;
            if (state.hasDispatched && state.lastDispatched != state.graphProbeState)
            {
                state.graphProbeState = state.lastDispatched;
                ArmGraphProbe(state, "state");
            }
            if (!state.graphProbeReason)
            {
                return;
            }
            if (!IsAnimationStateSettled(a_remote) || state.attackPlaybackPending || state.locomotionReassertReason
                || IsHandPresentationPlaying(a_remote) || IsWeaponTransitionUnsettled(state)
                || state.graphRecoveryInProgress)
            {
                state.graphProbeElapsed = 0.0F;
                return;
            }
            state.graphProbeElapsed += FrameTime::Clamp(a_deltaTime);
            if (state.graphProbeElapsed < kGraphProbeSettleSeconds)
            {
                return;
            }
            LogGraphProbe(a_actor, a_remote, state.graphProbeReason);
            state.graphProbeReason = nullptr;
        }
#else
        void LogGraphProbe(RE::Actor&, RemoteActor&, const char*) {}
        void ArmGraphProbe(State&, const char*) {}
        void AdvanceGraphProbe(RE::Actor&, RemoteActor&, float) {}
#endif

        constexpr bool IsBowAttackState(RE::ATTACK_STATE_ENUM a_state) noexcept
        {
            return a_state >= RE::ATTACK_STATE_ENUM::kBowDraw && a_state <= RE::ATTACK_STATE_ENUM::kBowFollowThrough;
        }

        static_assert(IsBowAttackState(RE::ATTACK_STATE_ENUM::kBowDrawn));
        static_assert(!IsBowAttackState(RE::ATTACK_STATE_ENUM::kFollowThrough));

        void ResetWeaponTransition(State& a_state, bool a_pending)
        {
            a_state.weaponLocomotionReassertPending = a_pending;
            a_state.weaponTransitionCompletionObserved = false;
            a_state.weaponTransitionWaitElapsed = 0.0F;
            a_state.weaponTransitionAnimationObserved = false;
            a_state.weaponTransitionExitForced = false;
            a_state.weaponDefaultStateDispatched = false;
            a_state.weaponReplayBaseDispatched = false;
            a_state.weaponRestoredWithoutCompletion = false;
        }

        void SetLandingCompletionPending(State& a_state, bool a_pending)
        {
            a_state.landingCompletionPending = a_pending;
            a_state.landingCompletionWaitFrames = 0;
            a_state.landingCompletionWaitElapsed = 0.0F;
        }

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

        bool CanStartHandPresentation(const RemoteActor& a_remote, RE::Actor& a_actor)
        {
            const auto& state = a_remote.animation;
            if (!a_remote.ready || !a_remote.animationGraphHolder || !IsAnimationStateSettled(a_remote)
                || state.attackPlaybackPending || state.locomotionReassertReason || state.bowReleasePending
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

        void AdvanceSwimMoveStart(RE::Actor& a_actor, RemoteActor& a_remote, State& a_state,
            const RE::ActorState* a_actorState, float a_deltaTime)
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

            a_state.swimMoveStartElapsed += FrameTime::Clamp(a_deltaTime);
            // SwimStart is evaluated on the graph's next update. Counting passes
            // rather than time is intended: the swim branch must have been entered.
            if (++a_state.swimMoveStartWaitPasses < 2)
            {
                return;
            }

            static const RE::BSFixedString moveStartEvent{ "moveStart" };
            ++a_state.swimMoveStartAttempts;
            const bool alreadyMoving
                = GraphStance::ReadStanceSelectors(a_actor).moving == GraphStance::SelectorState::kOn;
            const bool accepted = alreadyMoving || SendNetworkAnimationEvent(a_remote, moveStartEvent);
            if (!accepted && a_state.swimMoveStartElapsed < kSwimMoveStartFallbackSeconds)
            {
                return;
            }
            a_state.swimMoveStartPending = false;
            logs::debug(
                "Animation sync selected moving swim state: actor={:08X} state={} accepted={} alreadyMoving={} "
                "attempts={} elapsed={:.3f}",
                a_remote.handle.native_handle(),
                Multiplayer::GetMovementStateName(a_state.targetAnimationState.movementState), accepted, alreadyMoving,
                a_state.swimMoveStartAttempts, a_state.swimMoveStartElapsed);
        }

        // Skyrim's event result is not an acknowledgement, so a sneak or move edge
        // that ran out of retries is latched as taken whether or not the graph took
        // it, and a branch entered standing leaves the graph standing under a moving
        // latch. Later dispatches only send an edge when the latched state changes.
        void ReconcileGraphStance(RE::Actor& a_actor, RemoteActor& a_remote, State& a_state, float a_deltaTime)
        {
            const auto& target = a_state.targetAnimationState;
            const auto* actorState = a_actor.AsActorState();
            const bool reconcilable = a_state.hasDispatched && a_state.lastDispatched == target
                && IsAnimationStateSettled(a_remote) && !target.jumping && !IsSprinting(target.movementState)
                && !a_state.locomotionReassertReason && !a_state.attackPlaybackPending
                && !a_state.graphRecoveryInProgress && !IsWeaponTransitionUnsettled(a_state)
                && !IsBowOrCastPlaying(a_state) && !IsBlockPlaying(a_state)
                && !(actorState && actorState->IsSwimming());
            if (!reconcilable)
            {
                a_state.graphStanceSampleElapsed = 0.0F;
                a_state.graphSneakMismatchElapsed = 0.0F;
                a_state.graphStandingMismatchElapsed = 0.0F;
                return;
            }
            a_state.graphStanceSampleElapsed += FrameTime::Clamp(a_deltaTime);
            if (a_state.graphStanceSampleElapsed < kGraphStanceSampleSeconds)
            {
                return;
            }
            const float sampledSeconds = a_state.graphStanceSampleElapsed;
            a_state.graphStanceSampleElapsed = 0.0F;
            const auto selectors = GraphStance::ReadStanceSelectors(a_actor);

            const auto graphSneak = ReadGraphSneak(a_actor, selectors.sneak);
            const bool targetSneaking = IsSneaking(target.movementState);
            if (graphSneak.sneak != (targetSneaking ? GraphSneak::kNotSneaking : GraphSneak::kSneaking))
            {
                a_state.graphSneakMismatchElapsed = 0.0F;
            }
            else
            {
                a_state.graphSneakMismatchElapsed += sampledSeconds;
                if (a_state.graphSneakMismatchElapsed >= kGraphSneakMismatchSeconds
                    && a_state.graphSneakCorrections < kMaximumGraphStanceCorrections)
                {
                    a_state.graphSneakMismatchElapsed = 0.0F;
                    a_state.graphStandingMismatchElapsed = 0.0F;
                    ++a_state.graphSneakCorrections;
                    // Latch what the graph shows, so the next dispatch sends the missing
                    // edge through the normal ordering, retries, and post-sneak moveStart.
                    a_state.lastDispatched.movementState = ToggleSneak(target.movementState);
                    logs::debug(
                        "Animation sync graph sneak disagrees with the latched state; reasserting: actor={:08X} "
                        "state={} graphSneak={} iState={} correction={}/{}",
                        a_remote.handle.native_handle(), Multiplayer::GetMovementStateName(target.movementState),
                        GetGraphSneakName(graphSneak), GetGraphStateTag(a_actor), a_state.graphSneakCorrections,
                        kMaximumGraphStanceCorrections);
                    return;
                }
            }

            // Only standing under a moving target is repaired: the graph rejected
            // moveStop once Speed was zeroed, and the receiver writes Speed 0 every
            // pass while the target is idle.
            if (!IsMoving(target.movementState) || selectors.moving != GraphStance::SelectorState::kOff)
            {
                a_state.graphStandingMismatchElapsed = 0.0F;
                return;
            }
            a_state.graphStandingMismatchElapsed += sampledSeconds;
            if (a_state.graphStandingMismatchElapsed < kGraphStandingMismatchSeconds
                || a_state.graphStandingCorrections >= kMaximumGraphStanceCorrections)
            {
                return;
            }
            a_state.graphStandingMismatchElapsed = 0.0F;
            ++a_state.graphStandingCorrections;
            a_state.lastDispatched.movementState = ToStanding(target.movementState);
            logs::debug(
                "Animation sync graph stands while the latched state moves; reasserting: actor={:08X} state={} "
                "graphMoving={} graphSneak={} iState={} correction={}/{}",
                a_remote.handle.native_handle(), Multiplayer::GetMovementStateName(target.movementState),
                GetGraphMovingName(selectors.moving), GetGraphSneakName(graphSneak), GetGraphStateTag(a_actor),
                a_state.graphStandingCorrections, kMaximumGraphStanceCorrections);
        }

        void ResetGraphDispatch(RE::Actor& a_actor, RemoteActor& a_remote)
        {
            AnimationFirewall::Disable(a_remote);
            ActorRegistry::SetLandingCompletionObservation(a_actor, false);
            InvalidateAnimationDispatch(a_remote.animation);
        }

        void ResetUnavailableGraph(RE::Actor& a_actor, RemoteActor& a_remote, const char* a_reason)
        {
            if (a_remote.animation.hasDispatched || a_remote.animation.graphManager)
            {
                logs::debug("Animation sync reset graph state: actor={:08X} reason={}", a_remote.handle.native_handle(),
                    a_reason);
            }
            ResetGraphDispatch(a_actor, a_remote);
        }
    }

    Multiplayer::AnimationState GetTargetState(const RemoteActor& a_remote)
    {
        return a_remote.animation.targetAnimationState;
    }

    void SetAnimationState(RemoteActor& a_remote, Multiplayer::AnimationState a_animationState)
    {
        if (a_remote.form != Multiplayer::Shapeshift::kNone && IsSneaking(a_animationState.movementState))
        {
            // The beast graphs take no SneakStart: a sneaking beast walks or runs as usual.
            a_animationState.movementState = ToggleSneak(a_animationState.movementState);
        }
        const auto previousAnimationState = a_remote.animation.targetAnimationState;
        a_remote.animation.targetAnimationState = a_animationState;
        ActorRegistry::SetAuthoritativeJumpState(a_remote, a_animationState.jumping);
        ActorRegistry::SetAuthoritativeDirection(a_remote);
        ActorRegistry::SetAuthoritativeWeaponDrawnState(a_remote, a_animationState.weaponDrawn);
        RequestAnimationUpdate(a_remote.animation);
        if (previousAnimationState != a_animationState)
        {
            a_remote.animation.graphSneakCorrections = 0;
            a_remote.animation.graphStandingCorrections = 0;
            logs::debug("Animation sync updated actor request: actor={:08X} revision={} previous={}/{} desired={}/{}",
                a_remote.handle.native_handle(), static_cast<unsigned long long>(a_remote.animation.requestedRevision),
                Multiplayer::GetMovementStateName(previousAnimationState.movementState),
                previousAnimationState.jumping ? "jumping" : "grounded",
                Multiplayer::GetMovementStateName(a_animationState.movementState),
                a_animationState.jumping ? "jumping" : "grounded");
        }
    }

    bool SendNetworkAnimationEvent(RemoteActor& a_remote, const RE::BSFixedString& a_eventName)
    {
        PROFILE_FUNCTION();
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
        return !a_remote.animation.landingCompletionPending && IsAnimationStateSettledExceptLanding(a_remote);
    }

    bool IsAnimationStateSettledExceptLanding(const RemoteActor& a_remote)
    {
        return a_remote.animation.processedRevision == a_remote.animation.requestedRevision;
    }

    bool ShouldHoldTranslation(RemoteActor& a_remote, float a_deltaTime)
    {
        auto& state = a_remote.animation;
        if (IsAnimationStateSettledExceptLanding(a_remote))
        {
            state.unsettledTranslationHoldElapsed = 0.0F;
            state.unsettledTranslationHoldReported = false;
            return false;
        }

        state.unsettledTranslationHoldElapsed += FrameTime::Clamp(a_deltaTime);
        if (state.unsettledTranslationHoldElapsed < kUnsettledTranslationHoldSeconds)
        {
            return true;
        }
        if (!state.unsettledTranslationHoldReported)
        {
            state.unsettledTranslationHoldReported = true;
            logs::debug(
                "Animation sync resumed translation while its locomotion dispatch is unsettled: actor={:08X} "
                "dispatched={} heldSeconds={:.3f} rejectedEventAttempts={}",
                a_remote.handle.native_handle(), Multiplayer::GetMovementStateName(state.lastDispatched.movementState),
                state.unsettledTranslationHoldElapsed, state.rejectedEventAttempts);
            if (auto actor = a_remote.handle.get())
            {
                LogGraphProbe(*actor, a_remote, "unsettled-translation");
            }
        }
        return false;
    }

    bool CanDispatchAttack(const RemoteActor& a_remote, const Network::Attack& a_attack)
    {
        if (!GameThread::Require("CanDispatchAttack") || !a_remote.ready || !a_remote.animationGraphHolder
            || !IsAnimationStateSettled(a_remote) || a_remote.animation.attackPlaybackPending
            || a_remote.animation.locomotionReassertReason || IsBowOrCastPlaying(a_remote.animation) ||
            // Only a bash plays from a raised block; other attacks wait until it is lowered.
            (a_attack.action != Network::AttackAction::kBash && IsBlockPlaying(a_remote.animation)) ||
            // The reassert flag stays armed while idle after a completed draw, so only a
            // transition that is still unsettled may hold the attack.
            IsWeaponTransitionUnsettled(a_remote.animation) || a_remote.animation.graphRecoveryInProgress
            || a_remote.animation.targetAnimationState.jumping || !a_remote.animation.targetAnimationState.weaponDrawn)
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
            && a_remote.animation.graphManager == graphManager.get();
    }

    bool CanChainAttack(const RemoteActor& a_remote, const Network::Attack& a_attack)
    {
        if (!GameThread::Require("CanChainAttack") || !a_remote.ready || a_attack.action == Network::AttackAction::kBash
            || !a_remote.animationGraphHolder || !IsAnimationStateSettled(a_remote)
            || !a_remote.animation.attackPlaybackPending || !a_remote.animation.attackPlaybackObserved
            || a_remote.animation.attackChainDispatched || a_remote.animation.graphRecoveryInProgress
            || a_remote.animation.targetAnimationState.jumping || !a_remote.animation.targetAnimationState.weaponDrawn)
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
            && a_remote.animation.graphManager == graphManager.get();
    }

    bool CanPowerUpAttack(const RemoteActor& a_remote, const Network::Attack& a_attack)
    {
        if (!GameThread::Require("CanPowerUpAttack") || !a_remote.ready || !a_attack.power
            || a_attack.action == Network::AttackAction::kBash || !a_remote.animationGraphHolder
            || !IsAnimationStateSettled(a_remote)
            || !a_remote.animation.attackPlaybackPending
            // The actor's attack state is not waited for: a remote actor reports it
            // well into the swing, which showed most of the plain swing before the
            // power attack. A graph not yet in its attack state rejects the event
            // and the action is retried.
            || a_remote.animation.attackPlaybackBash || a_remote.animation.attackPlaybackPower
            || a_remote.animation.attackPlaybackAction != a_attack.action || a_remote.animation.graphRecoveryInProgress
            || a_remote.animation.targetAnimationState.jumping || !a_remote.animation.targetAnimationState.weaponDrawn)
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
            && a_remote.animation.graphManager == graphManager.get();
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
        // A rejected chain only means the combo window does not take it.
        const bool powerFellBack
            = !accepted && a_attack.power && startsNewAttack && SendNetworkAnimationEvent(a_remote, plainEvent);
        accepted = accepted || powerFellBack;
        // The dual-wield branch requires two one-handed weapons in the actor's hand types.
        const bool dualFellBack = !accepted && a_attack.action == Network::AttackAction::kDual
            && (!a_attack.power || startsNewAttack) && SendNetworkAnimationEvent(a_remote, attackStartEvent);
        accepted = accepted || dualFellBack;
        actorFlags.set(RE::Actor::BOOL_FLAGS::kAttackingDisabled);
        if (!accepted)
        {
            a_remote.animation.attackChainRejections += chaining ? 1 : 0;
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

        a_remote.animation.attackPlaybackElapsed = 0.0F;
        a_remote.animation.attackPlaybackPower = a_attack.power && !powerFellBack;
        if (poweringUp)
        {
            // The power attack plays on in the attack state. Its own attack state is
            // observed again, so the transition cannot end ownership early.
            a_remote.animation.attackPlaybackObserved = false;
            return true;
        }
        a_remote.animation.attackPlaybackAction = a_attack.action;
        if (chaining)
        {
            // The chained swing starts on a later update, or only after attackWinStart;
            // until then the actor state still reports the hit or the combo window.
            a_remote.animation.attackChainDispatched = true;
            ++a_remote.animation.attackChainCount;
            return true;
        }
        a_remote.animation.attackPlaybackPending = true;
        a_remote.animation.attackPlaybackBash = a_attack.action == Network::AttackAction::kBash;
        a_remote.animation.attackPlaybackObserved = false;
        a_remote.animation.attackChainDispatched = false;
        a_remote.animation.attackChainCount = 0;
        a_remote.animation.attackChainRejections = 0;
        a_remote.animation.locomotionReassertReason = nullptr;
        const auto* actorState = actorPtr->AsActorState();
        a_remote.animation.attackPlaybackObserved
            = actorState && actorState->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
        return true;
    }

    void OwnDispatchedAttack(RemoteActor& a_remote, const RE::Actor& a_actor)
    {
        auto& state = a_remote.animation;
        state.attackPlaybackPending = true;
        state.attackPlaybackElapsed = 0.0F;
        state.attackPlaybackPower = false;
        state.attackPlaybackAction = Network::AttackAction::kNormal;
        state.attackPlaybackBash = false;
        state.attackChainDispatched = false;
        state.attackChainCount = 0;
        state.attackChainRejections = 0;
        state.locomotionReassertReason = nullptr;
        const auto* actorState = a_actor.AsActorState();
        state.attackPlaybackObserved = actorState && actorState->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
    }

    bool IsAnimationActionPlaying(const RemoteActor& a_remote)
    {
        return a_remote.animation.attackPlaybackPending;
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
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
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
            logs::debug("Animation sync completed bow shot ownership: actor={:08X} elapsed={:.3f} timedOut={}",
                a_remote.handle.native_handle(), state.bowPhaseElapsed, timedOut);
            state.bowReleasePending = false;
            state.bowPhaseElapsed = 0.0F;
            state.locomotionReassertReason = "bow-shot";
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
            // An early release is rejected until the draw can take it.
            const bool released = SendHandPresentationEvent(a_remote, actor, attackReleaseEvent);
            const bool releaseTimedOut = state.bowReleaseRequestElapsed >= kBowReleaseFallbackSeconds;
            if (!released && !releaseTimedOut)
            {
                return;
            }
            const bool cancelled = !released
                && (SendHandPresentationEvent(a_remote, actor, attackStopEvent)
                    || SendHandPresentationEvent(a_remote, actor, bowResetEvent));
            logs::debug(
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
        state.bowPhaseElapsed = 0.0F;
        state.bowReleaseRequestElapsed = 0.0F;
        // A release that is already due goes out on the next pass.
        state.bowEventRetryElapsed = kMinimumEventRetrySeconds;
        logs::debug("Animation sync started bow draw: actor={:08X}", a_remote.handle.native_handle());
    }

    void ApplySpellCast(RemoteActor& a_remote, bool a_leftHand, bool a_casting, bool a_mayStart, float a_deltaTime)
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
        const std::size_t hand = a_leftHand ? 1 : 0;
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
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
            // The last cast to stop also waits for the graph to leave the cast branch,
            // so the locomotion replay below reaches the spell-ready locomotion.
            bool graphCasting = false;
            actor.GetGraphVariableBool(castingVariable, graphCasting);
            const std::size_t otherHand = 1 - hand;
            const bool lastCast = !state.castHeld[otherHand] && !state.castStopping[otherHand];
            const auto graphStateTag = GetGraphStateTag(actor);
            const bool graphLeftCast = !lastCast || graphStateTag != kCastGraphStateTag;
            const bool idle = casterState == RE::MagicCaster::State::kNone && !graphCasting && graphLeftCast;
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
                state.locomotionReassertReason = "cast";
            }
            logs::debug(
                "Animation sync stopped spell cast: actor={:08X} hand={} idle={} stopSeconds={:.3f} "
                "reassertedLocomotion={} iState={}",
                a_remote.handle.native_handle(), a_leftHand ? "left" : "right", idle, stopSeconds, presentationEnded,
                graphStateTag);
            return;
        }

        if (state.castHeld[hand])
        {
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
        state.castStopRequestElapsed[hand] = 0.0F;
        logs::debug("Animation sync started spell cast: actor={:08X} hand={} spell={:08X}",
            a_remote.handle.native_handle(), a_leftHand ? "left" : "right", spell ? spell->GetFormID() : 0);
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
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        state.blockEventRetryElapsed = (std::min)(state.blockEventRetryElapsed + deltaTime, kMinimumEventRetrySeconds);

        if (state.blockStopping)
        {
            state.blockStopElapsed += deltaTime;
            bool graphBlocking = false;
            const bool graphBlockingAvailable = actor.GetGraphVariableBool(isBlockingVariable, graphBlocking);
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
            logs::debug("Animation sync lowered block: actor={:08X} lowered={} forcedInstant={} stopSeconds={:.3f}",
                a_remote.handle.native_handle(), lowered, forced, state.blockStopElapsed);
            state.blockStopping = false;
            state.blockStopAccepted = false;
            state.blockStopElapsed = 0.0F;
            state.locomotionReassertReason = "block";
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
            logs::debug("Animation sync requested block stop: actor={:08X} accepted={}",
                a_remote.handle.native_handle(), state.blockStopAccepted);
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
        logs::debug("Animation sync raised block: actor={:08X}", a_remote.handle.native_handle());
    }

    bool IsHandPresentationPlaying(const RemoteActor& a_remote)
    {
        return IsBowOrCastPlaying(a_remote.animation) || IsBlockPlaying(a_remote.animation);
    }

    bool IsWeaponTransitionPlaying(const RemoteActor& a_remote)
    {
        return IsWeaponTransitionClipPending(a_remote.animation);
    }

    void ApplyRiderWeaponState(RE::Actor& a_actor, RemoteActor& a_remote, bool a_weaponDrawn)
    {
        const auto* actorState = a_actor.AsActorState();
        const auto weaponState = actorState ? actorState->GetWeaponState() : RE::WEAPON_STATE::kSheathed;
        auto& target = a_remote.animation.targetAnimationState;
        if (target.weaponDrawn != a_weaponDrawn)
        {
            target.weaponDrawn = a_weaponDrawn;
            ActorRegistry::SetAuthoritativeWeaponDrawnState(a_remote, a_weaponDrawn);
            logs::debug("Remote rider weapon state changed: actor={:08X} weaponDrawn={} weaponState={}",
                a_remote.handle.native_handle(), a_weaponDrawn,
                static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(weaponState));
        }
        if (!actorState)
        {
            return;
        }
        const bool transitionPending
            = a_weaponDrawn ? weaponState != RE::WEAPON_STATE::kDrawn : weaponState != RE::WEAPON_STATE::kSheathed;
        // As on foot, the request is only recorded until the transition animates.
        if (transitionPending && !IsWeaponTransitionAnimating(weaponState, a_weaponDrawn))
        {
            a_actor.DrawWeaponMagicHands(a_weaponDrawn);
        }
    }

    void RequestAnimationUpdate(State& a_state)
    {
        ++a_state.requestedRevision;
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
        a_state.graphStanceSampleElapsed = 0.0F;
        a_state.graphSneakMismatchElapsed = 0.0F;
        a_state.graphStandingMismatchElapsed = 0.0F;
        a_state.graphSneakCorrections = 0;
        a_state.graphStandingCorrections = 0;
        SetLandingCompletionPending(a_state, false);
        a_state.locomotionReassertReason = nullptr;
        ResetWeaponTransition(a_state, false);
        a_state.attackPlaybackPending = false;
        a_state.attackPlaybackObserved = false;
        a_state.attackPlaybackState = RE::ATTACK_STATE_ENUM::kNone;
        a_state.attackChainDispatched = false;
        a_state.attackChainCount = 0;
        a_state.attackChainRejections = 0;
        a_state.attackPlaybackPower = false;
        a_state.attackPlaybackElapsed = 0.0F;
        ResetHandPresentation(a_state);
        a_state.processedRevision = 0;
        a_state.graphManager = nullptr;
        a_state.landingEventSinkGraph = nullptr;
    }

    void AdvanceAttackPlayback(RE::Actor& a_actor, State& a_state, float a_deltaTime)
    {
        if (!a_state.attackPlaybackPending)
        {
            return;
        }

        a_state.attackPlaybackElapsed += FrameTime::Clamp(a_deltaTime);
        const auto* actorState = a_actor.AsActorState();
        const auto attackState = actorState ? actorState->GetAttackState() : RE::ATTACK_STATE_ENUM::kNone;
        const bool attacking = attackState != RE::ATTACK_STATE_ENUM::kNone;
        a_state.attackPlaybackObserved = a_state.attackPlaybackObserved || attacking;
        if (attackState != a_state.attackPlaybackState)
        {
            logs::debug(
                "Animation sync remote attack state changed: actor={:08X} state={} previousState={} elapsed={:.3f} "
                "chainDispatched={} chainRejections={}",
                a_actor.GetHandle().native_handle(),
                static_cast<std::underlying_type_t<RE::ATTACK_STATE_ENUM>>(attackState),
                static_cast<std::underlying_type_t<RE::ATTACK_STATE_ENUM>>(a_state.attackPlaybackState),
                a_state.attackPlaybackElapsed, a_state.attackChainDispatched, a_state.attackChainRejections);
            a_state.attackPlaybackState = attackState;
        }
        // A chained swing writes kSwing on its weaponSwing annotation.
        if (a_state.attackChainDispatched
            && (attackState == RE::ATTACK_STATE_ENUM::kSwing || attackState == RE::ATTACK_STATE_ENUM::kDraw))
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

        logs::debug(
            "Animation sync completed attack ownership: actor={:08X} bash={} bashExited={} observedPlayback={} "
            "elapsed={:.3f} startTimedOut={} playbackTimedOut={} chained={} chainRejections={} chainPending={}",
            a_actor.GetHandle().native_handle(), a_state.attackPlaybackBash, bashExited, a_state.attackPlaybackObserved,
            a_state.attackPlaybackElapsed, startTimedOut, playbackTimedOut, a_state.attackChainCount,
            a_state.attackChainRejections, a_state.attackChainDispatched);
        a_state.attackPlaybackPending = false;
        a_state.attackPlaybackObserved = false;
        a_state.attackPlaybackState = RE::ATTACK_STATE_ENUM::kNone;
        a_state.attackChainDispatched = false;
        a_state.attackChainCount = 0;
        a_state.attackChainRejections = 0;
        a_state.attackPlaybackPower = false;
        a_state.attackPlaybackElapsed = 0.0F;
        // Native attacks normally return to locomotion themselves. The explicit
        // replay keeps the drawn stance; a full graph reset would start another
        // equip lifecycle.
        a_state.locomotionReassertReason = "attack";
    }

    bool RecoverAuthoritativeAnimation(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
    {
        PROFILE_FUNCTION();
        auto* const animationState = &a_remote.animation;
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

        const float deltaTime = FrameTime::Clamp(a_deltaTime);
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
                logs::debug(
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
        logs::debug(
            "Recovered actor animation graph is stable; replaying authoritative locomotion: actor={:08X} desired={} "
            "graphReplaced={} fallback3DReset={} recovery={}",
            a_remote.handle.native_handle(),
            Multiplayer::GetMovementStateName(a_remote.animation.targetAnimationState.movementState), graphWasReplaced,
            used3DReset, animationState->recoveryCount);
        return false;
    }

    void HoldIdleTurnState(RE::Actor& a_actor, RemoteActor& a_remote)
    {
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
        auto& state = a_remote.animation;
        ++state.turnStateOverrides;
        if (ActorConstants::ShouldLogOccurrence(state.turnStateOverrides))
        {
            logs::debug(
                "Animation sync held idle turn state: actor={:08X} localTurnState={} accepted={} turnStop={} count={}",
                a_remote.handle.native_handle(), syncTurnState, accepted, turnStopAccepted, state.turnStateOverrides);
        }
    }

    void ApplyAnimationUpdate(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
    {
        PROFILE_FUNCTION();
        auto* const animationState = &a_remote.animation;
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

        const auto actorHandle = a_actor.GetHandle();
        if (!a_actor.Is3DLoaded())
        {
            ResetUnavailableGraph(a_actor, a_remote, "actor-or-3d-unavailable");
            return;
        }

        auto* actor = &a_actor;
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
        if (!actor->GetAnimationGraphManager(graphManager) || !graphManager)
        {
            ResetUnavailableGraph(a_actor, a_remote, "graph-unavailable");
            return;
        }
        if (animationState->graphManager != graphManager.get())
        {
            logs::debug("Animation sync detected {} graph: actor={:08X}; next state will be fully asserted",
                animationState->graphManager ? "replacement" : "initial", actorHandle.native_handle());
            ResetGraphDispatch(a_actor, a_remote);
            animationState->graphManager = graphManager.get();
        }
        bool landingEventSinkRegistered = animationState->landingEventSinkGraph == graphManager.get();
        if (!landingEventSinkRegistered)
        {
            landingEventSinkRegistered = AnimationFirewall::RegisterGraphOutputEventSink(*graphManager);
            if (landingEventSinkRegistered)
            {
                animationState->landingEventSinkGraph = graphManager.get();
                logs::debug("Registered actor animation output-event sink: actor={:08X}", actorHandle.native_handle());
            }
        }

        if (animationState->landingCompletionPending && animationState->targetAnimationState.jumping)
        {
            // A consecutive takeoff leaves the landing branch directly, as the
            // local player's graph does. Waiting for JumpLandEnd would hold the
            // takeoff until the remote jump is already over. The jump dispatch
            // below closes the landing observation window; grounded locomotion is
            // reasserted after that jump's own landing.
            SetLandingCompletionPending(*animationState, false);
            logs::debug("Animation sync takeoff preempted pending landing completion: actor={:08X}",
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
            SetLandingCompletionPending(*animationState, false);
            ActorRegistry::SetLandingCompletionObservation(*actor, false);
            logs::debug(
                "Animation sync landing branch replaced by swimming: actor={:08X}", actorHandle.native_handle());
        }
        if (animationState->landingCompletionPending)
        {
            const auto completedLandings = ActorRegistry::ConsumeJumpLandEnds(a_remote);
            bool landingCompleted = completedLandings != 0;
            bool forcedCompletion = false;
            bool forcedEventAccepted = false;
            const auto landingWaitFrames = ++animationState->landingCompletionWaitFrames;
            animationState->landingCompletionWaitElapsed += FrameTime::Clamp(a_deltaTime);
            if (!landingCompleted && (landingWaitFrames == 1 || landingWaitFrames % 15 == 0))
            {
                const auto currentPosition = actor->GetPosition();
                bool graphJumping = false;
                const bool graphJumpAvailable = actor->GetGraphVariableBool(inJumpStateVariable, graphJumping);
                logs::debug(
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
                SetLandingCompletionPending(*animationState, false);
                ActorRegistry::SetLandingCompletionObservation(*actor, false);
                // JumpLandEnd selects the graph's exit branch, which can exit into idle. Keep the dispatch
                // history: forgetting it reads a drawn weapon as a new draw.
                animationState->locomotionReassertReason = "landing";
                animationState->failedDispatchAttempts = 0;
                animationState->rejectedEventAttempts = 0;
                animationState->rejectedEventElapsed = 0.0F;
                logs::debug(
                    "Animation sync completing landing branch and reasserting grounded locomotion: actor={:08X} "
                    "observedMarkers={} waitedFrames={} forced={} forcedEventAccepted={}",
                    actorHandle.native_handle(), completedLandings, completedAfterFrames, forcedCompletion,
                    forcedEventAccepted);
            }
        }
        else
        {
            // Drain a duplicate annotation while no landing owns it; the next takeoff also
            // clears this observation window.
            (void)ActorRegistry::ConsumeJumpLandEnds(a_remote);
        }

        const auto requestedRevision = animationState->requestedRevision;
        const auto authoritativeState = animationState->targetAnimationState;
        const auto sampleSpeed = a_remote.movement.animationSpeed;
        const auto sampleDirection = a_remote.movement.animationDirection;
        const bool authoritativeDispatchRequired
            = !animationState->hasDispatched || animationState->lastDispatched != authoritativeState;
        const auto previousState = animationState->lastDispatched;
        const bool hadPreviouslyDispatched = animationState->hasDispatched;
        const bool weaponDrawnChanged = authoritativeDispatchRequired
            && (hadPreviouslyDispatched ? previousState.weaponDrawn != authoritativeState.weaponDrawn
                                        : authoritativeState.weaponDrawn);
        if (weaponDrawnChanged)
        {
            ResetWeaponTransition(*animationState, true);
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
                ResetWeaponTransition(*animationState, true);
                animationState->weaponTransitionCompletionObserved = true;
            }
            logs::debug(
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
        const bool beastGraph = a_remote.form != Multiplayer::Shapeshift::kNone;
        if (beastGraph && animationState->weaponLocomotionReassertPending
            && !animationState->weaponTransitionCompletionObserved
            && weaponState == (authoritativeState.weaponDrawn ? RE::WEAPON_STATE::kDrawn : RE::WEAPON_STATE::kSheathed))
        {
            // The beast graphs have no equip or unequip clips with exit annotations
            // (WerewolfBehavior.hkx takes weapEquip straight into CombatIdle), so the
            // engine's weapon state completes the transition.
            animationState->weaponTransitionCompletionObserved = true;
        }
        // A takeoff interrupts a playing draw or sheathe while WEAPON_STATE keeps
        // reporting it, and the landing exit starts the clip over (tailUnequip and
        // BeginWeaponSheathe again, observed in game). A wait that ran through the
        // jump timed out mid-sheathe and left the weapon in hand.
        const bool jumpOwnsGraph = animationState->lastDispatched.jumping || animationState->landingCompletionPending;
        if (animationState->weaponLocomotionReassertPending
            && animationState->weaponTransitionWaitElapsed < kWeaponTransitionFallbackSeconds)
        {
            animationState->weaponTransitionWaitElapsed
                = jumpOwnsGraph && !animationState->weaponTransitionCompletionObserved
                ? 0.0F
                : animationState->weaponTransitionWaitElapsed + FrameTime::Clamp(a_deltaTime);
        }
        const bool weaponTransitionTimedOut
            = animationState->weaponTransitionWaitElapsed >= kWeaponTransitionFallbackSeconds;
        const bool weaponTransitionCompleted
            = animationState->weaponTransitionCompletionObserved || weaponTransitionTimedOut;
        if (beastGraph && weaponTransitionCompleted)
        {
            // No default state to force after a beast's sheathe.
            animationState->weaponDefaultStateDispatched = true;
        }
        if (animationState->weaponLocomotionReassertPending && weaponTransitionTimedOut
            && !animationState->weaponTransitionCompletionObserved && !animationState->weaponTransitionExitForced
            && !beastGraph)
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
                // Give an accepted exit edge one graph update before restoring locomotion.
                return;
            }
        }
        if (animationState->weaponLocomotionReassertPending && weaponTransitionCompleted
            && !authoritativeState.weaponDrawn && !animationState->weaponDefaultStateDispatched && !beastGraph)
        {
            // The unequip exit annotation finishes the clip, but does not guarantee
            // that the behavior graph is in a state that can consume locomotion. A
            // direct moveStart can report success while being ignored. Once ActorState
            // is also sheathed, force the graph's matching default state first and
            // replay authoritative locomotion on the next update.
            const bool defaultStateAccepted = AnimationFirewall::ForceDefaultGraphState(a_actor);
            animationState->weaponDefaultStateDispatched = true;
            logs::debug(
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
            animationState->locomotionReassertReason = "swim-stop";
            animationState->swimMoveStartPending = false;
        }
        AdvanceSwimMoveStart(*actor, a_remote, *animationState, actorState, a_deltaTime);
        const bool authoritativeLocomotionActive
            = IsMoving(authoritativeState.movementState) || authoritativeState.jumping;
        const bool reassertLocomotionAfterWeaponTransition
            = ShouldReassertAfterWeaponTransition(animationState->weaponLocomotionReassertPending,
                weaponTransitionCompleted, IsMoving(authoritativeState.movementState), authoritativeState.jumping);
        // A released graph may have left locomotion. Edges it already holds are
        // skipped, so this asserts only the missing ones.
        const bool reassertLocomotion = animationState->locomotionReassertReason
            && !animationState->attackPlaybackPending && !(actorState && actorState->IsSwimming());
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
        const bool zeroSpeedSample = isMoving && !(std::isfinite(sampleSpeed) && sampleSpeed > 0.0F);
        animationState->zeroSpeedSampleElapsed
            = zeroSpeedSample ? animationState->zeroSpeedSampleElapsed + FrameTime::Clamp(a_deltaTime) : 0.0F;
        const float heldGraphSpeed = animationState->zeroSpeedSampleElapsed < kZeroSpeedHoldSeconds
            ? animationState->publishedGraphSpeed
            : 0.0F;
        const float graphSpeed = GetGraphSpeedForState(*actor, sampleSpeed, heldGraphSpeed, state.movementState);
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
            // Graph construction can expose the manager one pass before its first
            // animation graph becomes available.
            return;
        }
        const bool moveRequired = !jumpLandRequired
            && ((dispatchRequired && (hadPreviousState ? IsMoving(previousState.movementState) != isMoving : isMoving))
                || ((replayingWeaponBase || reassertLocomotion) && isMoving));
        const bool sneakRequired = !jumpLandRequired
            && ((dispatchRequired
                    && (hadPreviousState ? IsSneaking(previousState.movementState) != isSneaking : isSneaking))
                || ((replayingWeaponBase || reassertLocomotion) && isSneaking));
        const bool eventDispatchRequired = dispatchRequired || reassertLocomotion;
        const bool sprintRequired = !jumpLandRequired
            && ((dispatchRequired
                    && (hadPreviousState ? IsSprinting(previousState.movementState) != isSprinting : isSprinting))
                || ((replayingWeaponBase || reassertLocomotion) && isSprinting));
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
        // Stop edges are evaluated against the old graph variables. Clearing Speed
        // or IsSprinting first makes the sprint and idle exits reject and leaves
        // the graph in its previous branch.
        const bool moveStopRequired = moveRequired && !isMoving;
        const bool sneakStopRequired = sneakRequired && !isSneaking;
        const bool sprintStopRequired = sprintRequired && !isSprinting;
        if (jumpRequired)
        {
            // JumpLand starts a timed landing branch whose graph output emits
            // JumpLandEnd later, often from an animation worker thread. Observe that
            // output only while the network-authoritative state is grounded.
            ActorRegistry::SetLandingCompletionObservation(*actor, jumpLandRequired);
        }
        // Read before any event of this pass. Skyrim rejects an edge into the state a
        // selector already holds, which would hold the dispatch for the whole retry
        // window.
        using GraphStance::SelectorState;
        const bool stanceSelectorsRead = stringsAvailable && (moveRequired || sneakRequired || sprintRequired);
        const auto stanceSelectors
            = stanceSelectorsRead ? GraphStance::ReadStanceSelectors(*actor) : GraphStance::StanceSelectors{};
        if ((stanceSelectors.moving == SelectorState::kMixed || stanceSelectors.sneak == SelectorState::kMixed
                || stanceSelectors.sprinting == SelectorState::kMixed)
            && ShouldReportEventRetry(animationState->rejectedEventAttempts + 1))
        {
            logs::debug("Animation sync stance selectors disagree: actor={:08X} desired={} selectors=[{}]",
                actorHandle.native_handle(), Multiplayer::GetMovementStateName(state.movementState),
                GraphStance::DescribeStanceSelectors(*actor));
        }
        const char* const graphMovingName = stanceSelectorsRead ? GetGraphMovingName(stanceSelectors.moving) : "unread";
        const bool moveAlreadyStopped = moveStopRequired && stanceSelectors.moving == SelectorState::kOff;
        bool moveAccepted = moveStopRequired && stringsAvailable
            ? moveAlreadyStopped || SendNetworkAnimationEvent(a_remote, moveStopEvent)
            : false;
        // Only the selector states count: the sneak variables also read as not sneaking
        // in branches without a sneak transition, such as weapon equip.
        const bool sneakAlreadyLeft = sneakStopRequired && stanceSelectors.sneak == SelectorState::kOff;
        bool sneakAccepted = sneakStopRequired && stringsAvailable
            ? sneakAlreadyLeft || SendNetworkAnimationEvent(a_remote, sneakStopEvent)
            : false;
        const bool sprintAlreadyStopped = sprintStopRequired && stanceSelectors.sprinting == SelectorState::kOff;
        bool sprintAccepted = sprintStopRequired && stringsAvailable
            ? sprintAlreadyStopped || SendNetworkAnimationEvent(a_remote, sprintStopEvent)
            : false;
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
                logs::debug(
                    "Animation sync graph dispatch: actor={:08X} revision={} previous={}/{} desired={}/{} "
                    "result=retrying-stop-events strings={} moveEvent={}/{} moveAlreadyStopped={} sneakEvent={}/{} "
                    "sneakAlreadyLeft={} sprintEvent={}/{} sprintAlreadyStopped={} jumpEvent=deferred/{} iState={} "
                    "graphMoving={} "
                    "graphSneak={} eventAttempt={}/{}",
                    actorHandle.native_handle(), static_cast<unsigned long long>(requestedRevision), previousStateLabel,
                    hadPreviousState && previousState.jumping ? "jumping" : "grounded",
                    Multiplayer::GetMovementStateName(state.movementState), state.jumping ? "jumping" : "grounded",
                    stringsAvailable, moveAccepted, moveStopRequired, moveAlreadyStopped, sneakAccepted,
                    sneakStopRequired, sneakAlreadyLeft, sprintAccepted, sprintStopRequired, sprintAlreadyStopped,
                    jumpLandRequired, GetGraphStateTag(*actor), graphMovingName,
                    GetGraphSneakName(ReadGraphSneak(*actor)), stopEventAttempt, kMaximumEventDispatchAttempts);
            }
            ++animationState->rejectedEventAttempts;
            animationState->rejectedEventElapsed += FrameTime::Clamp(a_deltaTime);
            return;
        }
        if (jumpLandRequired)
        {
            const auto positionBeforeLanding = actor->GetPosition();
            bool graphJumpingBeforeLanding = false;
            const bool graphJumpAvailableBeforeLanding
                = actor->GetGraphVariableBool(inJumpStateVariable, graphJumpingBeforeLanding);
            // Clear the interpolation backlog before changing the persistent graph state, or
            // the landing branch can begin while the actor is aloft.
            auto& movement = a_remote.movement;
            const auto landingPosition = movement.landingPosition.value_or(movement.targetPosition);
            movement.landingPosition.reset();
            actor->SetPosition(landingPosition, true);
            if (animationState->rejectedEventAttempts == 0)
            {
                logs::debug(
                    "Animation sync aligned landing transform: actor={:08X} current=({:.3f},{:.3f},{:.3f}) "
                    "target=({:.3f},{:.3f},{:.3f}) distance={:.1f} zError={:.3f} physicalJump={} physicalMidair={} "
                    "graphJumpAvailable={} graphJump={}",
                    actorHandle.native_handle(), positionBeforeLanding.x, positionBeforeLanding.y,
                    positionBeforeLanding.z, landingPosition.x, landingPosition.y, landingPosition.z,
                    positionBeforeLanding.GetDistance(landingPosition), landingPosition.z - positionBeforeLanding.z,
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
        bool syncSprintStateAccepted = false;
        bool syncIdleLocomotionAccepted = false;
        bool jumpVariableAccepted = false;
        if (strings)
        {
            PROFILE_SCOPE("ApplyAnimationUpdate graph variables");
            speedAccepted = actor->SetGraphVariableFloat(strings->speed, graphSpeed);
            sampledSpeedAccepted = actor->SetGraphVariableFloat(strings->speedSampled, graphSpeed);
            dampedSpeedAccepted = actor->SetGraphVariableFloat(speedDampedVariable, graphSpeed);
            // The source's graph keeps its last Direction while standing, and the
            // directional blend stays active then.
            directionAccepted = actor->SetGraphVariableFloat(strings->direction, sampleDirection);
            turnAccepted = actor->SetGraphVariableFloat(strings->turnDelta, 0.0F);
            sprintVariableAccepted = actor->SetGraphVariableBool(strings->isSprinting, isSprinting);
            // The vanilla locomotion, sneak, jump, and landing state machines pick
            // their sprint or non-sprint state from iSyncSprintState when entered,
            // and only some of them write it back. A sprinting takeoff leaves it at
            // sprint, so the moveStart after a running landing would re-enter
            // Sprint_State without a SprintStop.
            syncSprintStateAccepted = actor->SetGraphVariableInt(strings->iSyncSprintState, isSprinting ? 1 : 0);
            // Every vanilla standing/moving machine binds its start state to
            // iSyncIdleLocomotion (0 standing, 1 moving) and no graph node writes it,
            // so on a remote actor it stayed 0 and each branch entered while moving,
            // such as a landing exit, swim, sneak magic, an equip, or a block, bow, or
            // cast lower body, started standing.
            syncIdleLocomotionAccepted = actor->SetGraphVariableInt(strings->iSyncIdleLocomotion, isMoving ? 1 : 0);
            jumpVariableAccepted = actor->SetGraphVariableBool(inJumpStateVariable, state.jumping);
        }
        if (speedAccepted && graphSpeed != animationState->publishedGraphSpeed)
        {
            ActorRegistry::SetAuthoritativeSpeed(a_remote, graphSpeed);
            animationState->publishedGraphSpeed = graphSpeed;
        }
        const bool variablesAccepted = stringsAvailable && speedAccepted && sampledSpeedAccepted && dampedSpeedAccepted
            && directionAccepted && turnAccepted && sprintVariableAccepted && jumpVariableAccepted;
        if (animationState->hasDispatched && animationState->hasVariableUpdateResult
            && animationState->lastVariableUpdateAccepted != variablesAccepted)
        {
            logs::debug(
                "Animation sync graph variable refresh changed: actor={:08X} revision={} state={} result={} strings={} "
                "speed={} sampledSpeed={} dampedSpeed={} direction={} turn={} sprint={} jump={}",
                actorHandle.native_handle(), static_cast<unsigned long long>(requestedRevision),
                Multiplayer::GetMovementStateName(state.movementState), variablesAccepted ? "accepted" : "rejected",
                stringsAvailable, speedAccepted, sampledSpeedAccepted, dampedSpeedAccepted, directionAccepted,
                turnAccepted, sprintVariableAccepted, jumpVariableAccepted);
        }
        animationState->lastVariableUpdateAccepted = variablesAccepted;
        animationState->hasVariableUpdateResult = true;
        if (!eventDispatchRequired && variablesAccepted)
        {
            animationState->processedRevision = requestedRevision;
        }
        if (eventDispatchRequired)
        {
            // Takeoff, landing, and locomotion start edges are evaluated against
            // the new persistent graph variables.
            bool moveAlreadyStarted = false;
            if (moveRequired && isMoving && variablesAccepted)
            {
                moveAlreadyStarted = stanceSelectors.moving == SelectorState::kOn;
                moveAccepted = moveAlreadyStarted || SendNetworkAnimationEvent(a_remote, moveStartEvent);
            }
            bool sneakAlreadyEntered = false;
            if (sneakRequired && isSneaking && variablesAccepted)
            {
                // Only positive evidence counts: some sneak branches report neither
                // variable.
                sneakAlreadyEntered = ReadGraphSneak(*actor, stanceSelectors.sneak).sneak == GraphSneak::kSneaking;
                sneakAccepted = sneakAlreadyEntered || SendNetworkAnimationEvent(a_remote, sneakStartEvent);
            }
            bool sprintAlreadyStarted = false;
            if (sprintRequired && isSprinting && variablesAccepted)
            {
                // A landing exit or an accepted moveStart enters Sprint_State through
                // iSyncSprintState, which leaves SprintStart without an edge.
                sprintAlreadyStarted = stanceSelectors.sprinting == SelectorState::kOn;
                sprintAccepted = sprintAlreadyStarted || SendNetworkAnimationEvent(a_remote, sprintStartEvent)
                    || (moveRequired && moveAccepted && !moveAlreadyStarted && syncSprintStateAccepted);
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
            const bool retryingEvents = variablesAccepted && !stateProcessed;
            const bool reportDispatch = retryingEvents ? ShouldReportEventRetry(eventAttempt)
                                                       : variablesAccepted || ShouldReportEventRetry(variableAttempt);
            if (reportDispatch)
            {
                logs::debug(
                    "Animation sync graph dispatch: actor={:08X} revision={} previous={}/{} desired={}/{} result={} "
                    "eventsAccepted={} strings={} speedVariable={} sampledSpeedVariable={} dampedSpeedVariable={} "
                    "directionVariable={} turnVariable={} sprintVariable={} syncSprintVariable={} "
                    "syncIdleVariable={} jumpVariable={} "
                    "moveEvent={}/{} moveAlreadyStarted={} moveAlreadyStopped={} sneakEvent={}/{} "
                    "sneakAlreadyEntered={} sneakAlreadyLeft={} sprintEvent={}/{} sprintAlreadyStarted={} "
                    "sprintAlreadyStopped={} jumpEvent={}/{} graphSpeed={:.1f} "
                    "iState={} graphMoving={} graphSneak={} variableAttempt={} eventAttempt={}/{}",
                    actorHandle.native_handle(), static_cast<unsigned long long>(requestedRevision), previousStateLabel,
                    hadPreviousState && previousState.jumping ? "jumping" : "grounded",
                    Multiplayer::GetMovementStateName(state.movementState), state.jumping ? "jumping" : "grounded",
                    !variablesAccepted           ? "variables-rejected"
                        : eventsAccepted         ? "processed"
                        : eventAttemptsExhausted ? "processed-event-timeout"
                                                 : "retrying-events",
                    eventsAccepted, stringsAvailable, speedAccepted, sampledSpeedAccepted, dampedSpeedAccepted,
                    directionAccepted, turnAccepted, sprintVariableAccepted, syncSprintStateAccepted,
                    syncIdleLocomotionAccepted, jumpVariableAccepted, moveAccepted, moveRequired, moveAlreadyStarted,
                    moveAlreadyStopped, sneakAccepted, sneakRequired, sneakAlreadyEntered, sneakAlreadyLeft,
                    sprintAccepted, sprintRequired, sprintAlreadyStarted, sprintAlreadyStopped, jumpAccepted,
                    jumpRequired, graphSpeed, GetGraphStateTag(*actor), graphMovingName,
                    GetGraphSneakName(ReadGraphSneak(*actor)), variableAttempt, eventAttempt,
                    kMaximumEventDispatchAttempts);
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
                        logs::debug(
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
                        ResetWeaponTransition(*animationState, false);
                        animationState->weaponRestoredWithoutCompletion = !weaponCompletionWasObserved;
                        logs::debug(
                            "Animation sync restored state after weapon transition: actor={:08X} state={} jumping={} "
                            "weaponDrawn={} completionMarkerObserved={} weaponState={} transitionWaitSeconds={:.3f} "
                            "eventTimedOut={}",
                            actorHandle.native_handle(), Multiplayer::GetMovementStateName(state.movementState),
                            state.jumping, state.weaponDrawn, weaponCompletionWasObserved,
                            static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(weaponState),
                            weaponTransitionWaitElapsed, eventAttemptsExhausted);
                        ArmGraphProbe(*animationState, "weapon-replay");
                    }
                }
                if (reassertLocomotion)
                {
                    logs::debug("Animation sync reasserted locomotion: actor={:08X} reason={} state={}",
                        actorHandle.native_handle(), animationState->locomotionReassertReason,
                        Multiplayer::GetMovementStateName(state.movementState));
                    animationState->locomotionReassertReason = nullptr;
                    ArmGraphProbe(*animationState, "reassert");
                }
                animationState->failedDispatchAttempts = 0;
                animationState->rejectedEventAttempts = 0;
                animationState->rejectedEventElapsed = 0.0F;
                if (jumpLandRequired)
                {
                    SetLandingCompletionPending(*animationState, true);
                    logs::debug("Animation sync waiting for landing completion marker: actor={:08X} revision={}",
                        actorHandle.native_handle(), static_cast<unsigned long long>(requestedRevision));
                }
                else if (landingInWater)
                {
                    logs::debug("Animation sync skipped landing branch while swimming: actor={:08X} revision={}",
                        actorHandle.native_handle(), static_cast<unsigned long long>(requestedRevision));
                }
                animationState->processedRevision = requestedRevision;
            }
            else if (!variablesAccepted)
            {
                ++animationState->failedDispatchAttempts;
            }
            else
            {
                ++animationState->rejectedEventAttempts;
                animationState->rejectedEventElapsed += FrameTime::Clamp(a_deltaTime);
            }
        }

        animationState->directionCrossBlendElapsed += FrameTime::Clamp(a_deltaTime);
        if (animationState->directionCrossBlendPending)
        {
            // One graph update after CyclicFreeze, matching Skyrim's own spacing.
            animationState->directionCrossBlendPending = false;
            (void)SendNetworkAnimationEvent(a_remote, cyclicCrossBlendEvent);
        }
        else if (state.jumping || !variablesAccepted)
        {
            animationState->hasCrossBlendedDirection = false;
        }
        // Standing keeps the cross-blended direction: the directional
        // BSCyclicBlendTransitionGenerator stays active while the actor stands (observed in
        // game), so a start in another direction is a change like one while moving.
        else if (isMoving && !animationState->hasCrossBlendedDirection)
        {
            animationState->crossBlendedDirection = sampleDirection;
            animationState->hasCrossBlendedDirection = true;
        }
        else if (isMoving)
        {
            const float directionChange = std::abs(sampleDirection - animationState->crossBlendedDirection);
            const float cyclicDirectionChange = (std::min)(directionChange, 1.0F - directionChange);
            if (cyclicDirectionChange >= kDirectionCrossBlendThreshold
                && animationState->directionCrossBlendElapsed >= kDirectionCrossBlendIntervalSeconds)
            {
                (void)SendNetworkAnimationEvent(a_remote, cyclicFreezeEvent);
                animationState->crossBlendedDirection = sampleDirection;
                animationState->directionCrossBlendPending = true;
                animationState->directionCrossBlendElapsed = 0.0F;
            }
        }

        // Let the native transition start only after this checkpoint's persistent
        // speed and locomotion events have settled.
        if (IsAnimationStateSettled(a_remote))
        {
            ApplyAuthoritativeWeaponPresentation(*actor);
        }
        ReconcileGraphStance(*actor, a_remote, *animationState, a_deltaTime);
        AdvanceGraphProbe(*actor, a_remote, a_deltaTime);
    }

    bool IsFurnitureEntryBlocked(const RemoteActor& a_remote)
    {
        return a_remote.animation.attackPlaybackPending || IsHandPresentationPlaying(a_remote)
            || IsWeaponTransitionUnsettled(a_remote.animation) || a_remote.animation.graphRecoveryInProgress;
    }
}
