#pragma once

#include "MovementState.h"

#include <RE/Skyrim.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace NPCSpawner
{
	// Trampoline bytes InstallRemoteReplicaHooks needs: three entry detours of
	// a 5-byte gateway plus two 14-byte absolute jumps each.
	inline constexpr std::size_t kRemoteReplicaTrampolineSize = 3 * (5 + 2 * 14);

	// Installs target-side damage/magic immunity, the remote-ownership policy,
	// combat isolation from local actors, and the local animation-event filter.
	// Requires kRemoteReplicaTrampolineSize bytes of the SKSE trampoline.
	void InstallRemoteReplicaHooks();
	// Registers event sinks that require Skyrim's data-loaded state.
	void RegisterRemoteReplicaEventSinks();
	// Binds engine mutations to the thread running Skyrim's main-loop hook.
	// Returns false if that hook unexpectedly migrates to another thread.
	[[nodiscard]] bool BindAuthoritativeGameThread();

	struct AnimationState
	{
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
		// A network attack temporarily owns the graph. Base-state changes and later
		// actions wait for the native attack state to finish (or a bounded fallback).
		bool attackPlaybackPending{ false };
		bool attackPlaybackObserved{ false };
		float attackPlaybackElapsed{ 0.0F };
		bool attackLocomotionReassertPending{ false };
		// Skyrim's native furniture interaction owns the graph until the actor has
		// fully returned to its normal sit/sleep state. During that interval the
		// firewall admits locally generated furniture events; no event name is
		// accepted from a peer.
		bool furnitureAnimationOwned{ false };
	};

	struct ControlledNPC
	{
		RE::ActorHandle handle{};
		RE::NiPoint3 targetPosition{};
		float targetHeading{ 0.0F };
		Multiplayer::AnimationState targetAnimationState{};
		std::shared_ptr<AnimationState> animationState;
		// Network translation speed is intentionally independent from animation.
		float translationSpeed{ 550.0F };
		// Speed from the same network sample as targetPosition and movement state.
		float animationSpeed{ 550.0F };
		// Graph `Direction` from the same sample: travel relative to heading.
		float animationDirection{ 0.0F };
		float stoppingDistance{ 1.0F };
		RE::IAnimationGraphManagerHolder* animationGraphHolder{ nullptr };
		RE::FormID targetFurnitureRefID{ 0 };
		RE::FormID requestedFurnitureRefID{ 0 };
		float furnitureRetryRemaining{ 0.0F };
		float furnitureTransitionElapsed{ 0.0F };
		RE::SIT_SLEEP_STATE loggedSitSleepState{ RE::SIT_SLEEP_STATE::kNormal };
		std::uint32_t initializationWaitFrames{ 0 };
		bool ready{ false };
	};

	// These functions operate on Skyrim objects and must run on the bound main-loop
	// thread. Network callbacks queue plain data for that frame hook to drain.
	[[nodiscard]] std::optional<ControlledNPC> SpawnControlledNPC(
		RE::TESNPC* a_npcBase,
		const RE::NiPoint3& a_targetPosition,
		float a_targetHeading,
		Multiplayer::AnimationState a_animationState,
		float a_speed = 250.0F,
		float a_stoppingDistance = 1.0F);

	void SetTargetTransform(
		ControlledNPC& a_npc,
		const RE::NiPoint3& a_targetPosition,
		float a_targetHeading,
		float a_translationSpeed,
		float a_direction);

	void SetAnimationState(
		ControlledNPC& a_npc,
		Multiplayer::AnimationState a_animationState);

	// Sets reconstructable native TESFurniture occupancy. Zero requests exit.
	void SetFurnitureState(
		ControlledNPC& a_npc,
		RE::FormID a_furnitureRefID);
	[[nodiscard]] bool IsFurnitureInteractionPlaying(const ControlledNPC& a_npc);

	// Sends a replicated animation event directly to the registered graph,
	// bypassing the local-event filter. This must only run on the game thread.
	[[nodiscard]] bool SendNetworkAnimationEvent(
		ControlledNPC& a_npc,
		const RE::BSFixedString& a_eventName);

	[[nodiscard]] bool IsAnimationStateSettled(const ControlledNPC& a_npc);
	// Every requested revision was processed, but the graph may still be playing
	// a landing branch. A consecutive takeoff is allowed to interrupt that branch.
	[[nodiscard]] bool IsAnimationStateSettledExceptLanding(
		const ControlledNPC& a_npc);

	// Semantic attacks may only enter a stable, grounded, fully drawn graph.
	[[nodiscard]] bool CanDispatchNormalAttack(const ControlledNPC& a_npc);
	[[nodiscard]] bool DispatchNormalAttack(ControlledNPC& a_npc);
	[[nodiscard]] bool IsAnimationActionPlaying(const ControlledNPC& a_npc);
	// A replicated draw or sheathe clip has not reached its exit annotation or
	// bounded fallback yet.
	[[nodiscard]] bool IsWeaponTransitionPlaying(const ControlledNPC& a_npc);

	void RemoveControlledNPC(ControlledNPC& a_npc);

	// Returns false when the actor handle is no longer valid. a_translationState
	// selects how the transform follows the network path; nullopt holds the
	// transform while a compound transition converges.
	[[nodiscard]] bool UpdateControlledNPC(
		ControlledNPC& a_npc,
		float a_deltaTime,
		std::optional<Multiplayer::AnimationState> a_translationState);
}
