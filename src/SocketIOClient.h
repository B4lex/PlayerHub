#pragma once

#include "AppearanceState.h"
#include "MovementState.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Network
{
	struct PlayerTransform
	{
		float x{ 0.0F };
		float y{ 0.0F };
		float z{ 0.0F };
		float heading{ 0.0F };
		// Measured translation speed in Skyrim world units per second.
		float speed{ 0.0F };
		// Locomotion, jump phase, and combat stance from the same snapshot.
		Multiplayer::AnimationState animationState{};
		std::uint32_t cellID{ 0 };
		// Non-zero for exterior cells. Interior cells use zero and must match by
		// cell ID because their local coordinates are not globally comparable.
		std::uint32_t worldspaceID{ 0 };
		// Placed TESFurniture reference currently occupied by the source. Zero means
		// no furniture interaction. Form IDs require matching peer load orders.
		std::uint32_t furnitureRefID{ 0 };
		// Identifies the source animation-graph lifetime represented by this
		// checkpoint. Transient actions are only valid against the same epoch.
		std::uint64_t graphEpoch{ 1 };
		std::uint64_t sequence{ 0 };
	};

	struct AnimationAction
	{
		std::string playerID;
		std::uint64_t actionSequence{ 0 };
		std::uint64_t graphEpoch{ 0 };
		std::uint64_t checkpointSequence{ 0 };
	};

	struct RemotePlayerTransform
	{
		std::string playerID;
		PlayerTransform transform;
	};

	struct RemotePlayerAppearance
	{
		std::string playerID;
		Multiplayer::PlayerAppearance appearance;
	};

	struct RemotePlayerChanges
	{
		std::vector<RemotePlayerTransform> transforms;
		// Only the newest pending appearance revision per player.
		std::vector<RemotePlayerAppearance> appearances;
		std::vector<AnimationAction> animationActions;
		std::vector<std::string> disconnectedPlayerIDs;
	};

	// Starts the Socket.IO client. Safe to call more than once.
	void Initialize();

	[[nodiscard]] bool IsConnected();

	// Called from the game thread. socket.io-client-cpp queues the emit for its
	// transport thread; no Skyrim objects cross the network boundary.
	void PublishLocalPlayerTransform(const PlayerTransform& a_transform);

	// Publishes the latest-wins appearance state. The caller owns revisions.
	void PublishLocalAppearance(const Multiplayer::PlayerAppearance& a_appearance);

	// Publishes one item in the ordered semantic action stream. Power attacks are
	// intentionally normalized to this same normal-attack vocabulary by capture.
	void PublishLocalNormalAttack(
		std::uint64_t a_graphEpoch,
		std::uint64_t a_checkpointSequence);

	// Transfers network-thread updates to the caller. The authoritative
	// main-loop frame hook is the only consumer.
	[[nodiscard]] RemotePlayerChanges DrainRemotePlayerChanges();
}
