#include "actor/SpellIsolation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"

#include <array>
#include <atomic>
#include <span>
#include <string_view>

namespace SpellIsolation
{
    namespace
    {
        using ActorConstants::ShouldLogOccurrence;
        using CodeDetour::EntryDetourTrampolineSize;
        using CodeDetour::WriteEntryDetour;
        using Prologue = std::span<const std::uint8_t>;
        using Archetype = RE::EffectSetting::Archetype;

        constexpr std::array<std::uint8_t, 5> kSpellFirePrologue{
            0x40, 0x56, // push rsi
            0x57, // push rdi
            0x41, 0x56 // push r14
        };
        // Past the entry guard test r8, r8; je to its ret, on both runtimes.
        constexpr std::array<std::uint8_t, 7> kSpellImpactPrologue{
            0x48, 0x8B, 0xC4, // mov rax, rsp
            0x4C, 0x89, 0x48, 0x20 // mov [rax+20h], r9
        };
        constexpr std::array<std::uint8_t, 5> kHitDataPrologue{
            0x48, 0x8B, 0xC4, // mov rax, rsp
            0x55, // push rbp
            0x56 // push rsi
        };
        constexpr std::array<std::uint8_t, 7> kDestructiblePrologue{
            0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00 // sub rsp, 0A8h
        };
        // SE opens with test rdx, rdx; je to its ret, which 1.7.104 does later.
        constexpr std::array<std::uint8_t, 8> kSEDestructiblePrologue{
            0x53, // push rbx
            0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00 // sub rsp, 0B0h
        };
        // Past the entry guard test r9, r9; je to its ret, on both runtimes.
        constexpr std::array<std::uint8_t, 5> kImpactImpulsePrologue{
            0x48, 0x8B, 0xC4, // mov rax, rsp
            0x56, // push rsi
            0x57 // push rdi
        };
        constexpr std::array<std::uint8_t, 5> kExplosionTargetsPrologue{
            0x48, 0x8B, 0xC4, // mov rax, rsp
            0x55, // push rbp
            0x53 // push rbx
        };
        constexpr std::array<std::uint8_t, 5> kExplosionForcesPrologue{
            0x40, 0x53, // push rbx
            0x55, // push rbp
            0x56, // push rsi
            0x57 // push rdi
        };
        constexpr std::array<std::uint8_t, 5> kSpellLaunchPrologue{
            0x40, 0x55, // push rbp
            0x53, // push rbx
            0x56, // push rsi
            0x57 // push rdi
        };
        constexpr std::array<std::uint8_t, 7> kImpactEffectsPrologue{
            0x48, 0x8B, 0xC4, // mov rax, rsp
            0x48, 0x89, 0x50, 0x10 // mov [rax+10h], rdx
        };
        constexpr std::array<std::uint8_t, 6> kSEImpactEffectsPrologue{
            0x48, 0x8B, 0xC4, // mov rax, rsp
            0x55, // push rbp
            0x41, 0x54 // push r12
        };
        // test reg, reg (3 bytes) and a near je (6 bytes).
        constexpr std::ptrdiff_t kEntryGuardSize = 9;
        // The projectile update's call of the explosion factory (ID 43829, SE 42657).
        constexpr std::ptrdiff_t kProjectileExplosionCallOffset = 0x3C8;
        constexpr std::ptrdiff_t kSEProjectileExplosionCallOffset = 0x39F;

        static_assert(6 * EntryDetourTrampolineSize(5) + EntryDetourTrampolineSize(kSpellImpactPrologue.size())
                + EntryDetourTrampolineSize(kImpactEffectsPrologue.size())
                + EntryDetourTrampolineSize(kSEDestructiblePrologue.size()) + CodeDetour::kCallDetourTrampolineSize
            == kTrampolineSize);
        static_assert(kSpellFirePrologue.size() == 5 && kHitDataPrologue.size() == 5
            && kImpactImpulsePrologue.size() == 5 && kExplosionTargetsPrologue.size() == 5
            && kExplosionForcesPrologue.size() == 5
            && kSpellLaunchPrologue.size() == 5 && kDestructiblePrologue.size() <= kSEDestructiblePrologue.size()
            && kSEImpactEffectsPrologue.size() <= kImpactEffectsPrologue.size());

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

        // Returns false when a remote actor's magic must not attach to the target:
        // anything but the caster itself, and on the caster only self-contained
        // archetypes. a_targetActor is null for a non-actor target.
        bool MayAddTarget(const RE::MagicTarget::AddTargetData& a_data, const RE::Actor* a_targetActor,
            const RE::TESObjectREFR* a_targetRef, std::string_view a_path)
        {
            if (!IsRemoteSource(a_data.caster))
            {
                return true;
            }

            const auto* baseEffect = a_data.effect ? a_data.effect->baseEffect : nullptr;
            const bool self = a_targetActor && a_data.caster == a_targetActor;
            if (self && baseEffect && IsSelfContainedArchetype(baseEffect->GetArchetype()))
            {
                return true;
            }

            static std::atomic_uint32_t suppressedCount{ 0 };
            const auto count = ++suppressedCount;
            if (ShouldLogOccurrence(count))
            {
                logs::debug(
                    "Suppressed remote actor magic effect: path={} source={:08X} target={:08X} self={} spell={:08X} "
                    "effect={:08X} archetype={} count={}",
                    a_path, a_data.caster->GetFormID(), a_targetRef ? a_targetRef->GetFormID() : 0, self,
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
                const auto* actor = static_cast<RE::Actor*>(a_target);
                if (!MayAddTarget(a_data, actor, actor, "ActorTarget"))
                {
                    return false;
                }
                return _original(a_target, a_data);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        struct NonActorMagicTargetHook
        {
            static bool Thunk(RE::NonActorMagicTarget* a_target, RE::MagicTarget::AddTargetData& a_data)
            {
                if (!MayAddTarget(a_data, nullptr, a_target->GetTargetStatsObject(), "ObjectTarget"))
                {
                    return false;
                }
                return _original(a_target, a_data);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // Applies a projectile's spell where it struck (ID 34412, SE 33634):
        // `(caster, location, projectile, target, ...)`. Every projectile impact
        // (Projectile::ProcessImpacts, HandleHits, the arrow and cone impacts, and a
        // lobber's detonation through ID 34411) casts through it with the shooter's
        // own caster, including target-location summons and the spell's effects on
        // objects.
        struct SpellImpactHook
        {
            static void Thunk(RE::MagicCaster* a_caster, const RE::NiPoint3* a_location, RE::Projectile* a_projectile,
                RE::TESObjectREFR* a_target, float a_arg5, float a_arg6, bool a_arg7, bool a_arg8)
            {
                if (IsRemoteProjectile(a_projectile))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    LogSuppressed(suppressedCount, "SpellImpact", a_projectile, a_target);
                    return;
                }
                _original(a_caster, a_location, a_projectile, a_target, a_arg5, a_arg6, a_arg7, a_arg8);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // Processes a HitData against an actor (ID 38586, SE 37633): damage, stagger,
        // impulses, hit effects, and the target's TESHitEvent. Spell projectiles,
        // beams, and explosions reach it without the actor hit (ID 38627) that
        // CombatIsolation filters.
        struct HitDataHook
        {
            static void Thunk(RE::Actor* a_target, RE::HitData& a_hitData)
            {
                if (ActorRegistry::IsRegistered(a_hitData.aggressor))
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
                if (IsRemoteSource(a_cause))
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
                if (IsRemoteProjectile(a_projectile))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    LogSuppressed(suppressedCount, "ImpactImpulse", a_projectile, nullptr);
                    return;
                }
                _original(a_projectile, a_impact, a_arg3, a_arg4);
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
                if (IsRemoteExplosion(a_explosion))
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
                if (IsRemoteExplosion(a_explosion))
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
                    && ActorRegistry::IsRegistered(a_caster->GetCasterAsActor()))
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
        // hazard an impact places, such as a wall spell's flames. Called from
        // ProcessImpacts, the beam and flame hits (ID 44205), and the cone impact.
        struct ImpactEffectsHook
        {
            static void Thunk(RE::Projectile* a_projectile, RE::TESObjectREFR* a_target, void* a_impact,
                void* a_arg4, std::uint32_t a_arg5, bool a_arg6)
            {
                if (IsRemoteProjectile(a_projectile))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    LogSuppressed(suppressedCount, "ImpactEffects", a_projectile, a_target);
                    return;
                }
                _original(a_projectile, a_target, a_impact, a_arg4, a_arg5, a_arg6);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // The projectile update's explosion (ID 44216 + 0x3C8, SE 43025 + 0x39F):
        // it fills a creation record, with the projectile at +0x20, and treats a
        // null explosion as none, killing the projectile as it would after one.
        struct ProjectileExplosionHook
        {
            static constexpr std::ptrdiff_t kRecordProjectileOffset = 0x20;

            static RE::Explosion* Thunk(std::byte* a_record)
            {
                auto* projectile = *reinterpret_cast<RE::Projectile**>(a_record + kRecordProjectileOffset);
                if (IsRemoteProjectile(projectile))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    LogSuppressed(suppressedCount, "ProjectileExplosion", projectile, nullptr);
                    return nullptr;
                }
                return _original(a_record);
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
        // casts are relayed graph events instead, and its hands hold stand-ins.
        struct SpellFireHook
        {
            static bool Thunk(RE::MagicCaster* a_caster, RE::TESObjectREFR* a_target, bool a_flag)
            {
                auto* actor = a_caster->GetCasterAsActor();
                if (ActorRegistry::IsRegistered(actor) && IsNativeRemoteSpell(a_caster->currentSpell))
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
                        && ActorRegistry::IsCastAnimationOwned(actor))
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
            REL::Relocation<std::uintptr_t> nonActorMagicTargetVTable{ RE::VTABLE_NonActorMagicTarget[0] };
            NonActorMagicTargetHook::_original
                = nonActorMagicTargetVTable.write_vfunc(0x1, NonActorMagicTargetHook::Thunk);

            logs::debug(
                "Remote spell vtable hooks: castSpellImmediate={} checkCast={} characterMagicTarget={} "
                "playerMagicTarget={} objectMagicTarget={}",
                CastSpellImmediateHook::_original.address() != 0, CheckCastHook::_original.address() != 0,
                ActorMagicTargetHook<0>::_original.address() != 0, ActorMagicTargetHook<1>::_original.address() != 0,
                NonActorMagicTargetHook::_original.address() != 0);
        }
    }

    void InstallHooks()
    {
        InstallVTableHooks();

        // The comments above name 1.7.104 IDs. Each SE ID was matched against the
        // 1.5.97 disassembly by its callers, call sequence, and field offsets.
        SpellFireHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(33629, 34407)), kSpellFirePrologue,
            reinterpret_cast<std::uintptr_t>(&SpellFireHook::Thunk), "spell fire");
        SpellImpactHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(33634, 34412)), kSpellImpactPrologue,
            reinterpret_cast<std::uintptr_t>(&SpellImpactHook::Thunk), "projectile spell impact", kEntryGuardSize);
        HitDataHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(37633, 38586)), kHitDataPrologue,
            reinterpret_cast<std::uintptr_t>(&HitDataHook::Thunk), "hit data");
        DestructibleHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(35934, 36909)),
            REL::Relocate(Prologue{ kSEDestructiblePrologue }, Prologue{ kDestructiblePrologue }),
            reinterpret_cast<std::uintptr_t>(&DestructibleHook::Thunk), "destructible update",
            REL::Relocate(std::ptrdiff_t{ kEntryGuardSize }, std::ptrdiff_t{ 0 }));
        ImpactImpulseHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(43024, 44215)),
            kImpactImpulsePrologue, reinterpret_cast<std::uintptr_t>(&ImpactImpulseHook::Thunk),
            "projectile impact impulse", kEntryGuardSize);
        ExplosionTargetsHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(42677, 43849)),
            kExplosionTargetsPrologue, reinterpret_cast<std::uintptr_t>(&ExplosionTargetsHook::Thunk),
            "explosion targets");
        ExplosionForcesHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(42678, 43850)),
            kExplosionForcesPrologue, reinterpret_cast<std::uintptr_t>(&ExplosionForcesHook::Thunk),
            "explosion forces");
        SpellLaunchHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(33672, 34452)), kSpellLaunchPrologue,
            reinterpret_cast<std::uintptr_t>(&SpellLaunchHook::Thunk), "spell projectile launch");
        ImpactEffectsHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(43022, 44213)),
            REL::Relocate(Prologue{ kSEImpactEffectsPrologue }, Prologue{ kImpactEffectsPrologue }),
            reinterpret_cast<std::uintptr_t>(&ImpactEffectsHook::Thunk), "projectile impact effects");
        ProjectileExplosionHook::_original = CodeDetour::WriteCallDetour(REL::ID(RELOCATION_ID(43025, 44216)),
            REL::Relocate(std::ptrdiff_t{ kSEProjectileExplosionCallOffset },
                std::ptrdiff_t{ kProjectileExplosionCallOffset }),
            reinterpret_cast<std::uintptr_t>(&ProjectileExplosionHook::Thunk), "projectile explosion");
        logs::debug(
            "Remote spell isolation hooks: spellFire={} spellImpact={} hitData={} destructible={} impactImpulse={} "
            "explosionTargets={} explosionForces={} spellLaunch={} impactEffects={} projectileExplosion={}",
            SpellFireHook::_original.address() != 0, SpellImpactHook::_original.address() != 0,
            HitDataHook::_original.address() != 0, DestructibleHook::_original.address() != 0,
            ImpactImpulseHook::_original.address() != 0, ExplosionTargetsHook::_original.address() != 0,
            ExplosionForcesHook::_original.address() != 0, SpellLaunchHook::_original.address() != 0,
            ImpactEffectsHook::_original.address() != 0, ProjectileExplosionHook::_original.address() != 0);
    }
}
