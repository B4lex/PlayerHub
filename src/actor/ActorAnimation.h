#pragma once

#include "protocol/MovementState.h"

#include <RE/Skyrim.h>

#include <array>
#include <atomic>
#include <cstdint>

struct RemoteActor;

namespace ActorAnimation
{
	// Graph state machine of one remote actor: locomotion, jump, weapon stance,
	// swim and attack transitions, and recovery from local interruptions.
	struct State
	{
		// The staged animation request; the graph converges toward it.
		Multiplayer::AnimationState targetAnimationState{};
		std::atomic<Multiplayer::MovementState> desired{
			Multiplayer::MovementState::kIdle
		};
		std::atomic_bool desiredJumping{ false };
		std::atomic_bool desiredWeaponDrawn{ false };
		std::atomic<float> desiredSpeed{ 0.0F };
		std::atomic<float> desiredDirection{ 0.0F };
		// Direction the active cyclic locomotion blend was last moved to.
		float crossBlendedDirection{ 0.0F };
		bool hasCrossBlendedDirection{ false };
		// CyclicFreeze was sent; CyclicCrossBlend follows on the next graph update.
		bool directionCrossBlendPending{ false };
		// Time since the last direction cross-blend started.
		float directionCrossBlendElapsed{ 0.0F };
		std::atomic_uint64_t desiredRevision{ 0 };
		std::atomic_uint64_t processedRevision{ 0 };

		// Accessed only by the main-loop synchronization pass. Rejected events receive
		// a short bounded retry because false can mean either "not ready yet" or
		// "already transitioning"; they must never hold the state indefinitely.
		Multiplayer::AnimationState lastDispatched{};
		Multiplayer::AnimationState lastAttempted{};
		RE::BSAnimationGraphManager* graphManager{ nullptr };
		RE::BSAnimationGraphManager* landingEventSinkGraph{ nullptr };
		std::uint32_t failedDispatchAttempts{ 0 };
		std::uint32_t rejectedEventAttempts{ 0 };
		float rejectedEventElapsed{ 0.0F };
		// JumpLand only enters Skyrim's timed landing branch. Keep the compound
		// transition open until that branch emits JumpLandEnd, then fully reassert
		// grounded locomotion before accepting another animation edge. Only a
		// consecutive takeoff may interrupt it, as it does on the local player.
		bool landingCompletionPending{ false };
		std::uint32_t landingCompletionWaitFrames{ 0 };
		float landingCompletionWaitElapsed{ 0.0F };
		// JumpLandEnd selects the graph's exit branch. Reassert the active grounded
		// locomotion events once, without forgetting the dispatched weapon stance.
		bool landingLocomotionReassertPending{ false };
		bool hasDispatched{ false };
		bool hasAttempted{ false };
		bool hasVariableUpdateResult{ false };
		// Weapon draw/sheathe can make Skyrim's graph leave its active locomotion
		// branch. Keep the replay pending even if the transition starts while idle,
		// then reassert locomotion/jump in valid graph stages after its completion
		// annotation (or a bounded fallback for custom graphs).
		bool weaponLocomotionReassertPending{ false };
		bool weaponTransitionCompletionObserved{ false };
		float weaponTransitionWaitElapsed{ 0.0F };
		// The fallback measures the matching clip, not time spent finishing an
		// opposite transition that was already running when the request flipped.
		bool weaponTransitionAnimationObserved{ false };
		bool weaponTransitionExitForced{ false };
		bool weaponDefaultStateDispatched{ false };
		bool weaponReplayBaseDispatched{ false };
		bool weaponTransitionMoveStartDispatched{ false };
		// Locomotion was restored by the bounded fallback before the clip's exit
		// annotation. A late exit annotation then replays locomotion once more.
		bool weaponRestoredWithoutCompletion{ false };
		// Bounded post-replay diagnostics compare the graph state reached after the
		// visually working draw path with the failing sheathe path.
		std::uint32_t weaponReplayDiagnosticFramesRemaining{ 0 };
		bool weaponReplayDiagnosticWeaponDrawn{ false };
		// swimStop exits through a client-managed graph branch. Reassert locomotion
		// once the actor state confirms that Skyrim has left swimming.
		bool swimLocomotionReassertPending{ false };
		bool lastVariableUpdateAccepted{ false };
		bool localInterruptionLatched{ false };
		bool graphRecoveryInProgress{ false };
		bool graphRecoveryUsed3DReset{ false };
		RE::BSAnimationGraphManager* graphBeforeRecovery{ nullptr };
		// A hit notification is only evidence that a local reaction may follow. Keep
		// it briefly so the main-loop pass can detect a delayed stagger/recoil without
		// rebuilding an otherwise healthy graph.
		std::uint32_t observedHitEvents{ 0 };
		float hitReactionObservationRemaining{ 0.0F };
		std::uint32_t recoveryWaitFrames{ 0 };
		float recoveryWaitElapsed{ 0.0F };
		std::uint32_t recoveryStableFrames{ 0 };
		std::uint32_t recoveryCount{ 0 };
		// A network attack temporarily owns the graph. Base-state changes wait for
		// the native attack state to finish (or a bounded fallback); later attacks
		// either chain in its combo window or wait as well.
		bool attackPlaybackPending{ false };
		bool attackPlaybackObserved{ false };
		float attackPlaybackElapsed{ 0.0F };
		// A queued attack was chained into the playing one. Cleared once the graph
		// leaves the combo window, so one window consumes at most one attack.
		bool attackChainDispatched{ false };
		bool attackLocomotionReassertPending{ false };
		// A network bow draw is held from an accepted bowAttackStart until the
		// checkpoint releases it. The shot then owns the graph for its clip length,
		// like a normal attack.
		bool bowDrawHeld{ false };
		bool bowReleasePending{ false };
		float bowPhaseElapsed{ 0.0F };
		// Time since the release was first requested; rejected releases retry.
		float bowReleaseRequestElapsed{ 0.0F };
		float bowEventRetryElapsed{ 0.0F };
		// Network spell casting per hand, indexed by IsLeftHand: [0] right, [1]
		// left. Held from a cast request on the hand's own magic caster until the
		// checkpoint stops it; then stopping until the caster and the graph have
		// both left the cast.
		std::array<bool, 2> castHeld{};
		std::array<bool, 2> castStopping{};
		std::array<float, 2> castStopRequestElapsed{};
		std::array<float, 2> castEventRetryElapsed{};
		// Mirrors ActorRegistry cast ownership; kept across graph resets so an
		// abandoned cast still releases it.
		bool castAnimationOwned{ false };
	};

	[[nodiscard]] Multiplayer::AnimationState GetTargetState(const RemoteActor& a_remote);

	void SetAnimationState(
		RemoteActor& a_remote,
		Multiplayer::AnimationState a_animationState);

	// Sends a replicated animation event directly to the registered graph,
	// bypassing the local-event filter. This must only run on the game thread.
	[[nodiscard]] bool SendNetworkAnimationEvent(
		RemoteActor& a_remote,
		const RE::BSFixedString& a_eventName);

	[[nodiscard]] bool IsAnimationStateSettled(const RemoteActor& a_remote);
	// Every requested revision was processed, but the graph may still be playing
	// a landing branch. A consecutive takeoff is allowed to interrupt that branch.
	[[nodiscard]] bool IsAnimationStateSettledExceptLanding(
		const RemoteActor& a_remote);

	// Semantic attacks may only enter a stable, grounded, fully drawn graph.
	[[nodiscard]] bool CanDispatchNormalAttack(const RemoteActor& a_remote);
	// The playing network attack is in its combo window, where the next queued
	// attack chains as the following swing, as it does on the local player.
	[[nodiscard]] bool CanChainNormalAttack(const RemoteActor& a_remote);
	// Starts an attack, or chains it into the playing one when that is allowed.
	[[nodiscard]] bool DispatchNormalAttack(RemoteActor& a_remote);
	[[nodiscard]] bool IsAnimationActionPlaying(const RemoteActor& a_remote);

	// Converges a network bow draw toward a_drawn: starts it when a_mayStart and
	// the graph is stable, grounded, fully drawn, and holds a bow; releases it
	// once a_drawn clears. Only the animation plays; CombatIsolation keeps the
	// arrow from being fired.
	void ApplyBowDraw(
		RemoteActor& a_remote,
		bool a_drawn,
		bool a_mayStart,
		float a_deltaTime);
	// Converges one hand's placeholder-spell channelling toward a_casting, under
	// the same graph conditions as ApplyBowDraw. CombatIsolation keeps the spell
	// from being released.
	void ApplySpellCast(
		RemoteActor& a_remote,
		bool a_leftHand,
		bool a_casting,
		bool a_mayStart,
		float a_deltaTime);
	// A bow draw, its release, or a cast still owns the actor's stance and hands.
	[[nodiscard]] bool IsHandPresentationPlaying(const RemoteActor& a_remote);
	// A replicated draw or sheathe clip has not reached its exit annotation or
	// bounded fallback yet.
	[[nodiscard]] bool IsWeaponTransitionPlaying(const RemoteActor& a_remote);

	// Actor-module internals, used by ActorSpawner, ActorRuntime, ActorMovement,
	// and ActorFurniture.
	void RequestAnimationUpdate(
		State* a_animationState,
		Multiplayer::AnimationState a_state,
		float a_speed,
		float a_direction);
	void InvalidateAnimationDispatch(State& a_state);
	void AdvanceAttackPlayback(
		RE::Actor& a_actor,
		State& a_state,
		float a_deltaTime);
	// Returns true while a graph recovery holds transform and graph writes.
	[[nodiscard]] bool RecoverAuthoritativeAnimation(
		RE::Actor& a_actor,
		RemoteActor& a_remote,
		float a_deltaTime);
	void ApplyAnimationUpdate(
		RE::Actor& a_actor,
		RemoteActor& a_remote,
		float a_deltaTime);
	// An attack, weapon transition, or graph recovery must finish before native
	// furniture entry may take the graph.
	[[nodiscard]] bool IsFurnitureEntryBlocked(const RemoteActor& a_remote);
}
