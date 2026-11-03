#include "sync/equipment/SpellStandIn.h"
#include "forms/DynamicForm.h"
#include "forms/FormLookup.h"
#include "runtime/GameThread.h"
#include "sync/equipment/FakeLight.h"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace SpellStandIn
{
    namespace
    {
        using SoundID = RE::MagicSystem::SoundID;

        const Multiplayer::FormReference kPlaceholderSpell{ .plugin = "Skyrim.esm", .localFormID = 0x04D3F2 };

        // Release and hit sounds belong to the cast outcome, which peers never see.
        constexpr std::array kHandSounds{ SoundID::kDrawSheatheLPM, SoundID::kCharge, SoundID::kReadyLoop,
            SoundID::kCastLoop };

        struct Look
        {
            RE::BGSArtObject* castingArt{ nullptr };
            RE::TESObjectLIGH* castingLight{ nullptr };
            std::array<RE::BGSSoundDescriptorForm*, kHandSounds.size()> sounds{};
        };

        // The spell data and effect keywords the idle conditions of the hand's attack
        // action read to pick the cast clips: GetCurrentCastingType,
        // GetCurrentDeliveryType, SpellHasKeyword (MagicWard, MagicTelekinesis,
        // RitualSpellEffect, RitualSpellIllusion), and the charge time the caster
        // waits before its ready state.
        struct Shape
        {
            RE::MagicSystem::CastingType castingType{ RE::MagicSystem::CastingType::kFireAndForget };
            RE::MagicSystem::Delivery delivery{ RE::MagicSystem::Delivery::kSelf };
            float chargeTime{ 0.0F };
            // Ordered by form ID, without duplicates.
            std::vector<RE::BGSKeyword*> keywords;
        };

        // Form IDs rather than pointers, which have no portable ordering.
        struct StandInKey
        {
            std::array<RE::FormID, 2 + kHandSounds.size()> look{};
            std::uint32_t castingType{ 0 };
            std::uint32_t delivery{ 0 };
            std::uint32_t chargeTime{ 0 };
            std::vector<RE::FormID> keywords;

            auto operator<=>(const StandInKey&) const = default;
        };

        StandInKey MakeKey(const Look& a_look, const Shape& a_shape)
        {
            const auto id = [](const RE::TESForm* a_form) { return a_form ? a_form->GetFormID() : 0; };
            StandInKey key{ .look = { id(a_look.castingArt), id(a_look.castingLight) },
                .castingType = static_cast<std::uint32_t>(a_shape.castingType),
                .delivery = static_cast<std::uint32_t>(a_shape.delivery),
                .chargeTime = std::bit_cast<std::uint32_t>(a_shape.chargeTime) };
            for (std::size_t i = 0; i < kHandSounds.size(); ++i)
            {
                key.look[2 + i] = id(a_look.sounds[i]);
            }
            key.keywords.reserve(a_shape.keywords.size());
            for (const auto* keyword : a_shape.keywords)
            {
                key.keywords.push_back(keyword->GetFormID());
            }
            return key;
        }

        Shape ReadShape(const RE::MagicItem& a_spell)
        {
            // A hand casts only fire-and-forget and concentration spells; a scroll
            // casts as fire-and-forget.
            Shape shape{ .castingType = a_spell.GetCastingType() == RE::MagicSystem::CastingType::kConcentration
                    ? RE::MagicSystem::CastingType::kConcentration
                    : RE::MagicSystem::CastingType::kFireAndForget,
                .delivery = a_spell.GetDelivery(),
                .chargeTime = (std::max)(a_spell.GetChargeTime(), 0.0F) };
            for (const auto* entry : a_spell.effects)
            {
                if (!entry || !entry->baseEffect)
                {
                    continue;
                }
                for (auto* keyword : entry->baseEffect->GetKeywords())
                {
                    if (keyword)
                    {
                        shape.keywords.push_back(keyword);
                    }
                }
            }
            std::ranges::sort(shape.keywords, {}, &RE::BGSKeyword::GetFormID);
            const auto duplicates = std::ranges::unique(shape.keywords);
            shape.keywords.erase(duplicates.begin(), duplicates.end());
            return shape;
        }

        RE::BGSSoundDescriptorForm* FindSound(const RE::EffectSetting& a_effect, SoundID a_id)
        {
            for (const auto& pair : a_effect.effectSounds)
            {
                if (pair.id == a_id)
                {
                    return pair.sound;
                }
            }
            return nullptr;
        }

        void ReadSounds(const RE::EffectSetting* a_effect, Look& a_look)
        {
            for (std::size_t i = 0; i < kHandSounds.size(); ++i)
            {
                a_look.sounds[i] = a_effect ? FindSound(*a_effect, kHandSounds[i]) : nullptr;
            }
        }

        // The magic caster takes the hand art, hand light, and hand sounds from the
        // selected spell's MagicItem::GetAVEffect (ID 11302): the costliest effect,
        // cached in avEffectSetting. Art via ID 38784, light in
        // ActorMagicCaster::CheckAttachCastingArt, sounds via ID 34438.
        Look ReadLook(const RE::EffectSetting* a_effect)
        {
            Look look{ .castingArt = a_effect ? a_effect->data.castingArt : nullptr,
                .castingLight = a_effect ? a_effect->data.light : nullptr };
            ReadSounds(a_effect, look);
            return look;
        }

        std::optional<Look> ResolveSentLook(
            const Multiplayer::EquipmentHand& a_hand, const RE::EffectSetting& a_placeholder)
        {
            if (a_hand.castingArt.IsEmpty() && a_hand.castingLight.IsEmpty())
            {
                return std::nullopt;
            }
            Look look;
            if (!a_hand.castingArt.IsEmpty())
            {
                look.castingArt = FormLookup::ResolveForm<RE::BGSArtObject>(a_hand.castingArt);
                if (!look.castingArt)
                {
                    return std::nullopt;
                }
            }
            if (!a_hand.castingLight.IsEmpty())
            {
                look.castingLight = FormLookup::ResolveForm<RE::TESObjectLIGH>(a_hand.castingLight);
                if (!look.castingLight)
                {
                    return std::nullopt;
                }
            }
            ReadSounds(std::addressof(a_placeholder), look);
            return look;
        }

        // What a hand casts: fire-and-forget and concentration spells, not powers or
        // abilities, which the source never holds in a hand anyway.
        bool IsHandSpell(const RE::SpellItem& a_spell)
        {
            const auto castingType = a_spell.GetCastingType();
            return a_spell.GetSpellType() == RE::MagicSystem::SpellType::kSpell
                && (castingType == RE::MagicSystem::CastingType::kFireAndForget
                    || castingType == RE::MagicSystem::CastingType::kConcentration);
        }

        // MagicItem::Copy (ID 11297) and EffectSetting::Copy (ID 11059) copy the
        // record data and effects, but not the equip slot or the effect's sounds and
        // keywords.
        RE::SpellItem* CreateStandIn(RE::SpellItem& a_placeholder, RE::EffectSetting& a_placeholderEffect,
            const Look& a_look, const Shape& a_shape)
        {
            auto* effect = DynamicForm::Create<RE::EffectSetting>();
            auto* spell = effect ? DynamicForm::Create<RE::SpellItem>() : nullptr;
            if (!spell)
            {
                logs::error("Spell stand-in could not be created: effect={:08X}", effect ? effect->GetFormID() : 0);
                return nullptr;
            }

            effect->Copy(std::addressof(a_placeholderEffect));
            effect->data.castingType = a_shape.castingType;
            effect->data.delivery = a_shape.delivery;
            // SpellItem::GetChargeTime (ID 11496) returns the largest spellmaking
            // charge time of the effects for an auto-calculated spell, which the
            // placeholder is, and the spell's own charge time otherwise.
            effect->data.spellmakingChargeTime = a_shape.chargeTime;
            if (!a_shape.keywords.empty())
            {
                effect->AddKeywords(a_shape.keywords);
            }
            effect->data.castingArt = a_look.castingArt;
            effect->data.light
                = a_look.castingLight ? std::addressof(FakeLight::Get(*a_look.castingLight)) : nullptr;
            effect->effectSounds.clear();
            for (std::size_t i = 0; i < kHandSounds.size(); ++i)
            {
                if (a_look.sounds[i])
                {
                    effect->effectSounds.push_back(
                        RE::EffectSetting::SoundPair{ .id = kHandSounds[i], .sound = a_look.sounds[i] });
                }
            }

            spell->Copy(std::addressof(a_placeholder));
            spell->SetEquipSlot(a_placeholder.GetEquipSlot());
            spell->data.castingType = a_shape.castingType;
            spell->data.delivery = a_shape.delivery;
            spell->data.chargeTime = a_shape.chargeTime;
            for (auto* entry : spell->effects)
            {
                if (entry && entry->baseEffect == std::addressof(a_placeholderEffect))
                {
                    entry->baseEffect = effect;
                }
            }
            spell->avEffectSetting = nullptr;

            const auto id = [](const RE::TESForm* a_form) { return a_form ? a_form->GetFormID() : 0; };
            logs::debug(
                "Spell stand-in created: spell={:08X} effect={:08X} castingArt={:08X} castingLight={:08X} "
                "sounds={:08X},{:08X},{:08X},{:08X} castingType={} delivery={} chargeTime={:.3f} keywords={}",
                spell->GetFormID(), effect->GetFormID(), id(a_look.castingArt), id(a_look.castingLight),
                id(a_look.sounds[0]), id(a_look.sounds[1]), id(a_look.sounds[2]), id(a_look.sounds[3]),
                static_cast<std::uint32_t>(a_shape.castingType), static_cast<std::uint32_t>(a_shape.delivery),
                a_shape.chargeTime, a_shape.keywords.size());
            return spell;
        }

        RE::SpellItem* GetStandIn(RE::SpellItem& a_placeholder, RE::EffectSetting& a_placeholderEffect,
            const Look& a_look, const Shape& a_shape)
        {
            // Game thread only; stand-ins are never deleted.
            static std::map<StandInKey, RE::SpellItem*> standIns;
            auto key = MakeKey(a_look, a_shape);
            if (const auto found = standIns.find(key); found != standIns.end())
            {
                return found->second;
            }
            auto* spell = CreateStandIn(a_placeholder, a_placeholderEffect, a_look, a_shape);
            if (spell)
            {
                standIns.emplace(std::move(key), spell);
            }
            return spell;
        }
    }

    Multiplayer::EquipmentHand Capture(const RE::MagicItem& a_spell, std::uint32_t& a_skipped)
    {
        Multiplayer::EquipmentHand hand{ .kind = Multiplayer::EquipmentHandKind::kSpell };
        if (auto reference = FormLookup::MakeFormReference(std::addressof(a_spell)))
        {
            hand.form = std::move(*reference);
        }
        else
        {
            ++a_skipped;
        }

        const auto look = ReadLook(a_spell.GetAVEffect());
        const auto art = look.castingArt ? FormLookup::MakeFormReference(look.castingArt) : std::nullopt;
        const auto light = look.castingLight ? FormLookup::MakeFormReference(look.castingLight) : std::nullopt;
        // A partial look would read as a spell without that part.
        if ((look.castingArt && !art) || (look.castingLight && !light))
        {
            ++a_skipped;
            return hand;
        }
        if (art)
        {
            hand.castingArt = *art;
        }
        if (light)
        {
            hand.castingLight = *light;
        }
        return hand;
    }

    Resolution Resolve(const Multiplayer::EquipmentHand& a_hand, bool a_allowNative)
    {
        if (!GameThread::Require("SpellStandIn::Resolve"))
        {
            return { .dropReason = "off-game-thread" };
        }
        auto* placeholder = FormLookup::ResolveForm<RE::SpellItem>(kPlaceholderSpell);
        auto* placeholderEffect = placeholder ? placeholder->GetAVEffect() : nullptr;
        if (!placeholderEffect)
        {
            return { .dropReason = "missing-placeholder-spell" };
        }

        std::string_view source = "spell";
        std::string_view dropReason;
        std::optional<Look> look;
        std::optional<Shape> shape;
        if (a_hand.form.IsEmpty())
        {
            dropReason = "unsent";
        }
        else if (auto* form = FormLookup::ResolveForm(a_hand.form); !form)
        {
            dropReason = "missing";
        }
        else if (!form->Is(RE::FormType::Spell) && !form->Is(RE::FormType::Scroll))
        {
            dropReason = "wrong-type";
        }
        else
        {
            // A scroll is an inventory item that EquipSpell does not take and its
            // cast consumes, so it keeps a stand-in.
            auto* nativeSpell = form->As<RE::SpellItem>();
            if (a_allowNative && nativeSpell && IsHandSpell(*nativeSpell))
            {
                return { .spell = nativeSpell, .source = "native" };
            }
            const auto* spell = form->As<RE::MagicItem>();
            look = ReadLook(spell->GetAVEffect());
            shape = ReadShape(*spell);
        }
        if (!look)
        {
            source = "look";
            look = ResolveSentLook(a_hand, *placeholderEffect);
        }
        if (!look)
        {
            // A copy of the placeholder rather than the placeholder itself, which
            // would cast natively.
            source = "placeholder";
            look = ReadLook(placeholderEffect);
        }
        if (!shape)
        {
            shape = ReadShape(*placeholder);
        }
        auto* standIn = GetStandIn(*placeholder, *placeholderEffect, *look, *shape);
        if (!standIn)
        {
            return { .dropReason = "stand-in-failed" };
        }
        return { .spell = standIn, .source = source, .dropReason = dropReason };
    }
}
