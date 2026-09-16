#pragma once

#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

namespace MovementSync
{
	constexpr bool IsSameCoordinateSpace(
		RE::FormID a_firstCellID,
		RE::FormID a_firstWorldspaceID,
		RE::FormID a_secondCellID,
		RE::FormID a_secondWorldspaceID)
	{
		// Exterior cells share world coordinates. They can synchronize across
		// their cell boundaries as long as both players are in the same world.
		if (a_firstWorldspaceID != 0 || a_secondWorldspaceID != 0) {
			return a_firstWorldspaceID != 0 &&
				a_firstWorldspaceID == a_secondWorldspaceID;
		}

		// Interior coordinates are local to their cell, so they only describe
		// the same space when the cell IDs match exactly.
		return a_firstCellID == a_secondCellID;
	}

	static_assert(IsSameCoordinateSpace(1, 0, 1, 0));
	static_assert(!IsSameCoordinateSpace(1, 0, 2, 0));
	static_assert(IsSameCoordinateSpace(1, 3, 2, 3));
	static_assert(!IsSameCoordinateSpace(1, 3, 1, 4));
	static_assert(!IsSameCoordinateSpace(1, 0, 1, 3));

	[[nodiscard]] std::uint32_t GetWorldspaceID(const RE::TESObjectREFR& a_reference);

	// Transform part of one local checkpoint.
	struct LocalSample
	{
		RE::NiPoint3 position{};
		float heading{ 0.0F };
		float speed{ 0.0F };
		float direction{ 0.0F };
		RE::FormID cellID{ 0 };
		RE::FormID worldspaceID{ 0 };
	};

	// Local stage, called by CheckpointPublisher once per published checkpoint.
	// Speed is measured against the previous published sample.
	[[nodiscard]] LocalSample SampleLocalMovement(
		const LocalFrame& a_local,
		const RE::TESObjectCELL& a_cell,
		float a_snapshotElapsed);

	[[nodiscard]] bool IsInLocalSpace(
		const RemoteFrame& a_remote,
		RE::FormID a_localCellID,
		RE::FormID a_localWorldspaceID);

	// Remote stage: hands a newly accepted checkpoint transform to the actor.
	void SyncMovement(
		const RemoteFrame& a_remote,
		std::uint64_t& a_appliedSequence,
		RemoteActor& a_actor);
}
