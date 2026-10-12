#pragma once

#include "protocol/PeerSlot.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

struct PeerState;

namespace NameplateSync
{
    struct LabelRevision
    {
        std::uint64_t roster{ 0 };
        std::uint64_t filters{ 0 };

        bool operator==(const LabelRevision&) const = default;
    };

    struct RemoteState
    {
        // Held so the root's address cannot be reused by another actor's 3D while it is compared.
        RE::NiPointer<RE::NiAVObject> root;
        RE::NiPointer<RE::NiAVObject> head;
        std::string label;
        bool isFriend{ false };
        std::optional<LabelRevision> labelRevision;
        bool inSight{ false };
        float sightCheckRemaining{ 0.0F };
        float alpha{ 0.0F };
    };

    struct Plate
    {
        // Both are owned by the peer's RemoteState and stay valid until the next pass.
        const RE::NiAVObject* head{ nullptr };
        std::string_view label;
        bool isFriend{ false };
        float alpha{ 0.0F };
        float distanceFraction{ 0.0F };
    };

    void Clear();

    void Update(std::unordered_map<Multiplayer::PeerSlot, PeerState>& a_peers, RE::PlayerCharacter& a_player,
        float a_delta);

    [[nodiscard]] std::span<const Plate> GetPlates();
}
