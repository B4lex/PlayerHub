#pragma once

#include <RE/Skyrim.h>

#include <cstdint>

namespace GameplayPolicy
{
    // What a remote actor's effect or hit comes from, which picks the [Gameplay]
    // setting that governs it.
    enum class Source : std::uint8_t
    {
        kSpell,
        kShout,
        kAttack,
    };

    // Main loop only. The queries below are safe from any thread.
    void Update();

    [[nodiscard]] bool ShowsSpellEffects();
    [[nodiscard]] bool ShowsShoutEffects();
    [[nodiscard]] bool ShowsImpactEffects();
    [[nodiscard]] bool AttacksReachPlayer();
    [[nodiscard]] bool DisablesCollision();

    [[nodiscard]] Source Classify(const RE::MagicItem* a_item);
    [[nodiscard]] Source Classify(const RE::Projectile& a_projectile);
    [[nodiscard]] Source Classify(RE::Explosion& a_explosion);
    // A reference acting for a remote actor: the actor itself, or its projectile,
    // explosion, or hazard.
    [[nodiscard]] Source Classify(RE::TESObjectREFR& a_source);

    // Whether a remote source may act on the local world beyond the player.
    [[nodiscard]] bool MayAffectWorld(Source a_source);
    // Whether a remote source may act on a_target, a local reference, or on a
    // place when a_target is null.
    [[nodiscard]] bool MayAffect(Source a_source, const RE::TESObjectREFR* a_target);
    // Whether a native spell fire of a remote actor shows what the spell does.
    [[nodiscard]] bool ShowsNativeFire(const RE::MagicItem* a_spell);
}
