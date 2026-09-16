#pragma once

#include "protocol/MovementState.h"
#include "sync/SyncFrame.h"
#include "sync/movement/MovementSync.h"

#include <cstdint>
#include <optional>

struct RemoteActor;

namespace AnimationSync
{
	struct LocalGraphEpoch
	{
		std::uint64_t epoch{ 1 };
		// The local graph was replaced or lost since the previous pass.
		bool changed{ false };
	};

	// Local stage, every pass: tracks the local animation-graph lifetime.
	[[nodiscard]] LocalGraphEpoch ObserveLocalGraphEpoch(const LocalFrame& a_local);

	// Local stage, called by CheckpointPublisher once per published checkpoint:
	// locomotion, jump phase, and weapon stance of that checkpoint.
	[[nodiscard]] Multiplayer::AnimationState SampleLocalAnimation(
		const LocalFrame& a_local,
		std::uint64_t a_sequence,
		const MovementSync::LocalSample& a_movement);

	// Receive stage: records a newer checkpoint's animation target.
	void OnCheckpointAccepted(const CheckpointUpdate& a_update);

	// Remote stage: stages the next graph step toward the checkpoint's state.
	// The caller skips it while an attack or furniture owns the graph.
	void SyncAnimation(
		const RemoteFrame& a_remote,
		RemoteActor& a_actor,
		bool a_readyForWeaponDraw);

	// Remote stage result: how the transform may follow the network path this
	// pass, or nullopt to hold it while a compound transition converges.
	[[nodiscard]] std::optional<Multiplayer::AnimationState> GetTranslationState(
		const RemoteFrame& a_remote,
		const RemoteActor& a_actor,
		bool a_readyForWeaponDraw,
		bool a_furniturePlaying);
}
