#include "actor/SelfEffects.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace SelfEffects
{
    namespace
    {
        using Archetype = RE::EffectSetting::Archetype;

        // Archetypes whose effect stays on the actor it applies to: its actor
        // values, look, light, and a cloak's aura. A cloak applies its spell to
        // others through new AddTarget calls, and a spawned hazard applies its
        // spell the same way, both with the remote actor as their source.
        bool IsSelfContainedArchetype(Archetype a_archetype)
        {
            switch (a_archetype)
            {
            case Archetype::kValueModifier:
            case Archetype::kDualValueModifier:
            case Archetype::kPeakValueModifier:
            case Archetype::kValueAndParts:
            case Archetype::kAccumulateMagnitude:
            case Archetype::kInvisibility:
            case Archetype::kLight:
            case Archetype::kNightEye:
            case Archetype::kEtherealize:
            case Archetype::kDisguise:
            case Archetype::kCloak:
            case Archetype::kSpawnHazard:
                return true;
            default:
                return false;
            }
        }

        // The vanilla hazards a shout or power spawns run scripts on the caster that
        // change the world: Storm Call's set the weather (magicSetWeatherScript) and
        // place activators that cast lightning (magicProjectileStormScript), and the Wind
        // Stone power's places an activator (magicPlaceActivatorScript).
        struct EffectReference
        {
            std::string_view plugin;
            RE::FormID localFormID;
        };

        // Self effects of an allowlisted archetype whose scripts reach past the caster.
        // VoiceMakeEthereal's magicImodScript applies its image space modifiers to the
        // local screen whichever actor the effect is on.
        constexpr std::array kIgnoredEffects{
            EffectReference{ .plugin = "Skyrim.esm", .localFormID = 0x064D68 },
        };

        // kScript effects a player can have that show and whose scripts act on the
        // actor alone: none, magicSetActorAlphaScript (SetAlpha on the target), or
        // magicSoundPlayIntroOutro (sounds played at the target). USSEP's
        // USSEP_TG09ShadowCloakScript on 0x017121 only dispels the caster's own
        // TGNightingaleShadow when the effect finishes.
        constexpr std::array kActorScriptEffects{
            EffectReference{ .plugin = "Skyrim.esm", .localFormID = 0x103647 },      // AbGhostPotionEffect
            EffectReference{ .plugin = "Skyrim.esm", .localFormID = 0x017121 },      // TGNightingaleShadowFFSelf
            EffectReference{ .plugin = "Skyrim.esm", .localFormID = 0x0E6DF4 },      // BladesAlchPotion
            EffectReference{ .plugin = "Skyrim.esm", .localFormID = 0x0F5FF6 },      // MarriageFoodMagicEffect
            EffectReference{ .plugin = "Dragonborn.esm", .localFormID = 0x01E7FF },  // DLC2BlackBookNoDamageEffect
            EffectReference{ .plugin = "Dragonborn.esm", .localFormID = 0x01E7FC },  // DLC2BlackBookNoMagickaEffect
            EffectReference{ .plugin = "Dragonborn.esm", .localFormID = 0x01E7F9 },  // DLC2BlackBookNoStaminaEffect
            EffectReference{ .plugin = "Dragonborn.esm", .localFormID = 0x034833 },  // DLC2BlackBookSeekerCombatEffect
            EffectReference{ .plugin = "Dragonborn.esm", .localFormID = 0x034835 },  // DLC2BlackBookSeekerMagicEffect
            EffectReference{ .plugin = "Dragonborn.esm", .localFormID = 0x034836 },  // DLC2BlackBookSeekerStealthEffect
            EffectReference{ .plugin = "Dragonborn.esm", .localFormID = 0x01DFAA },  // DLC2StandingStoneTreeEffect
            EffectReference{ .plugin = "Dragonborn.esm", .localFormID = 0x029A25 },  // DLC2ApocryphaRewardCastingEffect
        };

        template <std::size_t N>
        std::array<RE::FormID, N> LookupEffects(const std::array<EffectReference, N>& a_references)
        {
            std::array<RE::FormID, N> formIDs{};
            if (auto* dataHandler = RE::TESDataHandler::GetSingleton())
            {
                for (std::size_t index = 0; index < N; ++index)
                {
                    const auto& entry = a_references[index];
                    formIDs[index] = dataHandler->LookupFormID(entry.localFormID, entry.plugin);
                }
            }
            return formIDs;
        }

        // Remote magic reaches a target, and the player's effects are captured, only
        // after the data has loaded.
        bool IsIgnoredEffect(const RE::EffectSetting& a_effect)
        {
            static const auto formIDs = LookupEffects(kIgnoredEffects);
            return std::ranges::find(formIDs, a_effect.GetFormID()) != formIDs.end();
        }

        bool IsActorScriptEffect(const RE::EffectSetting& a_effect)
        {
            static const auto formIDs = LookupEffects(kActorScriptEffects);
            return a_effect.GetArchetype() == Archetype::kScript
                && std::ranges::find(formIDs, a_effect.GetFormID()) != formIDs.end();
        }

        bool IsSelfContainedEffect(const RE::EffectSetting& a_effect)
        {
            return IsSelfContainedArchetype(a_effect.GetArchetype()) || IsActorScriptEffect(a_effect);
        }

        // Lights, invisibility's refraction, and the ethereal shader come from the
        // archetype's own ActiveEffect; other archetypes show through the effect's
        // shaders and art.
        bool HasLook(const RE::EffectSetting& a_effect)
        {
            switch (a_effect.GetArchetype())
            {
            case Archetype::kInvisibility:
            case Archetype::kLight:
            case Archetype::kEtherealize:
                return true;
            default:
                break;
            }
            const auto& data = a_effect.data;
            return data.effectShader || data.enchantShader || data.hitEffectArt || data.enchantEffectArt;
        }
    }

    bool IsVoiceOrPower(const RE::MagicItem* a_item)
    {
        if (!a_item)
        {
            return false;
        }
        using SpellType = RE::MagicSystem::SpellType;
        const auto type = a_item->GetSpellType();
        return type == SpellType::kVoicePower || type == SpellType::kPower || type == SpellType::kLesserPower;
    }

    bool IsSelfContained(const RE::EffectSetting& a_effect, const RE::MagicItem* a_item)
    {
        const auto archetype = a_effect.GetArchetype();
        return IsSelfContainedEffect(a_effect)
            && !(archetype == Archetype::kSpawnHazard && IsVoiceOrPower(a_item)) && !IsIgnoredEffect(a_effect);
    }

    bool IsPresentationOwned(const RE::EffectSetting& a_effect)
    {
        switch (a_effect.GetArchetype())
        {
        case Archetype::kBoundWeapon:
        case Archetype::kWerewolf:
        case Archetype::kVampireLord:
            return true;
        default:
            return IsIgnoredEffect(a_effect);
        }
    }

    bool IsPresented(const RE::EffectSetting& a_effect)
    {
        // A concentration effect lasts only while its cast does, which the cast
        // replay shows. The magic firewall drops hostile and detrimental effects on a
        // remote actor, and a spawned hazard is placed once when its effect starts.
        using CastingType = RE::MagicSystem::CastingType;
        const auto castingType = a_effect.data.castingType;
        const auto archetype = a_effect.GetArchetype();
        return (castingType == CastingType::kConstantEffect || castingType == CastingType::kFireAndForget)
            && !a_effect.IsHostile() && !a_effect.IsDetrimental() && archetype != Archetype::kSpawnHazard
            && IsSelfContainedEffect(a_effect) && !IsPresentationOwned(a_effect) && HasLook(a_effect);
    }
}
