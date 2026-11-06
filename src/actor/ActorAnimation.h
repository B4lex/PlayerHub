#pragma once

#include "actor/ActorMovement.h"
#include "actor/AttackDispatch.h"
#include "actor/GraphProbe.h"
#include "actor/GraphRecovery.h"
#include "actor/HandPresentation.h"
#include "actor/IdlePresentation.h"
#include "actor/VoicePresentation.h"
#include "actor/WeaponTransition.h"
#include "protocol/MovementState.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>

struct RemoteActor;

namespace ActorAnimation
{
    struct Dispatch
    {
        Multiplayer::AnimationState latched{};
        Multiplayer::AnimationState attempted{};
        bool hasLatched{ false };
        bool hasAttempted{ false };
        std::uint32_t variableAttempts{ 0 };
        std::uint32_t eventAttempts{ 0 };
        float eventRetryElapsed{ 0.0F };
        // One bit per graph variable the last refresh could not write, for the log.
        std::uint32_t rejectedVariables{ 0 };
        // Set when a landing, attack, hand presentation, or swim releases the graph;
        // names the release for the log.
        const char* reassertReason{ nullptr };
        RE::BSAnimationGraphManager* graph{ nullptr };
        RE::BSAnimationGraphManager* landingSinkGraph{ nullptr };
    };

    struct Landing
    {
        bool completionPending{ false };
        float waitElapsed{ 0.0F };
    };

    struct Swim
    {
        // A graph that took SwimStart in SwimIdle while moving leaves it only on
        // moveStart.
        bool moveStartPending{ false };
        std::uint32_t waitPasses{ 0 };
        std::uint32_t attempts{ 0 };
        float elapsed{ 0.0F };
    };

    struct StanceReconciliation
    {
        float sampleElapsed{ 0.0F };
        float sneakMismatchElapsed{ 0.0F };
        float standingMismatchElapsed{ 0.0F };
        std::uint32_t sneakCorrections{ 0 };
        std::uint32_t standingCorrections{ 0 };
    };

    struct DirectionBlend
    {
        float direction{ 0.0F };
        bool hasDirection{ false };
        bool crossBlendPending{ false };
        float cooldown{ 0.0F };
    };

    struct TranslationHold
    {
        float elapsed{ 0.0F };
        bool reported{ false };
    };

    struct State
    {
        Multiplayer::AnimationState targetAnimationState{};
        std::uint64_t requestedRevision{ 0 };
        std::uint64_t processedRevision{ 0 };
        bool checkpointUpdatePending{ false };
        float publishedGraphSpeed{ 0.0F };
        float zeroSpeedSampleElapsed{ 0.0F };
        // Mirrors the registry's flag and outlives a dispatch reset, so the next
        // ApplySpellCast clears both together.
        bool castAnimationOwned{ false };
        std::uint32_t turnStateOverrides{ 0 };
        TranslationHold translationHold;
        VoicePresentation::State voice;
        GraphRecovery::State recovery;

        // Tied to one graph instance; InvalidateAnimationDispatch resets each of them.
        Dispatch dispatch;
        Landing landing;
        WeaponTransition::State weapon;
        Swim swim;
        StanceReconciliation stance;
        AttackDispatch::State attack;
        HandPresentation::State hands;
        IdlePresentation::State idle;
        DirectionBlend direction;

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
    [[nodiscard]] bool IsAnimationStateSettledExceptLanding(const RemoteActor& a_remote);
    // The actor's state when an attack or hand presentation event may reach its graph:
    // the dispatch is settled and grounded on the graph it tracks, with the weapon
    // drawn in both the target and the actor. Null otherwise.
    [[nodiscard]] const RE::ActorState* GetCombatReadyState(const RemoteActor& a_remote, const RE::Actor& a_actor);
    // A swimming actor's graph would play a land takeoff.
    [[nodiscard]] bool IsSwimming(const RemoteActor& a_remote);

    void RequestAnimationUpdate(State& a_state);
    void RequestCheckpointUpdate(State& a_state);
    void InvalidateAnimationDispatch(State& a_state);
    void ApplyAnimationUpdate(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime);
    void HoldIdleTurnState(RE::Actor& a_actor, RemoteActor& a_remote);
    // a_presentable is the network state the actor should show, or none while furniture
    // owns it. Returns none to hold translation for this pass.
    [[nodiscard]] std::optional<ActorMovement::TranslationMode> GetTranslationMode(RemoteActor& a_remote,
        const std::optional<Multiplayer::AnimationState>& a_presentable, float a_deltaTime);
    [[nodiscard]] bool IsFurnitureEntryBlocked(const RemoteActor& a_remote);
}
