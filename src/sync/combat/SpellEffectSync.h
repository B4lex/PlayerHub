#pragma once

#include "protocol/EquipmentState.h"
#include "protocol/PeerSlot.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <vector>

struct RemoteActor;

namespace SpellEffectSync
{
    [[nodiscard]] const Multiplayer::SpellEffects& SampleLocalSpellEffects(const LocalFrame& a_local, float a_delta);

    struct RemoteState
    {
        std::uint64_t resolvedRevision{ 0 };
        Multiplayer::SpellEffects resolvedEffects;
        std::vector<RE::TESEffectShader*> shaders;
        std::vector<RE::BGSArtObject*> art;
    };

    void SyncSpellEffects(RemoteState& a_state, const Multiplayer::PlayerEquipment* a_latest,
        Multiplayer::PeerSlot a_playerID, RemoteActor& a_actor);
}
