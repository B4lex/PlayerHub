#pragma once

#include <cmath>
#include <cstdint>

namespace Multiplayer
{
	enum class MovementState : std::uint8_t
	{
		kIdle,
		kWalking,
		kRunning,
		kSprinting,
		kSneakingIdle,
		kSneakingWalking,
		kSneakingRunning
	};

	// Jumping is orthogonal to grounded locomotion. Keeping both values allows
	// takeoff to establish its locomotion prerequisites first and landing to
	// restore grounded locomotion afterward without duplicating every state.
	struct AnimationState
	{
		MovementState movementState{ MovementState::kIdle };
		bool jumping{ false };
		// Combat stance is presentation state only. Remote combat decisions remain
		// suppressed independently by the puppet ownership policy.
		bool weaponDrawn{ false };

		constexpr bool operator==(const AnimationState&) const noexcept = default;
	};

	// Movement direction relative to the body facing, in the behavior graph's
	// `Direction` units: 0 forward, 0.25 right, 0.5 backward, 0.75 left.
	[[nodiscard]] inline float NormalizeMovementDirection(float a_direction) noexcept
	{
		if (!std::isfinite(a_direction)) {
			return 0.0F;
		}
		float direction = a_direction - std::floor(a_direction);
		// Rounding can produce exactly 1.0 for a tiny negative input.
		return direction >= 1.0F ? 0.0F : direction;
	}

	[[nodiscard]] constexpr const char* GetMovementStateName(
		MovementState a_state) noexcept
	{
		switch (a_state) {
		case MovementState::kIdle:
			return "idle";
		case MovementState::kWalking:
			return "walking";
		case MovementState::kRunning:
			return "running";
		case MovementState::kSprinting:
			return "sprinting";
		case MovementState::kSneakingIdle:
			return "sneakingIdle";
		case MovementState::kSneakingWalking:
			return "sneakingWalking";
		case MovementState::kSneakingRunning:
			return "sneakingRunning";
		}

		return "unknown";
	}
}
