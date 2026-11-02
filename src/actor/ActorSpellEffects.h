#pragma once

#include <RE/Skyrim.h>

#include <span>
#include <vector>

struct RemoteActor;

namespace ActorSpellEffects
{
    struct State
    {
        std::vector<RE::TESEffectShader*> shaders;
        std::vector<RE::BGSArtObject*> art;
        // Identity only, never dereferenced.
        const RE::NiAVObject* root{ nullptr };
    };

    // Plays each shader and art on the actor until it leaves the list; each list
    // holds a form at most once.
    void Show(RemoteActor& a_remote, std::span<RE::TESEffectShader* const> a_shaders,
        std::span<RE::BGSArtObject* const> a_art);
}
