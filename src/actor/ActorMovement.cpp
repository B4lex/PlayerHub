#include "actor/ActorMovement.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "actor/RemoteActor.h"

#include <algorithm>
#include <cmath>

namespace ActorMovement
{
	namespace
	{
		using ActorConstants::kMaximumFrameTime;
		using Multiplayer::IsMoving;

		constexpr float kArrivalTolerance = 1.0F;
		constexpr float kAirborneTransformCatchUpTime = 0.2F;
		constexpr float kTwoPi = 6.28318530717958647692F;
		constexpr float kHeadingDeadZone = 0.00872664625997164788F;
		constexpr float kMaximumHeadingRate = kTwoPi;

		void ApplySmoothedHeading(
			RE::Actor& a_actor,
			float a_targetHeading,
			float a_deltaTime)
		{
			if (!std::isfinite(a_targetHeading) || a_deltaTime <= 0.0F) {
				return;
			}

			const float currentHeading = a_actor.GetAngleZ();
			if (!std::isfinite(currentHeading)) {
				a_actor.SetHeading(a_targetHeading);
				return;
			}

			// remainder selects the shortest route across the -pi/pi boundary. Limiting
			// that route keeps individual network samples from snapping the actor and
			// repeatedly provoking Skyrim's native NPC_Turn transitions.
			const float headingDelta = std::remainder(
				a_targetHeading - currentHeading,
				kTwoPi);
			if (std::abs(headingDelta) <= kHeadingDeadZone) {
				return;
			}

			const float maximumStep = kMaximumHeadingRate * a_deltaTime;
			const float step = std::clamp(
				headingDelta,
				-maximumStep,
				maximumStep);
			a_actor.SetHeading(currentHeading + step);
		}
	}

	void SetTargetTransform(
		RemoteActor& a_remote,
		const RE::NiPoint3& a_targetPosition,
		float a_targetHeading,
		float a_translationSpeed,
		float a_direction)
	{
		a_remote.movement.targetPosition = a_targetPosition;
		a_remote.movement.targetHeading = a_targetHeading;
		if (std::isfinite(a_translationSpeed) && a_translationSpeed >= 0.0F) {
			a_remote.movement.translationSpeed = a_translationSpeed;
			a_remote.movement.animationSpeed = a_translationSpeed;
			a_remote.movement.animationDirection =
				Multiplayer::NormalizeMovementDirection(a_direction);
			ActorAnimation::RequestAnimationUpdate(
				a_remote.animation.get(),
				a_remote.animation->targetAnimationState,
				a_remote.movement.animationSpeed,
				a_remote.movement.animationDirection);
			ActorRegistry::SetAuthoritativeDirection(a_remote);
		}
	}

	void ApplyTranslation(
		RE::Actor& a_actor,
		RemoteActor& a_remote,
		float a_deltaTime,
		Multiplayer::AnimationState a_translationState)
	{
		const float deltaTime = std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
		ApplySmoothedHeading(a_actor, a_remote.movement.targetHeading, deltaTime);

		const auto current = a_actor.GetPosition();
		const auto& target = a_remote.movement.targetPosition;

		const RE::NiPoint3 offset{
			target.x - current.x,
			target.y - current.y,
			target.z - current.z
		};
		const float distanceSquared =
			offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;

		const float distance = std::sqrt(distanceSquared);
		const float distanceToTravel = distance - a_remote.movement.stoppingDistance;
		if (distanceToTravel <= kArrivalTolerance) {
			return;
		}
		if (!IsMoving(a_translationState.movementState) &&
			!a_translationState.jumping) {
			// An idle snapshot is a current-state checkpoint, not an instruction to
			// visibly walk the remaining interpolation backlog while playing idle.
			a_actor.SetPosition(target, true);
			return;
		}

		if (deltaTime <= 0.0F) {
			return;
		}

		const float interpolationSpeed = a_translationState.jumping ?
			(std::max)(
				a_remote.movement.translationSpeed,
				distanceToTravel / kAirborneTransformCatchUpTime) :
			a_remote.movement.translationSpeed;
		const float step = (std::min)(
			interpolationSpeed * deltaTime,
			distanceToTravel);
		const float interpolation = step / distance;

		const RE::NiPoint3 nextPosition{
			current.x + offset.x * interpolation,
			current.y + offset.y * interpolation,
			current.z + offset.z * interpolation
		};

		// Passing true keeps the actor's character controller in sync with its
		// reference transform, which is essential for a network-controlled remote actor.
		a_actor.SetPosition(nextPosition, true);
	}
}
