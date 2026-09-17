#pragma once

#include "protocol/PlayerState.h"
#include "sync/SyncFrame.h"

#include <cstdint>
#include <deque>
#include <optional>

struct RemoteActor;

// Semantic attack presentation: when a captured local attack or bash is
// published and when a received one may play on the remote actor, and the
// sustained bow draw, spell channelling, and blocking that travel with each
// checkpoint. Local world
// combat with the actor, and anything it would release, is prevented
// separately by actor/CombatIsolation.
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

	// Local stage, every pass: detects the start of a local melee attack or bash
	// and which hand it uses.
	[[nodiscard]] std::optional<Network::AttackAction> CaptureLocalAttack(
		const LocalFrame& a_local);
	// Local stage, every pass: observes the local bow draw, spell channelling, and
	// blocking. Returns true when one of them began, so equipment can be flushed
	// first.
	[[nodiscard]] bool CaptureLocalHandPresentation(const LocalFrame& a_local);
	// Local stage, called by CheckpointPublisher once per published checkpoint:
	// everything presented since the previous checkpoint, so a presentation
	// shorter than the snapshot interval still reaches peers.
	[[nodiscard]] Multiplayer::HandPresentation SampleLocalHandPresentation();
	// Local stage, every pass after equipment capture: adds a captured attack to
	// the bounded publish backlog, discarding it at a graph-epoch boundary.
	void QueueLocalAttacks(
		std::optional<Network::AttackAction> a_capturedAttack,
		bool a_graphEpochChanged);
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
	// Remote stage, after SyncCombat: converges the bow draw, spell channelling,
	// and blocking toward the checkpoint. A sheathe, jump, or furniture checkpoint
	// ends them. The caller skips it while furniture owns the actor.
	void SyncHandPresentation(
		const RemoteFrame& a_remote,
		RemoteActor& a_actor,
		bool a_handsSettled,
		float a_delta);
}
