#include "actor/ActorSpellEffects.h"
#include "actor/RemoteActor.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>

namespace ActorSpellEffects
{
    namespace
    {
        template <class T>
        bool Contains(std::span<T* const> a_forms, const T* a_form)
        {
            return std::ranges::find(a_forms, a_form) != a_forms.end();
        }

        template <class T>
        std::vector<T*> Without(std::span<T* const> a_forms, std::span<T* const> a_removed)
        {
            std::vector<T*> result;
            for (auto* form : a_forms)
            {
                if (!Contains(a_removed, form))
                {
                    result.push_back(form);
                }
            }
            return result;
        }

        // The engine owns and deletes reference effects, so they are found again by
        // target and form rather than kept. Nothing else applies these forms to a
        // remote actor: the magic firewall keeps local spells off it, and the source
        // leaves the vampire lord powers out.
        void End(RE::Actor& a_actor, std::span<RE::TESEffectShader* const> a_shaders,
            std::span<RE::BGSArtObject* const> a_art)
        {
            auto* processLists = RE::ProcessLists::GetSingleton();
            if (!processLists || (a_shaders.empty() && a_art.empty()))
            {
                return;
            }
            const auto handle = a_actor.GetHandle().native_handle();
            processLists->ForEachMagicTempEffect([&](RE::BSTempEffect* a_effect) {
                auto* referenceEffect = a_effect ? a_effect->As<RE::ReferenceEffect>() : nullptr;
                if (!referenceEffect || referenceEffect->target.native_handle() != handle)
                {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                if (auto* shader = a_effect->As<RE::ShaderReferenceEffect>())
                {
                    referenceEffect->finished = referenceEffect->finished || Contains(a_shaders, shader->effectData);
                }
                else if (auto* model = a_effect->As<RE::ModelReferenceEffect>())
                {
                    referenceEffect->finished = referenceEffect->finished || Contains(a_art, model->artObject);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
    }

    void Show(RemoteActor& a_remote, std::span<RE::TESEffectShader* const> a_shaders,
        std::span<RE::BGSArtObject* const> a_art)
    {
        auto& state = a_remote.spellEffects;
        if (a_shaders.empty() && a_art.empty() && state.shaders.empty() && state.art.empty())
        {
            return;
        }
        if (!a_remote.ready || !GameThread::Require("ShowSpellEffects"))
        {
            return;
        }
        auto actorPtr = a_remote.handle.get();
        if (!actorPtr || !actorPtr->Is3DLoaded())
        {
            return;
        }
        auto& actor = *actorPtr;
        const auto* root = actor.Get3D();
        const bool rootChanged = root != state.root;
        if (!rootChanged && std::ranges::equal(state.shaders, a_shaders) && std::ranges::equal(state.art, a_art))
        {
            return;
        }
        PROFILE_FUNCTION();

        // Whether the engine moves reference effects to a new 3D is not known, so they
        // are ended and applied again.
        if (rootChanged)
        {
            End(actor, state.shaders, state.art);
            state.shaders.clear();
            state.art.clear();
            state.root = root;
        }
        const auto endedShaders = Without<RE::TESEffectShader>(state.shaders, a_shaders);
        const auto endedArt = Without<RE::BGSArtObject>(state.art, a_art);
        End(actor, endedShaders, endedArt);

        std::uint32_t started = 0;
        std::uint32_t failed = 0;
        for (auto* shader : a_shaders)
        {
            if (!Contains<RE::TESEffectShader>(state.shaders, shader))
            {
                ++(actor.ApplyEffectShader(shader, -1.0F) ? started : failed);
            }
        }
        for (auto* art : a_art)
        {
            if (!Contains<RE::BGSArtObject>(state.art, art))
            {
                ++(actor.ApplyArtObject(art, -1.0F) ? started : failed);
            }
        }
        // A failed start is not retried until the list or the 3D changes, so it
        // cannot repeat every pass.
        state.shaders.assign(a_shaders.begin(), a_shaders.end());
        state.art.assign(a_art.begin(), a_art.end());
        logs::debug(
            "Spell effect sync updated actor spell effects: actor={:08X} shaders={} art={} started={} "
            "failed={} ended={} newRoot={}",
            a_remote.handle.native_handle(), state.shaders.size(), state.art.size(), started, failed,
            endedShaders.size() + endedArt.size(), rootChanged);
    }
}
