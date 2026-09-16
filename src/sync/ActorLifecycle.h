#pragma once

#include "actor/RemoteActor.h"

#include <RE/Skyrim.h>

#include <optional>

struct PeerState;
struct RemoteFrame;

// Spawn/retire state of a peer's remote actor. The actor handle and its ready
// flag have one owner, RemoteActor; other modules borrow it for a single pass.
struct ActorLifecycleState
{
	std::optional<RemoteActor> actor;
	RE::FormID spawnedCellID{ 0 };
	RE::FormID spawnedWorldspaceID{ 0 };
	float spawnRetryRemaining{ 0.0F };
};

namespace ActorLifecycle
{
	// The newest checkpoint left the coordinate space the actor was spawned in.
	[[nodiscard]] bool HasChangedSpawnedSpace(const PeerState& a_peer);

	// Retirements keep every reconstructable receive state (checkpoint,
	// appearance, equipment) and drop the transient attack history.
	void RetireForAppearanceChange(const RemoteFrame& a_remote, PeerState& a_peer);
	void RetireForSpaceChange(
		const RemoteFrame& a_remote,
		PeerState& a_peer,
		RE::FormID a_localCellID,
		RE::FormID a_localWorldspaceID);
	// The actor handle became invalid; schedules a respawn after a delay.
	void OnActorLost(const RemoteFrame& a_remote, PeerState& a_peer);
	// The player left; the peer state is about to be erased.
	void RemoveOnDisconnect(PeerState& a_peer);

	// Spawns the actor from the newest checkpoint once the retry delay elapsed
	// and an actor base is available.
	void SpawnWhenDue(
		const RemoteFrame& a_remote,
		PeerState& a_peer,
		RE::TESNPC* a_localPlayerBase,
		float a_delta);
}
