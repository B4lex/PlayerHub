#pragma once

#include "protocol/MovementState.h"

#include <cstdint>
#include <string>

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
		// Travel direction relative to heading, in graph `Direction` units [0, 1).
		float direction{ 0.0F };
		// Locomotion, jump phase, and combat stance from the same snapshot.
		Multiplayer::AnimationState animationState{};
		// Bow draw and spell channelling, gated on the receiver by weapon stance.
		Multiplayer::HandPresentation handPresentation{};
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
}
