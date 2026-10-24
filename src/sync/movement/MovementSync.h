#pragma once

#include "protocol/FormReference.h"
#include "protocol/MovementState.h"
#include "protocol/PlayerState.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>

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

    [[nodiscard]] bool IsSameCoordinateSpace(
        const Network::PlayerTransform& a_first, const Network::PlayerTransform& a_second);

    [[nodiscard]] std::uint32_t GetWorldspaceID(const RE::TESObjectREFR& a_reference);

    struct LocalSample
    {
        RE::NiPoint3 position{};
        float heading{ 0.0F };
        float pitch{ 0.0F };
        float measuredSpeed{ 0.0F };
        std::optional<float> graphSpeed{};
        float direction{ 0.0F };
        bool swimming{ false };
        bool grounded{ false };
        bool relocated{ false };
        RE::FormID cellID{ 0 };
        RE::FormID worldspaceID{ 0 };
    };

    [[nodiscard]] LocalSample SampleLocalMovement(
        const LocalFrame& a_local, RE::Actor& a_carrier, const RE::TESObjectCELL& a_cell, float a_snapshotElapsed);

    [[nodiscard]] float GetCheckpointSpeed(const LocalSample& a_sample, Multiplayer::MovementState a_state);

    struct LocalSpace
    {
        Multiplayer::FormReference interiorCell;
        Multiplayer::FormReference worldspace;
    };

    [[nodiscard]] const LocalSpace& SampleLocalSpace(const LocalSample& a_sample);

    struct RemoteState
    {
        RE::FormID cellID{ 0 };
        RE::FormID worldspaceID{ 0 };
    };

    void OnCheckpointPlayed(RemoteState& a_state, const CheckpointUpdate& a_update);

    [[nodiscard]] bool IsInLocalSpace(
        const RemoteFrame& a_remote, RE::FormID a_localCellID, RE::FormID a_localWorldspaceID);

    void SyncMovement(const RemoteFrame& a_remote, std::uint64_t& a_appliedSequence, RemoteActor& a_actor);
}
