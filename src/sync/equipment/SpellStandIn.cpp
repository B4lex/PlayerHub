#include "sync/equipment/SpellStandIn.h"
#include "forms/DynamicForm.h"
#include "forms/FormLookup.h"
#include "runtime/GameThread.h"
#include "sync/equipment/FakeLight.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <utility>

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

        // Form IDs rather than pointers, which have no portable ordering.
        using LookKey = std::array<RE::FormID, 2 + kHandSounds.size()>;

        LookKey MakeKey(const Look& a_look)
        {
            const auto id = [](const RE::TESForm* a_form) { return a_form ? a_form->GetFormID() : 0; };
            LookKey key{ id(a_look.castingArt), id(a_look.castingLight) };
            for (std::size_t i = 0; i < kHandSounds.size(); ++i)
            {
                key[2 + i] = id(a_look.sounds[i]);
            }
            return key;
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

        // MagicItem::Copy (ID 11297) and EffectSetting::Copy (ID 11059) copy the
        // record data and effects, but not the equip slot or the effect's sounds.
        RE::SpellItem* CreateStandIn(
            RE::SpellItem& a_placeholder, RE::EffectSetting& a_placeholderEffect, const Look& a_look)
        {
            auto* effect = DynamicForm::Create<RE::EffectSetting>();
            auto* spell = effect ? DynamicForm::Create<RE::SpellItem>() : nullptr;
            if (!spell)
            {
                logs::error("Spell stand-in could not be created: effect={:08X}", effect ? effect->GetFormID() : 0);
                return nullptr;
            }

            effect->Copy(std::addressof(a_placeholderEffect));
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
                "sounds={:08X},{:08X},{:08X},{:08X}",
                spell->GetFormID(), effect->GetFormID(), id(a_look.castingArt), id(a_look.castingLight),
                id(a_look.sounds[0]), id(a_look.sounds[1]), id(a_look.sounds[2]), id(a_look.sounds[3]));
            return spell;
        }

        RE::SpellItem* GetStandIn(
            RE::SpellItem& a_placeholder, RE::EffectSetting& a_placeholderEffect, const Look& a_look)
        {
            // Game thread only; stand-ins are never deleted.
            static std::map<LookKey, RE::SpellItem*> standIns;
            const auto key = MakeKey(a_look);
            if (const auto found = standIns.find(key); found != standIns.end())
            {
                return found->second;
            }
            auto* spell = CreateStandIn(a_placeholder, a_placeholderEffect, a_look);
            if (spell)
            {
                standIns.emplace(key, spell);
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

    Resolution Resolve(const Multiplayer::EquipmentHand& a_hand)
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
            look = ReadLook(form->As<RE::MagicItem>()->GetAVEffect());
        }
        if (!look)
        {
            source = "look";
            look = ResolveSentLook(a_hand, *placeholderEffect);
        }
        if (!look)
        {
            return { .spell = placeholder, .source = "placeholder", .dropReason = dropReason };
        }
        auto* standIn = GetStandIn(*placeholder, *placeholderEffect, *look);
        if (!standIn)
        {
            return { .spell = placeholder, .source = "placeholder", .dropReason = "stand-in-failed" };
        }
        return { .spell = standIn, .source = source, .dropReason = dropReason };
    }
}
