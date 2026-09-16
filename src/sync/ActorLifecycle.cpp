#include "sync/ActorLifecycle.h"
#include "actor/ActorSpawner.h"
#include "sync/PeerState.h"
#include "sync/SyncFrame.h"
#include "sync/movement/MovementSync.h"

#include <algorithm>

namespace ActorLifecycle
{
	namespace
	{
		constexpr float kActorSpeed = 550.0F;
		constexpr float kActorStoppingDistance = 1.0F;
		constexpr float kSpawnRetryDelay = 1.0F;

		// Returns the retired actor's handle for logging.
		std::uint32_t Retire(PeerState& a_peer)
		{
			auto& lifecycle = a_peer.lifecycle;
			const auto actorHandle = lifecycle.actor->handle.native_handle();
			ActorSpawner::Remove(*lifecycle.actor);
			lifecycle.actor.reset();
			CombatSync::ClearRemoteActions(a_peer.combat);
			a_peer.appliedSequence = 0;
			lifecycle.spawnRetryRemaining = 0.0F;
			return actorHandle;
		}
	}

	bool HasChangedSpawnedSpace(const PeerState& a_peer)
	{
		return a_peer.lifecycle.actor &&
			!MovementSync::IsSameCoordinateSpace(
				a_peer.latestCheckpoint.cellID,
				a_peer.latestCheckpoint.worldspaceID,
				a_peer.lifecycle.spawnedCellID,
				a_peer.lifecycle.spawnedWorldspaceID);
	}

	void RetireForAppearanceChange(const RemoteFrame& a_remote, PeerState& a_peer)
	{
		// Race, sex, head parts, and weight are only applied when 3D loads.
		// Rebuild the actor from the newest checkpoint with the new base.
		const auto actorHandle = Retire(a_peer);
		logs::info(
			"Recycling remote player puppet {} actor {:08X} after appearance change: spawnedRevision={} revision={}",
			a_remote.playerID,
			actorHandle,
			static_cast<unsigned long long>(a_peer.appearance.spawnedRevision),
			static_cast<unsigned long long>(a_peer.appearance.latest->revision));
	}

	void RetireForSpaceChange(
		const RemoteFrame& a_remote,
		PeerState& a_peer,
		RE::FormID a_localCellID,
		RE::FormID a_localWorldspaceID)
	{
		const auto actorHandle = Retire(a_peer);
		logs::info(
			"Recycling remote player puppet {} actor {:08X} after coordinate-space change: spawnedCell={:08X} spawnedWorld={:08X} targetCell={:08X} targetWorld={:08X} localCell={:08X} localWorld={:08X}",
			a_remote.playerID,
			actorHandle,
			a_peer.lifecycle.spawnedCellID,
			a_peer.lifecycle.spawnedWorldspaceID,
			a_remote.checkpoint.cellID,
			a_remote.checkpoint.worldspaceID,
			a_localCellID,
			a_localWorldspaceID);
	}

	void OnActorLost(const RemoteFrame& a_remote, PeerState& a_peer)
	{
		logs::warn(
			"Remote player puppet {} lost its actor handle; respawning it",
			a_remote.playerID);
		a_peer.lifecycle.actor.reset();
		CombatSync::ClearRemoteActions(a_peer.combat);
		a_peer.lifecycle.spawnRetryRemaining = kSpawnRetryDelay;
	}

	void RemoveOnDisconnect(PeerState& a_peer)
	{
		if (a_peer.lifecycle.actor) {
			ActorSpawner::Remove(*a_peer.lifecycle.actor);
		}
	}

	void SpawnWhenDue(
		const RemoteFrame& a_remote,
		PeerState& a_peer,
		RE::TESNPC* a_localPlayerBase,
		float a_delta)
	{
		auto& lifecycle = a_peer.lifecycle;
		lifecycle.spawnRetryRemaining -= (std::max)(a_delta, 0.0F);
		if (lifecycle.spawnRetryRemaining > 0.0F) {
			return;
		}

		auto* spawnBase = AppearanceSync::GetSpawnBase(
			a_peer.appearance,
			a_remote.playerID,
			a_localPlayerBase);
		if (!spawnBase) {
			// Briefly wait for the first appearance so a late joiner is not spawned
			// with the local look and immediately replaced.
			return;
		}

		const RE::NiPoint3 targetPosition{
			a_remote.checkpoint.x,
			a_remote.checkpoint.y,
			a_remote.checkpoint.z
		};
		lifecycle.actor = ActorSpawner::Spawn(
			spawnBase,
			targetPosition,
			a_remote.checkpoint.heading,
			Multiplayer::AnimationState{},
			kActorSpeed,
			kActorStoppingDistance);
		if (!lifecycle.actor) {
			lifecycle.spawnRetryRemaining = kSpawnRetryDelay;
			return;
		}
		AppearanceSync::OnActorSpawned(a_peer.appearance, spawnBase);
		EquipmentSync::OnActorSpawned(a_peer.equipment);
		a_peer.appliedSequence = a_peer.lastSequence;
		lifecycle.spawnedCellID = a_remote.checkpoint.cellID;
		lifecycle.spawnedWorldspaceID = a_remote.checkpoint.worldspaceID;

		logs::info(
			"Allocated remote player puppet {} as actor {:08X} at transform sequence={} with target animation state={}/{}; waiting for 3D and animation graph readiness",
			a_remote.playerID,
			lifecycle.actor->handle.native_handle(),
			static_cast<unsigned long long>(a_peer.appliedSequence),
			Multiplayer::GetMovementStateName(
				a_remote.checkpoint.animationState.movementState),
			a_remote.checkpoint.animationState.jumping ? "jumping" : "grounded");

		if (auto* console = RE::ConsoleLog::GetSingleton()) {
			console->Print(
				"SkyrimMP remote player puppet reference allocated; initializing.");
		}
	}
}
