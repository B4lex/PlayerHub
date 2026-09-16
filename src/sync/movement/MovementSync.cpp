#include "sync/movement/MovementSync.h"
#include "actor/ActorMovement.h"
#include "actor/RemoteActor.h"

#include <cmath>

namespace MovementSync
{
	namespace
	{
		constexpr float kMinimumHeadingTravel = 1.0F;
		constexpr float kTwoPi = 6.28318530717958647692F;

		RE::NiPoint3 s_previousPosition{};
		RE::FormID s_previousCellID = 0;
		RE::FormID s_previousWorldspaceID = 0;
		bool s_hasPreviousPosition = false;
	}

	std::uint32_t GetWorldspaceID(const RE::TESObjectREFR& a_reference)
	{
		const auto* worldspace = a_reference.GetWorldspace();
		return worldspace ? worldspace->GetFormID() : 0;
	}

	LocalSample SampleLocalMovement(
		const LocalFrame& a_local,
		const RE::TESObjectCELL& a_cell,
		float a_snapshotElapsed)
	{
		const auto position = a_local.player.GetPosition();
		const auto cellID = a_cell.GetFormID();
		const auto worldspaceID = GetWorldspaceID(a_local.player);
		const bool comparableLocalSpace = s_hasPreviousPosition &&
			((worldspaceID != 0 || s_previousWorldspaceID != 0) ?
				(worldspaceID != 0 && worldspaceID == s_previousWorldspaceID) :
				(cellID == s_previousCellID));

		float speed = 0.0F;
		// The body angle is the facing direction. Travel direction is carried
		// separately so a backpedalling remote actor keeps facing forward.
		float heading = a_local.player.GetAngleZ();
		if (!std::isfinite(heading)) {
			heading = 0.0F;
		}
		static const RE::BSFixedString directionVariable{ "Direction" };
		float direction = 0.0F;
		const bool graphDirectionAvailable = a_local.player.GetGraphVariableFloat(
			directionVariable,
			direction) &&
			std::isfinite(direction);
		if (comparableLocalSpace && a_snapshotElapsed > 0.0F) {
			const float deltaX = position.x - s_previousPosition.x;
			const float deltaY = position.y - s_previousPosition.y;
			const float deltaZ = position.z - s_previousPosition.z;
			const float horizontalDistanceSquared =
				deltaX * deltaX + deltaY * deltaY;
			speed = std::sqrt(
				horizontalDistanceSquared + deltaZ * deltaZ) /
				a_snapshotElapsed;
			if (!std::isfinite(speed)) {
				speed = 0.0F;
			}

			if (!graphDirectionAvailable &&
				horizontalDistanceSquared >=
					kMinimumHeadingTravel * kMinimumHeadingTravel) {
				// A custom graph may not expose Direction. Skyrim heading zero faces
				// positive Y, so derive the travel angle relative to the body.
				direction = (std::atan2(deltaX, deltaY) - heading) / kTwoPi;
			}
		}
		direction = Multiplayer::NormalizeMovementDirection(direction);

		s_previousPosition = position;
		s_previousCellID = cellID;
		s_previousWorldspaceID = worldspaceID;
		s_hasPreviousPosition = true;

		return LocalSample{
			.position = position,
			.heading = heading,
			.speed = speed,
			.direction = direction,
			.cellID = cellID,
			.worldspaceID = worldspaceID
		};
	}

	bool IsInLocalSpace(
		const RemoteFrame& a_remote,
		RE::FormID a_localCellID,
		RE::FormID a_localWorldspaceID)
	{
		return IsSameCoordinateSpace(
			a_remote.checkpoint.cellID,
			a_remote.checkpoint.worldspaceID,
			a_localCellID,
			a_localWorldspaceID);
	}

	void SyncMovement(
		const RemoteFrame& a_remote,
		std::uint64_t& a_appliedSequence,
		RemoteActor& a_actor)
	{
		if (a_appliedSequence == a_remote.lastSequence) {
			return;
		}

		const RE::NiPoint3 targetPosition{
			a_remote.checkpoint.x,
			a_remote.checkpoint.y,
			a_remote.checkpoint.z
		};
		ActorMovement::SetTargetTransform(
			a_actor,
			targetPosition,
			a_remote.checkpoint.heading,
			a_remote.checkpoint.speed,
			a_remote.checkpoint.direction);
		a_appliedSequence = a_remote.lastSequence;
	}
}
