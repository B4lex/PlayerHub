#include "sync/combat/SpellEffectSync.h"
#include "actor/ActorSpellEffects.h"
#include "forms/BeastForms.h"
#include "forms/FormLookup.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace SpellEffectSync
{
    namespace
    {
        constexpr float kSampleSeconds = 0.1F;

        using Multiplayer::FormReference;
        using Multiplayer::SpellEffects;

        // Self delivery has no projectile or beam, and the types are what the player
        // casts from a hand, a scroll, or the power key.
        bool IsShownSpell(const RE::MagicItem& a_spell)
        {
            if (a_spell.GetDelivery() != RE::MagicSystem::Delivery::kSelf)
            {
                return false;
            }
            switch (a_spell.GetSpellType())
            {
                case RE::MagicSystem::SpellType::kSpell:
                case RE::MagicSystem::SpellType::kPower:
                case RE::MagicSystem::SpellType::kLesserPower:
                case RE::MagicSystem::SpellType::kScroll:
                    return true;
                default:
                    return false;
            }
        }

        bool IsVampireLordPowerEffect(const RE::EffectSetting* a_effect)
        {
            for (const auto power : { Forms::VampireLordPower::kMistForm, Forms::VampireLordPower::kBats })
            {
                if (a_effect && a_effect == Forms::GetVampireLordPower(power).effect)
                {
                    return true;
                }
            }
            return false;
        }

        // Mist Form and bats travel as checkpoint flags and have their own presentation.
        bool IsShownEffect(const RE::ActiveEffect& a_effect, std::uint32_t a_playerHandle)
        {
            return !a_effect.flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)
                && a_effect.caster.native_handle() == a_playerHandle && a_effect.spell && IsShownSpell(*a_effect.spell)
                && !IsVampireLordPowerEffect(a_effect.GetBaseObject());
        }

        // An active effect's hit shader and art are reference effects whose controller
        // is an ActiveEffectReferenceEffectController naming that active effect.
        const RE::ActiveEffect* GetControllingEffect(const RE::ReferenceEffect& a_effect)
        {
            static const REL::Relocation<std::uintptr_t> controllerVTable{
                RE::VTABLE_ActiveEffectReferenceEffectController[0]
            };
            const auto* controller = a_effect.controller;
            if (!controller || *reinterpret_cast<const std::uintptr_t*>(controller) != controllerVTable.address())
            {
                return nullptr;
            }
            return static_cast<const RE::ActiveEffectReferenceEffectController*>(controller)->effect;
        }

        template <class T>
        void AddUnique(std::vector<T*>& a_forms, T* a_form)
        {
            if (a_form && std::ranges::find(a_forms, a_form) == a_forms.end())
            {
                a_forms.push_back(a_form);
            }
        }

        template <class T>
        std::vector<FormReference> MakeReferences(const std::vector<T*>& a_forms, std::size_t a_maximum)
        {
            std::vector<FormReference> references;
            references.reserve(a_forms.size());
            for (const auto* form : a_forms)
            {
                // Dynamic forms cannot be referenced and are left out.
                if (auto reference = FormLookup::MakeFormReference(form))
                {
                    references.push_back(std::move(*reference));
                }
            }
            std::ranges::sort(references);
            const auto duplicates = std::ranges::unique(references);
            references.erase(duplicates.begin(), duplicates.end());
            if (references.size() > a_maximum)
            {
                references.resize(a_maximum);
            }
            return references;
        }

        template <class T>
        bool Resolve(const std::vector<FormReference>& a_references, std::vector<T*>& a_forms,
            Multiplayer::PeerSlot a_playerID, std::uint64_t a_revision, std::string_view a_kind)
        {
            a_forms.clear();
            bool complete = true;
            for (const auto& reference : a_references)
            {
                if (auto* form = FormLookup::ResolveForm<T>(reference))
                {
                    a_forms.push_back(form);
                    continue;
                }
                complete = false;
                logs::debug("Spell effect sync could not resolve remote {}: player={} revision={} form={}|{:06X}",
                    a_kind, a_playerID, static_cast<unsigned long long>(a_revision), reference.plugin,
                    reference.localFormID);
            }
            return complete;
        }
    }

    const SpellEffects& SampleLocalSpellEffects(const LocalFrame& a_local, float a_delta)
    {
        static float sampleElapsed = kSampleSeconds;
        static SpellEffects sampled;
        // Pointers only compared with the next sample's, never dereferenced.
        static std::vector<RE::TESEffectShader*> sampledShaders;
        static std::vector<RE::BGSArtObject*> sampledArt;
        static std::vector<const RE::ActiveEffect*> shownEffects;
        static std::vector<RE::TESEffectShader*> shaders;
        static std::vector<RE::BGSArtObject*> art;

        sampleElapsed += FrameTime::Clamp(a_delta);
        if (sampleElapsed < kSampleSeconds)
        {
            return sampled;
        }
        sampleElapsed = 0.0F;
        PROFILE_FUNCTION();

        auto& player = a_local.player;
        const auto playerHandle = player.GetHandle().native_handle();
        shownEffects.clear();
        shaders.clear();
        art.clear();
        auto* magicTarget = player.AsMagicTarget();
        if (auto* activeEffects = magicTarget ? magicTarget->GetActiveEffectList() : nullptr)
        {
            for (const auto* activeEffect : *activeEffects)
            {
                if (activeEffect && IsShownEffect(*activeEffect, playerHandle))
                {
                    shownEffects.push_back(activeEffect);
                }
            }
        }
        auto* processLists = shownEffects.empty() ? nullptr : RE::ProcessLists::GetSingleton();
        if (processLists)
        {
            processLists->ForEachMagicTempEffect([&](RE::BSTempEffect* a_effect) {
                auto* referenceEffect = a_effect ? a_effect->As<RE::ReferenceEffect>() : nullptr;
                if (!referenceEffect || referenceEffect->finished
                    || referenceEffect->target.native_handle() != playerHandle
                    || std::ranges::find(shownEffects, GetControllingEffect(*referenceEffect)) == shownEffects.end())
                {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                if (auto* shader = a_effect->As<RE::ShaderReferenceEffect>())
                {
                    if (!shader->IsInterfaceEffect())
                    {
                        AddUnique(shaders, shader->effectData);
                    }
                }
                else if (auto* model = a_effect->As<RE::ModelReferenceEffect>())
                {
                    AddUnique(art, model->artObject);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        std::ranges::sort(shaders);
        std::ranges::sort(art);
        if (shaders == sampledShaders && art == sampledArt)
        {
            return sampled;
        }
        sampledShaders = shaders;
        sampledArt = art;
        sampled = SpellEffects{ .shaders = MakeReferences(shaders, SpellEffects::kMaximumShaders),
            .art = MakeReferences(art, SpellEffects::kMaximumArt) };
        logs::debug("Spell effect sync sampled local spell effects: shaders={}/{} art={}/{} activeEffects={}",
            sampled.shaders.size(), shaders.size(), sampled.art.size(), art.size(), shownEffects.size());
        return sampled;
    }

    void SyncSpellEffects(RemoteState& a_state, const Multiplayer::PlayerEquipment* a_latest,
        Multiplayer::PeerSlot a_playerID, RemoteActor& a_actor)
    {
        if (a_latest && a_latest->revision != a_state.resolvedRevision)
        {
            a_state.resolvedRevision = a_latest->revision;
            if (a_latest->spellEffects != a_state.resolvedEffects)
            {
                a_state.resolvedEffects = a_latest->spellEffects;
                const bool shaders = Resolve(
                    a_state.resolvedEffects.shaders, a_state.shaders, a_playerID, a_latest->revision, "shader");
                const bool art
                    = Resolve(a_state.resolvedEffects.art, a_state.art, a_playerID, a_latest->revision, "art");
                logs::debug(
                    "Spell effect sync resolved remote spell effects: player={} revision={} shaders={}/{} "
                    "art={}/{} complete={}",
                    a_playerID, static_cast<unsigned long long>(a_latest->revision), a_state.shaders.size(),
                    a_state.resolvedEffects.shaders.size(), a_state.art.size(), a_state.resolvedEffects.art.size(),
                    shaders && art);
            }
        }
        ActorSpellEffects::Show(a_actor, a_state.shaders, a_state.art);
    }
}
