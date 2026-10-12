#include "actor/CombatIsolation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "runtime/CodeDetour.h"

#include <array>
#include <atomic>
#include <span>
#include <string_view>

namespace CombatIsolation
{
    namespace
    {
        using ActorConstants::ShouldLogOccurrence;
        using CodeDetour::EntryDetourTrampolineSize;
        using CodeDetour::WriteEntryDetour;

        bool IsRemoteActorCombatQuery(const RE::Actor* a_actor, const RE::Actor* a_target, std::string_view a_path)
        {
            if (!ActorRegistry::IsRegistered(a_target) && !ActorRegistry::IsRegistered(a_actor))
            {
                return false;
            }

            static std::atomic_uint32_t suppressedCount{ 0 };
            const auto count = ++suppressedCount;
            if (ShouldLogOccurrence(count))
            {
                logs::debug(
                    "Suppressed local hostility involving remote actor: path={} actor={:08X} target={:08X} count={}",
                    a_path, a_actor ? a_actor->GetFormID() : 0, a_target ? a_target->GetFormID() : 0, count);
            }
            return true;
        }

        bool IsRemoteActorHit(const RE::Actor* a_aggressor, const RE::TESObjectREFR* a_target, std::string_view a_path)
        {
            if (!ActorRegistry::IsRegistered(a_aggressor)
                && (!a_target || !ActorRegistry::IsRegistered(a_target->As<RE::Actor>())))
            {
                return false;
            }

            static std::atomic_uint32_t suppressedCount{ 0 };
            const auto count = ++suppressedCount;
            if (ShouldLogOccurrence(count))
            {
                logs::debug("Suppressed hit involving remote actor: path={} aggressor={:08X} target={:08X} count={}",
                    a_path, a_aggressor ? a_aggressor->GetFormID() : 0, a_target ? a_target->GetFormID() : 0, count);
            }
            return true;
        }

        bool IsRemoteActorRelease(const RE::Actor* a_actor, std::string_view a_path)
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

        constexpr std::array<std::uint8_t, 5> kHostilityPrologue{
            0x44, 0x88, 0x4C, 0x24, 0x20 // mov [rsp+20h], r9b
        };
        constexpr std::array<std::uint8_t, 5> kIsHostileToActorPrologue{
            0x48, 0x89, 0x5C, 0x24, 0x10 // mov [rsp+10h], rbx
        };
        constexpr std::array<std::uint8_t, 5> kStartCombatPrologue{
            0x4C, 0x8B, 0xDC, // mov r11, rsp
            0x55, // push rbp
            0x56 // push rsi
        };
        constexpr std::array<std::uint8_t, 5> kSEStartCombatPrologue{
            0x4C, 0x8B, 0xDC, // mov r11, rsp
            0x56, // push rsi
            0x57 // push rdi
        };
        constexpr std::array<std::uint8_t, 5> kSpellFirePrologue{
            0x40, 0x56, // push rsi
            0x57, // push rdi
            0x41, 0x56 // push r14
        };
        constexpr std::array<std::uint8_t, 6> kWeaponSwingHitPrologue{
            0x48, 0x8B, 0xC4, // mov rax, rsp
            0x88, 0x50, 0x10 // mov [rax+10h], dl
        };
        constexpr std::array<std::uint8_t, 5> kSEWeaponSwingHitPrologue{
            0x48, 0x8B, 0xC4, // mov rax, rsp
            0x56, // push rsi
            0x57 // push rdi
        };
        // Shared by the swing target entry and, on 1.7.104, the actor hit past its guard.
        constexpr std::array<std::uint8_t, 7> kHitPrologue{
            0x48, 0x8B, 0xC4, // mov rax, rsp
            0x48, 0x89, 0x48, 0x08 // mov [rax+8], rcx
        };
        constexpr std::array<std::uint8_t, 6> kSEActorHitPrologue{
            0x48, 0x8B, 0xC4, // mov rax, rsp
            0x57, // push rdi
            0x41, 0x54 // push r12
        };
        // The actor hit opens with test rdx, rdx; je to its ret on both runtimes.
        constexpr std::ptrdiff_t kActorHitGuardSize = 9;

        static_assert(EntryDetourTrampolineSize(kHostilityPrologue.size())
                + EntryDetourTrampolineSize(kIsHostileToActorPrologue.size())
                + EntryDetourTrampolineSize(kStartCombatPrologue.size())
                + EntryDetourTrampolineSize(kSpellFirePrologue.size())
                + EntryDetourTrampolineSize(kWeaponSwingHitPrologue.size())
                + 2 * EntryDetourTrampolineSize(kHitPrologue.size())
            == kTrampolineSize);
        static_assert(kSEStartCombatPrologue.size() <= kStartCombatPrologue.size()
            && kSEWeaponSwingHitPrologue.size() <= kWeaponSwingHitPrologue.size()
            && kSEActorHitPrologue.size() <= kHitPrologue.size());

        using Prologue = std::span<const std::uint8_t>;

        // The comments below name 1.7.104 IDs and offsets. Each SE ID was matched
        // against the 1.5.97 disassembly by its code and the functions it calls.

        // The engine's hostility decision (ID 37534): aggression and faction reaction
        // toward a target. Its wrappers (IDs 37532 and 37533) serve combat group target
        // handling, StartCombat, and many AI callers.
        struct HostilityHook
        {
            static bool Thunk(RE::Actor* a_actor, RE::Actor* a_target, std::int32_t a_aggression, std::uint8_t a_mode)
            {
                if (IsRemoteActorCombatQuery(a_actor, a_target, "Hostility"))
                {
                    return false;
                }
                return _original(a_actor, a_target, a_aggression, a_mode);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // Actor::IsHostileToActor (ID 37537). Besides the hostility decision it
        // also returns true from a faction-reaction path of its own.
        struct IsHostileToActorHook
        {
            static bool Thunk(RE::Actor* a_actor, RE::Actor* a_target)
            {
                if (IsRemoteActorCombatQuery(a_actor, a_target, "IsHostileToActor"))
                {
                    return false;
                }
                return _original(a_actor, a_target);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // Actor::StartCombat (ID 38561). Also reached without a hostility check, for
        // example when a remote actor's replicated swing hits a local actor.
        struct StartCombatHook
        {
            static bool Thunk(RE::Actor* a_actor, RE::Actor* a_target, RE::CombatGroup* a_combatGroup)
            {
                if (IsRemoteActorCombatQuery(a_actor, a_target, "StartCombat"))
                {
                    return false;
                }
                return _original(a_actor, a_target, a_combatGroup);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // The melee hit frame (ID 38603): sweeps the attacker's weapon reach and hits
        // the actors and objects it finds, applying damage, stagger, impulses, and hit
        // effects. Reached from the attack animation's HitFrame annotation, possibly
        // after being queued to another update pass.
        struct WeaponSwingHitHook
        {
            static void Thunk(RE::Actor* a_attacker, bool a_leftHand, bool a_flag)
            {
                if (IsRemoteActorHit(a_attacker, nullptr, "WeaponSwing"))
                {
                    return;
                }
                _original(a_attacker, a_leftHand, a_flag);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // The swing's hit target (ID 38628, Precision's
        // CalculateCurrentHitTargetForWeaponSwing): the melee hit frame is its only
        // caller in the executable, and Precision calls it directly before each hit of
        // its weapon collisions. The hit frame treats a null result as no target.
        struct SwingHitTargetHook
        {
            static RE::TESObjectREFR* Thunk(RE::Actor* a_attacker)
            {
                if (IsRemoteActorHit(a_attacker, nullptr, "SwingHitTarget"))
                {
                    return nullptr;
                }
                return _original(a_attacker);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // The actor hit (ID 38627): builds HitData for the aggressor and applies
        // damage, stagger, hit effects, and the target's hit event. The melee hit frame
        // (+0x40A sweep, +0x45A primary target) and the projectile impact
        // (ID 44218 + 0x90) call it, and Precision calls it directly.
        struct ActorHitHook
        {
            static void Thunk(
                RE::Actor* a_aggressor, RE::TESObjectREFR* a_target, RE::Projectile* a_projectile, bool a_leftHand)
            {
                if (IsRemoteActorHit(a_aggressor, a_target, a_projectile ? "ProjectileHit" : "ActorHit"))
                {
                    return;
                }
                _original(a_aggressor, a_target, a_projectile, a_leftHand);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // The arrowRelease annotation handler launches the nocked arrow and spends
        // the ammo. A remote actor's bow draw keeps the arrow model it attached.
        struct ArrowReleaseHandlerHook
        {
            static bool Thunk(RE::AnimHandler* a_handler, RE::Actor& a_actor, const RE::BSFixedString& a_tag)
            {
                if (IsRemoteActorRelease(&a_actor, "ArrowRelease"))
                {
                    return true;
                }
                return _original(a_handler, a_actor, a_tag);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

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

        // The magic caster's spell fire (ID 34407): checks the cast, spends the cost,
        // applies the spell or launches its projectile, and enters the casting state.
        // Reached from the M?h_SpellFire_Event annotations and, while a concentration
        // cast is casting, from the caster update. The hand's release action
        // (StartCastImpl) that selects the cast clip stays native. The annotation
        // handlers are not hooked instead: a remote actor's cast reached this function
        // without passing through their vtables (observed in game).
        struct SpellFireHook
        {
            static bool Thunk(RE::MagicCaster* a_caster, RE::TESObjectREFR* a_target, bool a_flag)
            {
                if (IsRemoteActorRelease(a_caster->GetCasterAsActor(), "SpellFire"))
                {
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
                if (IsRemoteActorRelease(a_caster->GetCasterAsActor(), "CastSpellImmediate"))
                {
                    return;
                }
                _original(a_caster, a_spell, a_noHitEffectArt, a_target, a_effectiveness, a_hostileEffectivenessOnly,
                    a_magnitudeOverride, a_blameActor);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        void InstallReleaseHooks()
        {
            // IHandlerFunctor vfunc 1 is ExecuteHandler.
            REL::Relocation<std::uintptr_t> arrowReleaseHandlerVTable{ RE::VTABLE_ArrowReleaseHandler[0] };
            ArrowReleaseHandlerHook::_original
                = arrowReleaseHandlerVTable.write_vfunc(0x1, ArrowReleaseHandlerHook::Thunk);

            // Every actor's hand casters share this vtable; the thunks pass other
            // actors through unchanged.
            REL::Relocation<std::uintptr_t> actorMagicCasterVTable{ RE::VTABLE_ActorMagicCaster[0] };
            CastSpellImmediateHook::_original = actorMagicCasterVTable.write_vfunc(0x1, CastSpellImmediateHook::Thunk);
            CheckCastHook::_original = actorMagicCasterVTable.write_vfunc(0xA, CheckCastHook::Thunk);
            logs::debug("Remote actor release hooks: arrowRelease={} castSpellImmediate={} checkCast={}",
                ArrowReleaseHandlerHook::_original.address() != 0, CastSpellImmediateHook::_original.address() != 0,
                CheckCastHook::_original.address() != 0);
        }
    }

    void InstallHooks()
    {
        InstallReleaseHooks();

        HostilityHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(36534, 37534)), kHostilityPrologue,
            reinterpret_cast<std::uintptr_t>(&HostilityHook::Thunk), "hostility decision");
        IsHostileToActorHook::_original
            = WriteEntryDetour(REL::ID(RELOCATION_ID(36537, 37537)), kIsHostileToActorPrologue,
                reinterpret_cast<std::uintptr_t>(&IsHostileToActorHook::Thunk), "IsHostileToActor");
        StartCombatHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(37608, 38561)),
            REL::Relocate(Prologue{ kSEStartCombatPrologue }, Prologue{ kStartCombatPrologue }),
            reinterpret_cast<std::uintptr_t>(&StartCombatHook::Thunk), "StartCombat");
        WeaponSwingHitHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(37650, 38603)),
            REL::Relocate(Prologue{ kSEWeaponSwingHitPrologue }, Prologue{ kWeaponSwingHitPrologue }),
            reinterpret_cast<std::uintptr_t>(&WeaponSwingHitHook::Thunk), "weapon swing hit");
        SpellFireHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(33629, 34407)), kSpellFirePrologue,
            reinterpret_cast<std::uintptr_t>(&SpellFireHook::Thunk), "spell fire");
        SwingHitTargetHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(37674, 38628)), kHitPrologue,
            reinterpret_cast<std::uintptr_t>(&SwingHitTargetHook::Thunk), "swing hit target");
        ActorHitHook::_original = WriteEntryDetour(REL::ID(RELOCATION_ID(37673, 38627)),
            REL::Relocate(Prologue{ kSEActorHitPrologue }, Prologue{ kHitPrologue }),
            reinterpret_cast<std::uintptr_t>(&ActorHitHook::Thunk), "actor hit", kActorHitGuardSize);
        logs::debug(
            "Remote actor combat isolation hooks: hostility={} isHostileToActor={} startCombat={} weaponSwingHit={} "
            "spellFire={} swingHitTarget={} actorHit={}",
            HostilityHook::_original.address() != 0, IsHostileToActorHook::_original.address() != 0,
            StartCombatHook::_original.address() != 0, WeaponSwingHitHook::_original.address() != 0,
            SpellFireHook::_original.address() != 0, SwingHitTargetHook::_original.address() != 0,
            ActorHitHook::_original.address() != 0);
    }
}
