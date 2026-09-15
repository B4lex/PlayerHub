#pragma once

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
