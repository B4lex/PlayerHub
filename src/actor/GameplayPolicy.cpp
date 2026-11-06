#include "actor/GameplayPolicy.h"
#include "config/PluginConfig.h"

#include <atomic>
#include <optional>

namespace GameplayPolicy
{
    namespace
    {
        constexpr Config::GameplaySettings kDefaults{};

        // Written by Update on the main loop and read by hooks on job and AI threads.
        // Each flag stands alone, so relaxed order is enough.
        std::atomic_bool s_showSpellEffects{ kDefaults.showSpellEffects };
        std::atomic_bool s_showShoutEffects{ kDefaults.showShoutEffects };
        std::atomic_bool s_isolateSpellEffects{ kDefaults.isolateSpellEffects };
        std::atomic_bool s_isolateShoutEffects{ kDefaults.isolateShoutEffects };
        std::atomic_bool s_disableCollision{ kDefaults.disableCollision };

        std::optional<std::uint64_t> s_configRevision;
        std::optional<Config::GameplaySettings> s_applied;

        bool Load(const std::atomic_bool& a_flag)
        {
            return a_flag.load(std::memory_order_relaxed);
        }

        void Store(std::atomic_bool& a_flag, bool a_value)
        {
            a_flag.store(a_value, std::memory_order_relaxed);
        }
    }

    void Update()
    {
        const auto revision = Config::GetRevision();
        if (s_configRevision == revision)
        {
            return;
        }
        s_configRevision = revision;
        const auto settings = Config::Get()->gameplay;
        if (s_applied == settings)
        {
            return;
        }
        s_applied = settings;
        Store(s_showSpellEffects, settings.showSpellEffects);
        Store(s_showShoutEffects, settings.showShoutEffects);
        Store(s_isolateSpellEffects, settings.isolateSpellEffects);
        Store(s_isolateShoutEffects, settings.isolateShoutEffects);
        Store(s_disableCollision, settings.disableCollision);
        logs::debug("Gameplay policy: showSpellEffects={} showShoutEffects={} isolateSpellEffects={} "
                    "isolateShoutEffects={} disableCollision={}",
            settings.showSpellEffects, settings.showShoutEffects, settings.isolateSpellEffects,
            settings.isolateShoutEffects, settings.disableCollision);
    }

    bool ShowsSpellEffects()
    {
        return Load(s_showSpellEffects);
    }

    bool ShowsShoutEffects()
    {
        return Load(s_showShoutEffects);
    }

    bool DisablesCollision()
    {
        return Load(s_disableCollision);
    }

    Source Classify(const RE::MagicItem* a_item)
    {
        if (!a_item)
        {
            return Source::kSpell;
        }
        using SpellType = RE::MagicSystem::SpellType;
        switch (a_item->GetSpellType())
        {
        case SpellType::kVoicePower:
        case SpellType::kPower:
        case SpellType::kLesserPower:
            return Source::kShout;
        // A weapon's or an ammunition's enchantment, an explosion's from exploding
        // ammunition, and a poison reach targets through a hit.
        case SpellType::kEnchantment:
        case SpellType::kPoison:
            return Source::kAttack;
        default:
            return Source::kSpell;
        }
    }

    Source Classify(const RE::Projectile& a_projectile)
    {
        if (a_projectile.GetFormType() == RE::FormType::ProjectileArrow)
        {
            return Source::kAttack;
        }
        return Classify(a_projectile.GetProjectileRuntimeData().spell);
    }

    Source Classify(RE::Explosion& a_explosion)
    {
        const auto& data = a_explosion.GetExplosionRuntimeData();
        if (data.weaponSource)
        {
            return Source::kAttack;
        }
        return Classify(data.magicCaster ? data.magicCaster->currentSpell : nullptr);
    }

    Source Classify(RE::TESObjectREFR& a_source)
    {
        switch (a_source.GetFormType())
        {
        case RE::FormType::ActorCharacter:
            return Source::kAttack;
        case RE::FormType::ProjectileMissile:
        case RE::FormType::ProjectileArrow:
        case RE::FormType::ProjectileGrenade:
        case RE::FormType::ProjectileBeam:
        case RE::FormType::ProjectileFlame:
        case RE::FormType::ProjectileCone:
        case RE::FormType::ProjectileBarrier:
            return Classify(static_cast<const RE::Projectile&>(a_source));
        case RE::FormType::Explosion:
            return Classify(static_cast<RE::Explosion&>(a_source));
        default:
            return Source::kSpell;
        }
    }

    bool MayAffectWorld(Source a_source)
    {
        switch (a_source)
        {
        case Source::kSpell:
            return !Load(s_isolateSpellEffects);
        case Source::kShout:
            return !Load(s_isolateShoutEffects);
        case Source::kAttack:
        default:
            return false;
        }
    }

    bool ShowsNativeFire(const RE::MagicItem* a_spell)
    {
        return Classify(a_spell) == Source::kShout ? ShowsShoutEffects() : ShowsSpellEffects();
    }
}
