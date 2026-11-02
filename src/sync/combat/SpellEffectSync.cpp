#include "sync/combat/SpellEffectSync.h"
#include "actor/ActorSpellEffects.h"
#include "forms/BeastForms.h"
#include "forms/FormLookup.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

namespace SpellEffectSync
{
    namespace
    {
        constexpr float kSampleSeconds = 0.1F;

        using Multiplayer::FormReference;
        using Multiplayer::SpellEffect;

        // Pointers only compared between samples, never dereferenced afterwards.
        struct ShownEffect
        {
            const RE::EffectSetting* effect{ nullptr };
            const RE::TESEffectShader* shader{ nullptr };
            const RE::BGSArtObject* art{ nullptr };
            bool shaderShown{ false };
            bool artShown{ false };

            bool operator==(const ShownEffect&) const = default;
        };

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

        ShownEffect& FindOrAdd(std::vector<ShownEffect>& a_shown, const RE::EffectSetting* a_effect)
        {
            const auto found = std::ranges::find(a_shown, a_effect, &ShownEffect::effect);
            return found != a_shown.end() ? *found : a_shown.emplace_back(ShownEffect{ .effect = a_effect });
        }

        bool HaveSameEntries(const std::vector<ShownEffect>& a_shown, const std::vector<ShownEffect>& a_other)
        {
            return a_shown.size() == a_other.size() && std::ranges::all_of(a_shown, [&](const ShownEffect& a_entry) {
                return std::ranges::find(a_other, a_entry) != a_other.end();
            });
        }

        FormReference MakeReference(const RE::TESForm* a_form)
        {
            auto reference = a_form ? FormLookup::MakeFormReference(a_form) : std::nullopt;
            return reference ? std::move(*reference) : FormReference{};
        }

        // Dynamic forms cannot be referenced and are left out; an entry left with
        // nothing to resolve is dropped.
        std::vector<SpellEffect> MakeReferences(const std::vector<ShownEffect>& a_shown)
        {
            std::vector<SpellEffect> effects;
            effects.reserve(a_shown.size());
            for (const auto& entry : a_shown)
            {
                SpellEffect effect{ .effect = MakeReference(entry.effect),
                    .shaderShown = entry.shaderShown,
                    .artShown = entry.artShown,
                    .shader = MakeReference(entry.shader),
                    .art = MakeReference(entry.art) };
                if (!effect.effect.IsEmpty() || !effect.shader.IsEmpty() || !effect.art.IsEmpty())
                {
                    effects.push_back(std::move(effect));
                }
            }
            std::ranges::sort(effects);
            const auto duplicates = std::ranges::unique(effects);
            effects.erase(duplicates.begin(), duplicates.end());
            if (effects.size() > Multiplayer::PlayerEquipment::kMaximumSpellEffects)
            {
                effects.resize(Multiplayer::PlayerEquipment::kMaximumSpellEffects);
            }
            return effects;
        }

        template <class T>
        T* ResolveFallback(const FormReference& a_reference, Multiplayer::PeerSlot a_playerID, std::uint64_t a_revision,
            std::string_view a_kind)
        {
            if (a_reference.IsEmpty())
            {
                return nullptr;
            }
            auto* form = FormLookup::ResolveForm<T>(a_reference);
            if (!form)
            {
                logs::debug("Spell effect sync could not resolve remote {}: player={} revision={} form={}|{:06X}",
                    a_kind, a_playerID, static_cast<unsigned long long>(a_revision), a_reference.plugin,
                    a_reference.localFormID);
            }
            return form;
        }
    }

    const std::vector<SpellEffect>& SampleLocalSpellEffects(const LocalFrame& a_local, float a_delta)
    {
        static float sampleElapsed = kSampleSeconds;
        static std::vector<SpellEffect> sampled;
        static std::vector<ShownEffect> sampledShown;
        static std::vector<ShownEffect> shown;
        static std::vector<const RE::ActiveEffect*> shownEffects;

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
        shown.clear();
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
                const auto* activeEffect = referenceEffect && !referenceEffect->finished
                        && referenceEffect->target.native_handle() == playerHandle
                    ? GetControllingEffect(*referenceEffect)
                    : nullptr;
                if (!activeEffect || std::ranges::find(shownEffects, activeEffect) == shownEffects.end())
                {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                if (const auto* shader = a_effect->As<RE::ShaderReferenceEffect>())
                {
                    if (!shader->IsInterfaceEffect() && shader->effectData)
                    {
                        auto& entry = FindOrAdd(shown, activeEffect->GetBaseObject());
                        entry.shaderShown = true;
                        entry.shader = entry.shader ? entry.shader : shader->effectData;
                    }
                }
                else if (const auto* model = a_effect->As<RE::ModelReferenceEffect>())
                {
                    if (model->artObject)
                    {
                        auto& entry = FindOrAdd(shown, activeEffect->GetBaseObject());
                        entry.artShown = true;
                        entry.art = entry.art ? entry.art : model->artObject;
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        if (HaveSameEntries(shown, sampledShown))
        {
            return sampled;
        }
        sampledShown = shown;
        sampled = MakeReferences(shown);
        logs::debug("Spell effect sync sampled local spell effects: effects={}/{} activeEffects={}", sampled.size(),
            shown.size(), shownEffects.size());
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
                a_state.shaders.clear();
                a_state.art.clear();
                std::uint32_t own = 0;
                std::uint32_t fallback = 0;
                for (const auto& entry : a_state.resolvedEffects)
                {
                    // The receiver's version of the effect, so its replacers apply.
                    const auto* effect
                        = entry.effect.IsEmpty() ? nullptr : FormLookup::ResolveForm<RE::EffectSetting>(entry.effect);
                    if (effect)
                    {
                        ++own;
                        if (effect->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kNoHitEffect))
                        {
                            continue;
                        }
                        if (entry.shaderShown)
                        {
                            AddUnique(a_state.shaders, effect->data.effectShader);
                        }
                        if (entry.artShown)
                        {
                            AddUnique(a_state.art, effect->data.hitEffectArt);
                        }
                        continue;
                    }
                    ++fallback;
                    if (entry.shaderShown)
                    {
                        AddUnique(a_state.shaders,
                            ResolveFallback<RE::TESEffectShader>(
                                entry.shader, a_playerID, a_latest->revision, "shader"));
                    }
                    if (entry.artShown)
                    {
                        AddUnique(a_state.art,
                            ResolveFallback<RE::BGSArtObject>(entry.art, a_playerID, a_latest->revision, "art"));
                    }
                }
                logs::debug(
                    "Spell effect sync resolved remote spell effects: player={} revision={} effects={} "
                    "own={} fallback={} shaders={} art={}",
                    a_playerID, static_cast<unsigned long long>(a_latest->revision), a_state.resolvedEffects.size(),
                    own, fallback, a_state.shaders.size(), a_state.art.size());
            }
        }
        ActorSpellEffects::Show(a_actor, a_state.shaders, a_state.art);
    }
}
