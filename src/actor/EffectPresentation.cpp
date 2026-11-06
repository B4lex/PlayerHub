#include "actor/EffectPresentation.h"
#include "forms/DynamicForm.h"
#include "forms/FormLookup.h"
#include "runtime/GameThread.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace EffectPresentation
{
    namespace
    {
        using CastingType = RE::MagicSystem::CastingType;

        // Candlelight: a fire-and-forget self spell with one effect and no conditions.
        const Multiplayer::FormReference kTemplateSpell{ .plugin = "Skyrim.esm", .localFormID = 0x043324 };

        // A fire-and-forget effect lasts until the source's effects drop it; the
        // receiver reapplies one that ends anyway.
        constexpr std::uint32_t kTimedDurationSeconds = 24 * 60 * 60;

        // Written on the game thread, read by the magic hooks on any thread.
        std::shared_mutex s_spellIDsLock;
        std::unordered_set<RE::FormID> s_spellIDs;

        // Game thread only; the spells are never deleted.
        std::unordered_map<RE::FormID, RE::SpellItem*> s_spells;

        bool IsConstant(const RE::EffectSetting& a_effect)
        {
            return a_effect.data.castingType == CastingType::kConstantEffect;
        }

        // A constant effect is an ability, which Actor::AddSpell applies through the
        // instant caster's FindTargets (ID 34464, then 34410) and RemoveSpell ends. A
        // fire-and-forget effect is a self spell cast through CastSpellImmediate,
        // which stores its magnitude override for FindTargets and clears it after
        // (ID 34404); an override of 0 leaves the spell's own magnitude, also 0.
        // MagicItem::Copy (ID 11297) copies the template's effects, so the entry
        // belongs to the copy.
        RE::SpellItem* CreateSpell(RE::SpellItem& a_template, RE::EffectSetting& a_effect)
        {
            auto* spell = DynamicForm::Create<RE::SpellItem>();
            if (!spell)
            {
                logs::error("Effect presentation spell could not be created: effect={:08X}", a_effect.GetFormID());
                return nullptr;
            }
            spell->Copy(std::addressof(a_template));
            auto* entry = spell->effects.empty() ? nullptr : spell->effects.front();
            if (!entry)
            {
                logs::error("Effect presentation template has no effect: effect={:08X}", a_effect.GetFormID());
                return nullptr;
            }
            const bool constant = IsConstant(a_effect);
            entry->baseEffect = std::addressof(a_effect);
            entry->effectItem.magnitude = 0.0F;
            entry->effectItem.area = 0;
            entry->effectItem.duration = constant ? 0 : kTimedDurationSeconds;
            entry->cost = 0.0F;
            spell->data.spellType = constant ? RE::MagicSystem::SpellType::kAbility : RE::MagicSystem::SpellType::kSpell;
            spell->data.castingType = constant ? CastingType::kConstantEffect : CastingType::kFireAndForget;
            spell->data.delivery = RE::MagicSystem::Delivery::kSelf;
            spell->data.chargeTime = 0.0F;
            spell->avEffectSetting = nullptr;
            {
                const std::unique_lock lock{ s_spellIDsLock };
                s_spellIDs.insert(spell->GetFormID());
            }
            logs::debug("Effect presentation spell created: spell={:08X} effect={:08X} archetype={} constant={}",
                spell->GetFormID(), a_effect.GetFormID(), static_cast<std::int32_t>(a_effect.GetArchetype()),
                constant);
            return spell;
        }

        RE::SpellItem* GetSpell(RE::EffectSetting& a_effect)
        {
            const auto formID = a_effect.GetFormID();
            if (const auto found = s_spells.find(formID); found != s_spells.end())
            {
                return found->second;
            }
            auto* spellTemplate = FormLookup::ResolveForm<RE::SpellItem>(kTemplateSpell);
            if (!spellTemplate)
            {
                logs::error("Effect presentation template spell is missing");
                return nullptr;
            }
            auto* spell = CreateSpell(*spellTemplate, a_effect);
            if (spell)
            {
                s_spells.emplace(formID, spell);
            }
            return spell;
        }

        RE::SpellItem* FindSpell(const RE::EffectSetting& a_effect)
        {
            const auto found = s_spells.find(a_effect.GetFormID());
            return found != s_spells.end() ? found->second : nullptr;
        }

        template <class Visitor>
        void ForEachEffectOf(RE::Actor& a_actor, const RE::SpellItem& a_spell, Visitor&& a_visitor)
        {
            auto* magicTarget = a_actor.AsMagicTarget();
            auto* activeEffects = magicTarget ? magicTarget->GetActiveEffectList() : nullptr;
            if (!activeEffects)
            {
                return;
            }
            for (auto* activeEffect : *activeEffects)
            {
                if (activeEffect && activeEffect->spell == std::addressof(a_spell)
                    && !activeEffect->flags.any(RE::ActiveEffect::Flag::kDispelled))
                {
                    a_visitor(*activeEffect);
                }
            }
        }
    }

    bool IsPresentationSpell(const RE::MagicItem* a_item)
    {
        if (!a_item || !a_item->IsDynamicForm())
        {
            return false;
        }
        const std::shared_lock lock{ s_spellIDsLock };
        return s_spellIDs.contains(a_item->GetFormID());
    }

    bool Apply(RE::Actor& a_actor, RE::EffectSetting& a_effect, float a_magnitude)
    {
        if (!GameThread::Require("EffectPresentation::Apply"))
        {
            return false;
        }
        auto* spell = GetSpell(a_effect);
        if (!spell)
        {
            return false;
        }
        if (IsConstant(a_effect))
        {
            // The ability's magnitude is read when the engine applies it, so a
            // reapplication on another actor can carry another peer's magnitude; no
            // look depends on it.
            spell->effects.front()->effectItem.magnitude = a_magnitude;
            a_actor.RemoveSpell(spell);
            return a_actor.AddSpell(spell);
        }
        auto* caster = a_actor.GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
        if (!caster)
        {
            return false;
        }
        caster->CastSpellImmediate(spell, false, std::addressof(a_actor), 1.0F, false, a_magnitude, std::addressof(a_actor));
        return true;
    }

    void Remove(RE::Actor& a_actor, RE::EffectSetting& a_effect)
    {
        if (!GameThread::Require("EffectPresentation::Remove"))
        {
            return;
        }
        auto* spell = FindSpell(a_effect);
        if (!spell)
        {
            return;
        }
        if (IsConstant(a_effect))
        {
            a_actor.RemoveSpell(spell);
            return;
        }
        std::vector<RE::ActiveEffect*> effects;
        ForEachEffectOf(a_actor, *spell, [&effects](RE::ActiveEffect& a_activeEffect) {
            effects.push_back(std::addressof(a_activeEffect));
        });
        for (auto* activeEffect : effects)
        {
            activeEffect->Dispel(true);
        }
    }

    bool IsApplied(RE::Actor& a_actor, const RE::EffectSetting& a_effect)
    {
        const auto* spell = FindSpell(a_effect);
        bool applied = false;
        if (spell)
        {
            ForEachEffectOf(a_actor, *spell, [&applied](RE::ActiveEffect&) { applied = true; });
        }
        return applied;
    }
}
