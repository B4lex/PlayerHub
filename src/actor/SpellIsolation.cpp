#include "actor/SpellIsolation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "actor/GameplayPolicy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <span>
#include <string_view>

namespace SpellIsolation
{
    namespace
    {
        using ActorConstants::ShouldLogOccurrence;
        using CodeDetour::WriteCalleeDetour;
        using CodeDetour::WriteEntryDetour;
        using Archetype = RE::EffectSetting::Archetype;
        using GameplayPolicy::Source;

        constexpr std::array<std::uint8_t, 5> kSpellFirePrologue{
            0x40, 0x56, // push rsi
            0x57, // push rdi
            0x41, 0x56 // push r14
        };
        constexpr std::array<std::uint8_t, 5> kHitDataPrologue{
            0x48, 0x8B, 0xC4, // mov rax, rsp
            0x55, // push rbp
            0x56 // push rsi
        };
        // Every caller of each function on 1.5.97 and on 1.7.104, found by scanning
        // both executables for calls, jumps, and pointers to it. These functions
        // are not entry-detoured: a plugin that hooks an entry later by copying its
        // first bytes into a gateway, as PapyrusExtender does for ID 44213 at
        // kDataLoaded, would copy our relative branch, which then jumps elsewhere.
        constexpr std::array kSpellImpactCallers{
            RELOCATION_ID(33633, 34411),
            RELOCATION_ID(42943, 44123),
            RELOCATION_ID(43015, 44206),
        };
        constexpr std::array kDestructibleCallers{
            RELOCATION_ID(14069, 14168),
            RELOCATION_ID(25298, 25818),
            RELOCATION_ID(42943, 44123),
            RELOCATION_ID(55578, 56107),
        };
        constexpr std::array kImpactImpulseCallers{ RELOCATION_ID(42943, 44123) };
        constexpr std::array kExplosionUpdateCallers{ RELOCATION_ID(42679, 43851) };
        // SE's FindTargets (ID 33632) reaches the launch only through ID 33670; an SE
        // ID of 0 marks a caller only AE has.
        constexpr std::array kSpellLaunchCallers{
            RELOCATION_ID(0, 34410),
            RELOCATION_ID(33670, 34450),
            RELOCATION_ID(33671, 34451),
        };
        // The cone's impact pass (ID 43806) also calls the impact effects, but a
        // remote cone records no impacts for it, since ConeHitsHook skips its HandleHits.
        constexpr std::array kImpactEffectsCallers{
            RELOCATION_ID(42943, 44123),
            RELOCATION_ID(43014, 44205),
        };
        constexpr std::array kProjectileUpdateCallers{ RELOCATION_ID(43025, 44216) };

        static_assert(kSpellImpactCallers.size() + kDestructibleCallers.size() + kImpactImpulseCallers.size()
                + 2 * kExplosionUpdateCallers.size() + kSpellLaunchCallers.size() + kImpactEffectsCallers.size()
                + kProjectileUpdateCallers.size()
            == kCallDetourCount);
        static_assert(kSpellFirePrologue.size() == 5 && kHitDataPrologue.size() == 5);

        void LogSuppressed(std::atomic_uint32_t& a_counter, std::string_view a_path, const RE::TESForm* a_source,
            const RE::TESForm* a_target)
        {
            const auto count = ++a_counter;
            if (ShouldLogOccurrence(count))
            {
                logs::debug("Suppressed remote spell world effect: path={} source={:08X} target={:08X} count={}",
                    a_path, a_source ? a_source->GetFormID() : 0, a_target ? a_target->GetFormID() : 0, count);
            }
        }

        bool IsRemoteCause(const RE::ActorCause* a_cause)
        {
            return a_cause && ActorRegistry::IsRegistered(a_cause->actor);
        }

        bool IsRemoteProjectile(const RE::Projectile* a_projectile)
        {
            if (!a_projectile)
            {
                return false;
            }
            const auto& data = a_projectile->GetProjectileRuntimeData();
            return ActorRegistry::IsRegistered(data.shooter) || IsRemoteCause(data.actorCause.get());
        }

        bool IsRemoteExplosion(RE::Explosion* a_explosion)
        {
            auto& data = a_explosion->GetExplosionRuntimeData();
            return ActorRegistry::IsRegistered(data.actorOwner) || IsRemoteCause(data.actorCause.get());
        }

        // The reference that applies a spell is the casting actor itself, or a
        // projectile, explosion, or hazard acting for one. Each keeps its actor
        // cause, which a hazard also takes from the projectile that placed it.
        bool IsRemoteSource(const RE::TESObjectREFR* a_ref)
        {
            if (!a_ref)
            {
                return false;
            }
            switch (a_ref->GetFormType())
            {
            case RE::FormType::ActorCharacter:
                return ActorRegistry::IsRegistered(a_ref->As<RE::Actor>());
            case RE::FormType::ProjectileMissile:
            case RE::FormType::ProjectileArrow:
            case RE::FormType::ProjectileGrenade:
            case RE::FormType::ProjectileBeam:
            case RE::FormType::ProjectileFlame:
            case RE::FormType::ProjectileCone:
            case RE::FormType::ProjectileBarrier:
                if (IsRemoteProjectile(static_cast<const RE::Projectile*>(a_ref)))
                {
                    return true;
                }
                break;
            case RE::FormType::PlacedHazard:
            {
                const auto* hazard = static_cast<const RE::Hazard*>(a_ref);
                if (ActorRegistry::IsRegistered(hazard->GetHazardRuntimeData().ownerActor))
                {
                    return true;
                }
                break;
            }
            default:
                break;
            }
            return IsRemoteCause(a_ref->GetActorCause());
        }

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

        // The vanilla hazards a shout or power spawns run scripts on the caster that
        // change the world: Storm Call's set the weather (magicSetWeatherScript) and
        // place activators that cast lightning (magicProjectileStormScript), and the Wind
        // Stone power's places an activator (magicPlaceActivatorScript).
        struct IgnoredEffect
        {
            std::string_view plugin;
            RE::FormID localFormID;
        };

        // Self effects of an allowlisted archetype whose scripts reach past the caster.
        // VoiceMakeEthereal's magicImodScript applies its image space modifiers to the
        // local screen whichever actor the effect is on.
        constexpr std::array kIgnoredEffects{
            IgnoredEffect{ .plugin = "Skyrim.esm", .localFormID = 0x064D68 },
        };

        bool IsIgnoredEffect(const RE::EffectSetting& a_effect)
        {
            // Remote magic reaches a target only after the data has loaded.
            static const auto ignored = [] {
                std::array<RE::FormID, kIgnoredEffects.size()> formIDs{};
                if (auto* dataHandler = RE::TESDataHandler::GetSingleton())
                {
                    for (std::size_t index = 0; index < kIgnoredEffects.size(); ++index)
                    {
                        const auto& entry = kIgnoredEffects[index];
                        formIDs[index] = dataHandler->LookupFormID(entry.localFormID, entry.plugin);
                    }
                }
                return formIDs;
            }();
            const auto formID = a_effect.GetFormID();
            return std::ranges::find(ignored, formID) != ignored.end();
        }

        bool IsSelfContainedEffect(const RE::EffectSetting& a_effect, const RE::MagicItem* a_item)
        {
            const auto archetype = a_effect.GetArchetype();
            return IsSelfContainedArchetype(archetype)
                && !(archetype == Archetype::kSpawnHazard && IsVoiceOrPower(a_item)) && !IsIgnoredEffect(a_effect);
        }

        // Effects on the caster that a sync stage owns whatever the isolation: the
        // hands are equipment sync's, and a beast form is shapeshift sync's.
        bool IsPresentationOwnedEffect(const RE::EffectSetting& a_effect)
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

        // Returns false when a remote actor's magic must not attach to the target.
        // While its source is isolated, that is anything but the caster itself, and
        // on the caster anything but self-contained archetypes.
        bool MayAddTarget(RE::MagicTarget* a_target, const RE::MagicTarget::AddTargetData& a_data,
            std::string_view a_path)
        {
            if (!IsRemoteSource(a_data.caster))
            {
                return true;
            }

            const auto* baseEffect = a_data.effect ? a_data.effect->baseEffect : nullptr;
            const auto* casterActor = a_data.caster->As<RE::Actor>();
            const bool self = casterActor && casterActor->AsMagicTarget() == a_target;
            const auto source = GameplayPolicy::Classify(a_data.magicItem);
            if (self)
            {
                if (baseEffect
                    && (IsSelfContainedEffect(*baseEffect, a_data.magicItem)
                        || (GameplayPolicy::MayAffectWorld(source) && !IsPresentationOwnedEffect(*baseEffect))))
                {
                    return true;
                }
            }
            else if (GameplayPolicy::MayAffectWorld(source))
            {
                return true;
            }

            static std::atomic_uint32_t suppressedCount{ 0 };
            const auto count = ++suppressedCount;
            if (ShouldLogOccurrence(count))
            {
                const auto* targetRef = a_target->GetTargetStatsObject();
                logs::debug(
                    "Suppressed remote actor magic effect: path={} source={:08X} target={:08X} self={} spell={:08X} "
                    "effect={:08X} archetype={} count={}",
                    a_path, a_data.caster->GetFormID(), targetRef ? targetRef->GetFormID() : 0, self,
                    a_data.magicItem ? a_data.magicItem->GetFormID() : 0, baseEffect ? baseEffect->GetFormID() : 0,
                    baseEffect ? static_cast<std::int32_t>(baseEffect->GetArchetype()) : -1, count);
            }
            return false;
        }

        // MagicTarget vfunc 1 attaches a new ActiveEffect. Character and
        // PlayerCharacter share the implementation (ID 38786, SE 37832) in their own
        // vtables, so each vtable keeps its own original.
        template <int Tag>
        struct ActorMagicTargetHook
        {
            static bool Thunk(RE::MagicTarget* a_target, RE::MagicTarget::AddTargetData& a_data)
            {
                if (!MayAddTarget(a_target, a_data, "ActorTarget"))
                {
                    return false;
                }
                return _original(a_target, a_data);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // A non-actor's MagicTarget lives at +0x10 of NonActorMagicTarget, after its
        // BSExtraData base, so its AddTarget is in vtable 1 with the subobject as
        // `this`. Vtable 0 slot 1 is BSExtraData::GetType.
        struct NonActorMagicTargetHook
        {
            static bool Thunk(RE::MagicTarget* a_target, RE::MagicTarget::AddTargetData& a_data)
            {
                if (!MayAddTarget(a_target, a_data, "ObjectTarget"))
                {
                    return false;
                }
                return _original(a_target, a_data);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // Applies a projectile's spell where it struck (ID 34412, SE 33634):
        // `(caster, location, projectile, target, ...)`. Every projectile impact
        // (Projectile::ProcessImpacts, HandleHits, the arrow impacts, and a
        // lobber's detonation through ID 34411) casts through it with the shooter's
        // own caster, including target-location summons and the spell's effects on
        // objects.
        struct SpellImpactHook
        {
            static void Thunk(RE::MagicCaster* a_caster, const RE::NiPoint3* a_location, RE::Projectile* a_projectile,
                RE::TESObjectREFR* a_target, float a_arg5, float a_arg6, bool a_arg7, bool a_arg8)
            {
                if (IsRemoteProjectile(a_projectile)
                    && !GameplayPolicy::MayAffectWorld(GameplayPolicy::Classify(*a_projectile)))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    LogSuppressed(suppressedCount, "SpellImpact", a_projectile, a_target);
                    return;
                }
                _original(a_caster, a_location, a_projectile, a_target, a_arg5, a_arg6, a_arg7, a_arg8);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // A hit carries its projectile or explosion as its source; a melee hit has none.
        Source ClassifyHit(const RE::HitData& a_hitData)
        {
            if (const auto source = a_hitData.sourceRef.get())
            {
                return GameplayPolicy::Classify(*source);
            }
            return a_hitData.weapon || a_hitData.flags.any(RE::HitData::Flag::kMeleeAttack, RE::HitData::Flag::kBash)
                ? Source::kAttack
                : Source::kSpell;
        }

        // Processes a HitData against an actor (ID 38586, SE 37633): damage, stagger,
        // impulses, hit effects, and the target's TESHitEvent. Spell projectiles,
        // beams, and explosions reach it without the actor hit (ID 38627) that
        // CombatIsolation filters.
        struct HitDataHook
        {
            static void Thunk(RE::Actor* a_target, RE::HitData& a_hitData)
            {
                if (ActorRegistry::IsRegistered(a_hitData.aggressor)
                    && (ActorRegistry::IsRegistered(a_target)
                        || !GameplayPolicy::MayAffectWorld(ClassifyHit(a_hitData))))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    LogSuppressed(suppressedCount, "HitData", nullptr, a_target);
                    return;
                }
                _original(a_target, a_hitData);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // TaskQueueInterface::QueueUpdateDestructibleObject (ID 36909, SE 35934):
        // damages a destructible reference on behalf of a cause.
        struct DestructibleHook
        {
            static void Thunk(RE::TaskQueueInterface* a_queue, RE::TESObjectREFR* a_ref, float a_damage, bool a_arg4,
                RE::TESObjectREFR* a_cause)
            {
                if (IsRemoteSource(a_cause) && !GameplayPolicy::MayAffectWorld(GameplayPolicy::Classify(*a_cause)))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    LogSuppressed(suppressedCount, "Destructible", a_cause, a_ref);
                    return;
                }
                _original(a_queue, a_ref, a_damage, a_arg4, a_cause);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // Pushes the rigid body a projectile struck (ID 44215, SE 43024), with the
        // projectile's force scaled by ScaleGameplayImpulseForce (ID 26004). The
        // impact's art, decals, sounds, and hazard come from ID 44213 before it.
        struct ImpactImpulseHook
        {
            static void Thunk(RE::Projectile* a_projectile, void* a_impact, void* a_arg3, void* a_arg4)
            {
                if (IsRemoteProjectile(a_projectile)
                    && !GameplayPolicy::MayAffectWorld(GameplayPolicy::Classify(*a_projectile)))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    LogSuppressed(suppressedCount, "ImpactImpulse", a_projectile, nullptr);
                    return;
                }
                _original(a_projectile, a_impact, a_arg3, a_arg4);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        struct ConeHitsHook
        {
            static bool Thunk(RE::Projectile* a_projectile, void* a_hits)
            {
                if (IsRemoteProjectile(a_projectile)
                    && !GameplayPolicy::MayAffectWorld(GameplayPolicy::Classify(*a_projectile)))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    LogSuppressed(suppressedCount, "ConeHits", a_projectile, nullptr);
                    return true;
                }
                return _original(a_projectile, a_hits);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // The explosion's pass over the references it found (ID 43849, SE 42677):
        // for each, its actor cause, HitData damage and stagger, the enchantment,
        // and the havok impulse. The explosion update calls it once, while
        // kTargetsFound is set and kTargetsProcessed is not, and the original
        // sets kTargetsProcessed first.
        struct ExplosionTargetsHook
        {
            static void Thunk(RE::Explosion* a_explosion)
            {
                if (IsRemoteExplosion(a_explosion)
                    && !GameplayPolicy::MayAffectWorld(GameplayPolicy::Classify(*a_explosion)))
                {
                    a_explosion->GetExplosionRuntimeData().flags.set(RE::Explosion::Flags::kTargetsProcessed);
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    LogSuppressed(suppressedCount, "ExplosionTargets", a_explosion, nullptr);
                    return;
                }
                _original(a_explosion);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // Pushes the bodies in the explosion's radius (ID 43850, SE 42678) through
        // bhkRigidBody::SetLinearImpulse and SetAngularImpulse. The explosion
        // update calls it once targets are processed until kForcesApplied, which
        // the original sets.
        struct ExplosionForcesHook
        {
            static void Thunk(RE::Explosion* a_explosion)
            {
                if (IsRemoteExplosion(a_explosion)
                    && !GameplayPolicy::MayAffectWorld(GameplayPolicy::Classify(*a_explosion)))
                {
                    a_explosion->GetExplosionRuntimeData().flags.set(RE::Explosion::Flags::kForcesApplied);
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    LogSuppressed(suppressedCount, "ExplosionForces", a_explosion, nullptr);
                    return;
                }
                _original(a_explosion);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // A projectile that waits where it lands until an actor comes near, such
        // as a rune: kExplosionAltTrigger with an explosion proximity.
        bool IsProximityProjectile(const RE::BGSProjectile* a_projectile)
        {
            using Flags = RE::BGSProjectileData::BGSProjectileFlags;
            return a_projectile && a_projectile->data.flags.all(Flags::kExplosionAltTrigger)
                && a_projectile->data.explosionProximity > 0.0F;
        }

        // Launches the projectile of the caster's current spell (ID 34452, SE 33672).
        // Its callers (FindTargets and the area and target-location casts) only
        // test the result, which the original always sets.
        struct SpellLaunchHook
        {
            static bool Thunk(RE::MagicCaster* a_caster, void* a_arg2, RE::TESObjectREFR* a_shooter, void* a_arg4,
                const RE::NiPoint3* a_origin, float a_arg6, float a_arg7, std::uint32_t a_arg8, void* a_arg9)
            {
                const auto* spell = a_caster ? a_caster->currentSpell : nullptr;
                const auto* effect = spell ? spell->GetAVEffect() : nullptr;
                if (effect && IsProximityProjectile(effect->data.projectileBase)
                    && ActorRegistry::IsRegistered(a_caster->GetCasterAsActor())
                    && !GameplayPolicy::MayAffectWorld(GameplayPolicy::Classify(spell)))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    LogSuppressed(suppressedCount, "ProximityLaunch", spell, effect->data.projectileBase);
                    return true;
                }
                return _original(a_caster, a_arg2, a_shooter, a_arg4, a_origin, a_arg6, a_arg7, a_arg8, a_arg9);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // A projectile's impact effects (ID 44213, SE 43022): the impact data set's
        // art, particles, decal, and sound, the hit reference's actor cause, and the
        // hazard an impact places, such as a wall spell's flames.
        struct ImpactEffectsHook
        {
            static void Thunk(RE::Projectile* a_projectile, RE::TESObjectREFR* a_target, void* a_impact,
                void* a_arg4, std::uint32_t a_arg5, bool a_arg6)
            {
                if (IsRemoteProjectile(a_projectile)
                    && !GameplayPolicy::MayAffectWorld(GameplayPolicy::Classify(*a_projectile)))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    LogSuppressed(suppressedCount, "ImpactEffects", a_projectile, a_target);
                    return;
                }
                _original(a_projectile, a_target, a_impact, a_arg4, a_arg5, a_arg6);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // The explosion factory (ID 43829, SE 42657) as the projectile update calls
        // it (ID 44216 + 0x3C8, SE 43025 + 0x39F): that call fills a creation
        // record, with the projectile at +0x20, and treats a null explosion as
        // none, killing the projectile as it would after one.
        struct ProjectileExplosionHook
        {
            static constexpr std::ptrdiff_t kRecordProjectileOffset = 0x20;

            static RE::Explosion* Thunk(std::byte* a_record)
            {
                auto* projectile = *reinterpret_cast<RE::Projectile**>(a_record + kRecordProjectileOffset);
                if (!IsRemoteProjectile(projectile)
                    || GameplayPolicy::MayAffectWorld(GameplayPolicy::Classify(*projectile)))
                {
                    return _original(a_record);
                }
                static std::atomic_uint32_t suppressedCount{ 0 };
                LogSuppressed(suppressedCount, "ProjectileExplosion", projectile, nullptr);
                return nullptr;
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        bool IsRemoteActorCaster(const RE::Actor* a_actor, std::string_view a_path)
        {
            if (!ActorRegistry::IsRegistered(a_actor))
            {
                return false;
            }

            static std::atomic_uint32_t suppressedCount{ 0 };
            const auto count = ++suppressedCount;
            if (ShouldLogOccurrence(count))
            {
                logs::debug("Suppressed remote actor release: path={} actor={:08X} count={}", a_path,
                    a_actor->GetFormID(), count);
            }
            return true;
        }

        struct CheckCastHook
        {
            static bool Thunk(RE::MagicCaster* a_caster, RE::MagicItem* a_spell, bool a_dualCast,
                float* a_effectStrength, RE::MagicSystem::CannotCastReason* a_reason, bool a_useBaseValueForCost)
            {
                if (ActorRegistry::IsRegistered(a_caster->GetCasterAsActor()))
                {
                    if (a_reason)
                    {
                        *a_reason = RE::MagicSystem::CannotCastReason::kOK;
                    }
                    return true;
                }
                return _original(a_caster, a_spell, a_dualCast, a_effectStrength, a_reason, a_useBaseValueForCost);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // Written once by InstallHooks on the main thread before the game loop starts.
        bool s_isolatesNativeCasts{ false };

        // Set while a remote actor's native spell fire runs on this thread, so the
        // caster's own CastSpellImmediate inside it is not taken for a script's cast.
        thread_local const RE::MagicCaster* t_nativeSpellFireCaster{ nullptr };

        // A remote actor holds its source's own spell, which casts natively, or a
        // stand-in (SpellStandIn), a dynamic copy of Healing Hands whose fire would
        // heal. Nothing else puts a dynamic spell in its hands.
        bool IsNativeRemoteSpell(const RE::MagicItem* a_spell)
        {
            return a_spell && !a_spell->IsDynamicForm();
        }

        // The magic caster's spell fire (ID 34407): checks the cast, spends the cost,
        // applies the spell or launches its projectile, and enters the casting state.
        // Reached from the M?h_SpellFire_Event annotations and, while a concentration
        // cast is casting, from the caster update. The hand's release action
        // (StartCastImpl) stays native. The annotation handlers are not hooked
        // instead: a remote actor's cast reached this function without passing
        // through their vtables (observed in game). Its FindTargets (ID 34410) ends
        // with FinishCast for a cast that is not concentration, which the hook keeps
        // for a suppressed stand-in cast the remote actor's caster runs, so a released
        // fire-and-forget cast returns to idle as it would natively. A vampire lord's
        // casts are relayed graph events instead, and its hands hold stand-ins. The
        // voice caster's Voice_SpellFire_Event handler (ID 42818) enters kPostcast
        // once this returns true and resets the voice on false, so a hidden shout
        // finishes as a fired one.
        struct SpellFireHook
        {
            static bool IsSuppressedShoutFire(const RE::Actor* a_actor, const RE::MagicItem* a_spell)
            {
                return a_spell->GetSpellType() == RE::MagicSystem::SpellType::kVoicePower
                    && ActorRegistry::IsVoiceFireSuppressed(a_actor);
            }

            static bool Thunk(RE::MagicCaster* a_caster, RE::TESObjectREFR* a_target, bool a_flag)
            {
                auto* actor = a_caster->GetCasterAsActor();
                if (ActorRegistry::IsRegistered(actor) && IsNativeRemoteSpell(a_caster->currentSpell)
                    && GameplayPolicy::ShowsNativeFire(a_caster->currentSpell)
                    && !IsSuppressedShoutFire(actor, a_caster->currentSpell))
                {
                    static std::atomic_uint32_t nativeCount{ 0 };
                    if (const auto count = ++nativeCount; ShouldLogOccurrence(count))
                    {
                        logs::debug("Remote actor native spell fire: actor={:08X} spell={:08X} state={} count={}",
                            actor->GetFormID(), a_caster->currentSpell->GetFormID(),
                            static_cast<std::uint32_t>(a_caster->state.get()), count);
                    }
                    const auto* outer = t_nativeSpellFireCaster;
                    t_nativeSpellFireCaster = a_caster;
                    const bool result = _original(a_caster, a_target, a_flag);
                    t_nativeSpellFireCaster = outer;
                    return result;
                }
                if (IsRemoteActorCaster(actor, "SpellFire"))
                {
                    const auto* spell = a_caster->currentSpell;
                    if (spell && spell->GetCastingType() != RE::MagicSystem::CastingType::kConcentration
                        && a_caster->state.get() != RE::MagicCaster::State::kNone
                        && (ActorRegistry::IsCastAnimationOwned(actor)
                            || (IsVoiceOrPower(spell) && ActorRegistry::IsVoiceAnimationOwned(actor))))
                    {
                        a_caster->FinishCast();
                    }
                    return true;
                }
                return _original(a_caster, a_target, a_flag);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // Casts a spell without its animation, for example from a script.
        struct CastSpellImmediateHook
        {
            static void Thunk(RE::MagicCaster* a_caster, RE::MagicItem* a_spell, bool a_noHitEffectArt,
                RE::TESObjectREFR* a_target, float a_effectiveness, bool a_hostileEffectivenessOnly,
                float a_magnitudeOverride, RE::Actor* a_blameActor)
            {
                if (a_caster != t_nativeSpellFireCaster
                    && IsRemoteActorCaster(a_caster->GetCasterAsActor(), "CastSpellImmediate"))
                {
                    return;
                }
                _original(a_caster, a_spell, a_noHitEffectArt, a_target, a_effectiveness, a_hostileEffectivenessOnly,
                    a_magnitudeOverride, a_blameActor);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // True when every caller's call was detoured. Any detoured site gives the
        // original, since all call the same function.
        template <class Hook>
        bool DetourCallers(
            std::span<const REL::RelocationID> a_callers, REL::RelocationID a_callee, std::string_view a_name)
        {
            bool complete = true;
            for (const auto& caller : a_callers)
            {
                if (caller.id() == 0)
                {
                    continue;
                }
                if (const auto original = WriteCalleeDetour(REL::ID(caller), REL::ID(a_callee),
                        reinterpret_cast<std::uintptr_t>(&Hook::Thunk), a_name))
                {
                    Hook::_original = original;
                }
                else
                {
                    complete = false;
                }
            }
            return complete;
        }

        void InstallVTableHooks()
        {
            // Every actor's hand casters share this vtable; the thunks pass other
            // actors through unchanged.
            REL::Relocation<std::uintptr_t> actorMagicCasterVTable{ RE::VTABLE_ActorMagicCaster[0] };
            CastSpellImmediateHook::_original = actorMagicCasterVTable.write_vfunc(0x1, CastSpellImmediateHook::Thunk);
            CheckCastHook::_original = actorMagicCasterVTable.write_vfunc(0xA, CheckCastHook::Thunk);

            // Vtable 4 of an actor is its MagicTarget subobject. Character's slot
            // already holds SurvivalPolicy's incoming firewall, which this chains.
            REL::Relocation<std::uintptr_t> characterMagicTargetVTable{ RE::VTABLE_Character[4] };
            ActorMagicTargetHook<0>::_original
                = characterMagicTargetVTable.write_vfunc(0x1, ActorMagicTargetHook<0>::Thunk);
            REL::Relocation<std::uintptr_t> playerMagicTargetVTable{ RE::VTABLE_PlayerCharacter[4] };
            ActorMagicTargetHook<1>::_original
                = playerMagicTargetVTable.write_vfunc(0x1, ActorMagicTargetHook<1>::Thunk);
            REL::Relocation<std::uintptr_t> nonActorMagicTargetVTable{ RE::VTABLE_NonActorMagicTarget[1] };
            NonActorMagicTargetHook::_original
                = nonActorMagicTargetVTable.write_vfunc(0x1, NonActorMagicTargetHook::Thunk);

            REL::Relocation<std::uintptr_t> coneProjectileVTable{ RE::VTABLE_ConeProjectile[0] };
            ConeHitsHook::_original = coneProjectileVTable.write_vfunc(0xBE, ConeHitsHook::Thunk);

            logs::debug(
                "Remote spell vtable hooks: castSpellImmediate={} checkCast={} characterMagicTarget={} "
                "playerMagicTarget={} objectMagicTarget={} coneHits={}",
                CastSpellImmediateHook::_original.address() != 0, CheckCastHook::_original.address() != 0,
                ActorMagicTargetHook<0>::_original.address() != 0, ActorMagicTargetHook<1>::_original.address() != 0,
                NonActorMagicTargetHook::_original.address() != 0, ConeHitsHook::_original.address() != 0);
        }
    }

    void InstallHooks()
    {
        InstallVTableHooks();

        // The comments above name 1.7.104 IDs. Each SE ID was matched against the
        // 1.5.97 disassembly by its callers, call sequence, and field offsets.
        SpellFireHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(33629, 34407)), kSpellFirePrologue,
            reinterpret_cast<std::uintptr_t>(&SpellFireHook::Thunk), "spell fire");
        HitDataHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(37633, 38586)), kHitDataPrologue,
            reinterpret_cast<std::uintptr_t>(&HitDataHook::Thunk), "hit data");
        const bool spellImpact = DetourCallers<SpellImpactHook>(
            kSpellImpactCallers, RELOCATION_ID(33634, 34412), "projectile spell impact");
        const bool destructible = DetourCallers<DestructibleHook>(
            kDestructibleCallers, RELOCATION_ID(35934, 36909), "destructible update");
        const bool impactImpulse = DetourCallers<ImpactImpulseHook>(
            kImpactImpulseCallers, RELOCATION_ID(43024, 44215), "projectile impact impulse");
        const bool explosionTargets = DetourCallers<ExplosionTargetsHook>(
            kExplosionUpdateCallers, RELOCATION_ID(42677, 43849), "explosion targets");
        const bool explosionForces = DetourCallers<ExplosionForcesHook>(
            kExplosionUpdateCallers, RELOCATION_ID(42678, 43850), "explosion forces");
        const bool spellLaunch = DetourCallers<SpellLaunchHook>(
            kSpellLaunchCallers, RELOCATION_ID(33672, 34452), "spell projectile launch");
        const bool impactEffects = DetourCallers<ImpactEffectsHook>(
            kImpactEffectsCallers, RELOCATION_ID(43022, 44213), "projectile impact effects");
        const bool projectileExplosion = DetourCallers<ProjectileExplosionHook>(
            kProjectileUpdateCallers, RELOCATION_ID(42657, 43829), "projectile explosion");
        logs::debug(
            "Remote spell isolation hooks: spellFire={} hitData={} spellImpact={} destructible={} impactImpulse={} "
            "explosionTargets={} explosionForces={} spellLaunch={} impactEffects={} projectileExplosion={}",
            SpellFireHook::_original.address() != 0, HitDataHook::_original.address() != 0, spellImpact, destructible,
            impactImpulse, explosionTargets, explosionForces, spellLaunch, impactEffects, projectileExplosion);

        s_isolatesNativeCasts = SpellFireHook::_original.address() != 0 && HitDataHook::_original.address() != 0
            && spellImpact && destructible && impactImpulse && explosionTargets && explosionForces && spellLaunch
            && impactEffects && projectileExplosion;
        if (!s_isolatesNativeCasts)
        {
            logs::error("Remote spell isolation is incomplete: remote hands hold stand-ins, and remote shouts and "
                        "powers are not cast");
        }
    }

    bool IsolatesNativeCasts()
    {
        return s_isolatesNativeCasts;
    }
}
