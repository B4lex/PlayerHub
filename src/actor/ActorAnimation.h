#pragma once

#include "actor/GraphProbe.h"
#include "protocol/MovementState.h"
#include "protocol/PlayerState.h"

#include <RE/Skyrim.h>

#include <array>
#include <cstdint>

struct RemoteActor;

namespace ActorAnimation
{
    struct State
    {
        Multiplayer::AnimationState targetAnimationState{};
        float crossBlendedDirection{ 0.0F };
        bool hasCrossBlendedDirection{ false };
        bool directionCrossBlendPending{ false };
        float directionCrossBlendElapsed{ 0.0F };
        std::uint64_t requestedRevision{ 0 };
        std::uint64_t processedRevision{ 0 };
        bool checkpointUpdatePending{ false };

        Multiplayer::AnimationState lastDispatched{};
        Multiplayer::AnimationState lastAttempted{};
        RE::BSAnimationGraphManager* graphManager{ nullptr };
        RE::BSAnimationGraphManager* landingEventSinkGraph{ nullptr };
        float publishedGraphSpeed{ 0.0F };
        float zeroSpeedSampleElapsed{ 0.0F };
        std::uint32_t failedDispatchAttempts{ 0 };
        std::uint32_t rejectedEventAttempts{ 0 };
        float rejectedEventElapsed{ 0.0F };
        float unsettledTranslationHoldElapsed{ 0.0F };
        bool unsettledTranslationHoldReported{ false };
        bool landingCompletionPending{ false };
        // Main-loop passes, only used to throttle logs.
        std::uint32_t landingCompletionWaitFrames{ 0 };
        float landingCompletionWaitElapsed{ 0.0F };
        // Set when a landing, attack, hand presentation, or swim releases the graph;
        // names the release for the log.
        const char* locomotionReassertReason{ nullptr };
        bool hasDispatched{ false };
        bool hasAttempted{ false };
        bool hasVariableUpdateResult{ false };
        // Weapon draw/sheathe can make Skyrim's graph leave its active locomotion branch.
        bool weaponLocomotionReassertPending{ false };
        bool weaponTransitionCompletionObserved{ false };
        float weaponTransitionWaitElapsed{ 0.0F };
        bool weaponTransitionAnimationObserved{ false };
        bool weaponTransitionExitForced{ false };
        bool weaponDefaultStateDispatched{ false };
        bool weaponReplayBaseDispatched{ false };
        bool weaponRestoredWithoutCompletion{ false };
        // A graph that took SwimStart in SwimIdle while moving leaves it only on
        // moveStart.
        bool swimMoveStartPending{ false };
        std::uint32_t swimMoveStartWaitPasses{ 0 };
        std::uint32_t swimMoveStartAttempts{ 0 };
        float swimMoveStartElapsed{ 0.0F };
        float graphStanceSampleElapsed{ 0.0F };
        float graphSneakMismatchElapsed{ 0.0F };
        float graphStandingMismatchElapsed{ 0.0F };
        std::uint32_t graphSneakCorrections{ 0 };
        std::uint32_t graphStandingCorrections{ 0 };
        std::uint32_t turnStateOverrides{ 0 };
        bool lastVariableUpdateAccepted{ false };
        bool localInterruptionLatched{ false };
        bool graphRecoveryInProgress{ false };
        bool graphRecoveryUsed3DReset{ false };
        RE::BSAnimationGraphManager* graphBeforeRecovery{ nullptr };
        std::uint32_t observedHitEvents{ 0 };
        float hitReactionObservationRemaining{ 0.0F };
        // Main-loop passes, only used to throttle logs.
        std::uint32_t recoveryWaitFrames{ 0 };
        float recoveryWaitElapsed{ 0.0F };
        std::uint32_t recoveryStableFrames{ 0 };
        std::uint32_t recoveryCount{ 0 };
        bool attackPlaybackPending{ false };
        bool attackPlaybackObserved{ false };
        float attackPlaybackElapsed{ 0.0F };
        // A remote actor's attack state does not report bashes (observed in game).
        bool attackPlaybackBash{ false };
        Network::AttackAction attackPlaybackAction{ Network::AttackAction::kNormal };
        bool attackPlaybackPower{ false };
        bool attackChainDispatched{ false };
        // Only reported in the attack logs.
        RE::ATTACK_STATE_ENUM attackPlaybackState{ RE::ATTACK_STATE_ENUM::kNone };
        std::uint32_t attackChainCount{ 0 };
        std::uint32_t attackChainRejections{ 0 };
        bool bowDrawHeld{ false };
        bool bowReleasePending{ false };
        float bowPhaseElapsed{ 0.0F };
        float bowReleaseRequestElapsed{ 0.0F };
        float bowEventRetryElapsed{ 0.0F };
        std::array<bool, 2> castHeld{};
        std::array<bool, 2> castStopping{};
        std::array<float, 2> castStopRequestElapsed{};
        std::array<float, 2> castEventRetryElapsed{};
        bool castAnimationOwned{ false };
        bool blockHeld{ false };
        bool blockStopping{ false };
        bool blockStopAccepted{ false };
        float blockStopElapsed{ 0.0F };
        float blockEventRetryElapsed{ 0.0F };
#ifndef NDEBUG
        // Diagnostic: a set reason arms a graph probe for when the dispatch settles.
        const char* graphProbeReason{ nullptr };
        float graphProbeElapsed{ 0.0F };
        Multiplayer::AnimationState graphProbeState{};
        GraphProbe::Snapshot graphProbeSnapshot{};
#endif
    };

    [[nodiscard]] Multiplayer::AnimationState GetTargetState(const RemoteActor& a_remote);

    void SetAnimationState(RemoteActor& a_remote, Multiplayer::AnimationState a_animationState);

    [[nodiscard]] bool SendNetworkAnimationEvent(RemoteActor& a_remote, const RE::BSFixedString& a_eventName);

    [[nodiscard]] bool IsAnimationStateSettled(const RemoteActor& a_remote);
    [[nodiscard]] bool ShouldHoldTranslation(RemoteActor& a_remote, float a_deltaTime);
    [[nodiscard]] bool IsAnimationStateSettledExceptLanding(const RemoteActor& a_remote);

    [[nodiscard]] bool CanDispatchAttack(const RemoteActor& a_remote, const Network::Attack& a_attack);
    [[nodiscard]] bool CanChainAttack(const RemoteActor& a_remote, const Network::Attack& a_attack);
    [[nodiscard]] bool CanPowerUpAttack(const RemoteActor& a_remote, const Network::Attack& a_attack);
    [[nodiscard]] bool DispatchAttack(RemoteActor& a_remote, const Network::Attack& a_attack);
    // An attack event sent by another module, such as a beast attack.
    void OwnDispatchedAttack(RemoteActor& a_remote, const RE::Actor& a_actor);
    [[nodiscard]] bool IsAnimationActionPlaying(const RemoteActor& a_remote);
    // A swimming actor's graph would play a land takeoff.
    [[nodiscard]] bool IsSwimming(const RemoteActor& a_remote);

    void ApplyBowDraw(RemoteActor& a_remote, bool a_drawn, bool a_mayStart, float a_deltaTime);
    void ApplySpellCast(RemoteActor& a_remote, bool a_leftHand, bool a_casting, bool a_mayStart, float a_deltaTime);
    void ApplyBlock(RemoteActor& a_remote, bool a_blocking, bool a_mayStart, float a_deltaTime);
    [[nodiscard]] bool IsHandPresentationPlaying(const RemoteActor& a_remote);
    [[nodiscard]] bool IsWeaponTransitionPlaying(const RemoteActor& a_remote);
    void ApplyRiderWeaponState(RE::Actor& a_actor, RemoteActor& a_remote, bool a_weaponDrawn);

    void RequestAnimationUpdate(State& a_state);
    void RequestCheckpointUpdate(State& a_state);
    void InvalidateAnimationDispatch(State& a_state);
    void AdvanceAttackPlayback(RE::Actor& a_actor, State& a_state, float a_deltaTime);
    [[nodiscard]] bool RecoverAuthoritativeAnimation(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime);
    void ApplyAnimationUpdate(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime);
    void HoldIdleTurnState(RE::Actor& a_actor, RemoteActor& a_remote);
    [[nodiscard]] bool IsFurnitureEntryBlocked(const RemoteActor& a_remote);
}
