#pragma once

#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

namespace MovementSync
{
    inline constexpr float kRelocationDistance = 4096.0F;

    constexpr bool IsSameCoordinateSpace(RE::FormID a_firstCellID, RE::FormID a_firstWorldspaceID,
        RE::FormID a_secondCellID, RE::FormID a_secondWorldspaceID)
    {
        if (a_firstWorldspaceID != 0 || a_secondWorldspaceID != 0)
        {
            return a_firstWorldspaceID != 0 && a_firstWorldspaceID == a_secondWorldspaceID;
        }

        return a_firstCellID == a_secondCellID;
    }

    static_assert(IsSameCoordinateSpace(1, 0, 1, 0));
    static_assert(!IsSameCoordinateSpace(1, 0, 2, 0));
    static_assert(IsSameCoordinateSpace(1, 3, 2, 3));
    static_assert(!IsSameCoordinateSpace(1, 3, 1, 4));
    static_assert(!IsSameCoordinateSpace(1, 0, 1, 3));

    [[nodiscard]] std::uint32_t GetWorldspaceID(const RE::TESObjectREFR& a_reference);

    struct LocalSample
    {
        RE::NiPoint3 position{};
        float heading{ 0.0F };
        float pitch{ 0.0F };
        float speed{ 0.0F };
        float direction{ 0.0F };
        bool swimming{ false };
        RE::FormID cellID{ 0 };
        RE::FormID worldspaceID{ 0 };
    };

    [[nodiscard]] LocalSample SampleLocalMovement(
        const LocalFrame& a_local, RE::Actor& a_carrier, const RE::TESObjectCELL& a_cell, float a_snapshotElapsed);

    [[nodiscard]] bool IsInLocalSpace(
        const RemoteFrame& a_remote, RE::FormID a_localCellID, RE::FormID a_localWorldspaceID);

    void SyncMovement(const RemoteFrame& a_remote, std::uint64_t& a_appliedSequence, RemoteActor& a_actor);
}
