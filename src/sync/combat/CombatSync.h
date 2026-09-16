#pragma once

#include "protocol/PlayerState.h"
#include "sync/SyncFrame.h"

#include <cstdint>
#include <deque>

struct RemoteActor;

// Semantic attack presentation: when a captured local attack is published and
// when a received attack may play on the remote actor. Local world combat with
// the actor is prevented separately by actor/CombatIsolation.
namespace CombatSync
{
	struct PendingAnimationAction
	{
		Network::AnimationAction action;
		float age{ 0.0F };
		std::uint32_t dispatchAttempts{ 0 };
		// Time since the first rejected dispatch attempt.
		float retryElapsed{ 0.0F };
	};

	// Transient attack history of one peer. It is never reconstructed after a
	// respawn or coordinate-space change.
	struct RemoteState
	{
		std::deque<PendingAnimationAction> pendingAnimationActions;
	};

	// Local stage, every pass: detects the start of a local melee attack.
	[[nodiscard]] bool CaptureLocalAttack(const LocalFrame& a_local);
	// Local stage, every pass after equipment capture: adds captured attacks to
	// the bounded publish backlog, discarding it at a graph-epoch boundary.
	void QueueLocalAttacks(std::uint32_t a_capturedAttacks, bool a_graphEpochChanged);
	// Attacks captured while disconnected have no valid sequence boundary.
	void DiscardLocalAttacks();
	// Publishes queued attacks against the checkpoint that was just published.
	void PublishLocalAttacks(const Network::PlayerTransform& a_checkpoint);

	// Receive stage: rejects queued actions that a newer checkpoint invalidates.
	void OnCheckpointAccepted(RemoteState& a_state, const CheckpointUpdate& a_update);
	// Receive stage: a_state and a_checkpoint are null for an unknown player.
	void AcceptRemoteAction(
		RemoteState* a_state,
		const Network::PlayerTransform* a_checkpoint,
		Network::AnimationAction&& a_action);

	// Remote stage: ages queued actions and drops timed-out ones.
	void AgeRemoteActions(const RemoteFrame& a_remote, RemoteState& a_state, float a_delta);
	void ClearRemoteActions(RemoteState& a_state);
	// Remote stage: attempts the oldest queued attack. The caller skips it while
	// furniture owns the actor.
	void SyncCombat(
		const RemoteFrame& a_remote,
		RemoteState& a_state,
		RemoteActor& a_actor,
		bool a_handsSettled);
}
