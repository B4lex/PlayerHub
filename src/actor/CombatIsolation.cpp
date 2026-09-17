#include "actor/CombatIsolation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"

#include <array>
#include <atomic>
#include <cstring>
#include <span>
#include <string_view>

namespace CombatIsolation
{
    namespace
    {
        using ActorConstants::kBlockedEventLogInterval;
        using ActorConstants::kInitialBlockedEventLogs;

        // Returns true when either side of a hostility or combat query is a
        // remote actor. A remote actor belongs to another player's world, so local actors never
        // treat it as an enemy and it never treats them as one.
        bool IsRemoteActorCombatQuery(const RE::Actor* a_actor, const RE::Actor* a_target, std::string_view a_path)
        {
            if (!ActorRegistry::IsRegistered(a_target) && !ActorRegistry::IsRegistered(a_actor))
            {
                return false;
            }

            static std::atomic_uint32_t suppressedCount{ 0 };
            const auto count = ++suppressedCount;
            if (count <= kInitialBlockedEventLogs || count % kBlockedEventLogInterval == 0)
            {
                logs::info(
                    "Suppressed local hostility involving remote actor: path={} actor={:08X} target={:08X} count={}",
                    a_path, a_actor ? a_actor->GetFormID() : 0, a_target ? a_target->GetFormID() : 0, count);
            }
            return true;
        }

        // Returns true when a registered remote actor is the aggressor of a hit.
        // Its native attack animations still run the local hit pipeline, but the
        // swing belongs to another player's world and must not affect this one.
        bool IsRemoteActorHit(const RE::Actor* a_aggressor, const RE::TESObjectREFR* a_target, std::string_view a_path)
        {
            if (!ActorRegistry::IsRegistered(a_aggressor))
            {
                return false;
            }

            static std::atomic_uint32_t suppressedCount{ 0 };
            const auto count = ++suppressedCount;
            if (count <= kInitialBlockedEventLogs || count % kBlockedEventLogInterval == 0)
            {
                logs::info("Suppressed remote actor hit: path={} aggressor={:08X} target={:08X} count={}", a_path,
                    a_aggressor->GetFormID(), a_target ? a_target->GetFormID() : 0, count);
            }
            return true;
        }

        // Returns true when a registered remote actor would release a projectile or
        // spell. Its bow draws and casts are presentation only.
        bool IsRemoteActorRelease(const RE::Actor* a_actor, std::string_view a_path)
        {
            if (!ActorRegistry::IsRegistered(a_actor))
            {
                return false;
            }

            static std::atomic_uint32_t suppressedCount{ 0 };
            const auto count = ++suppressedCount;
            if (count <= kInitialBlockedEventLogs || count % kBlockedEventLogInterval == 0)
            {
                logs::info("Suppressed remote actor release: path={} actor={:08X} count={}", a_path,
                    a_actor->GetFormID(), count);
            }
            return true;
        }

        // jmp [rip+0] followed by the 64-bit resume address.
        constexpr std::size_t kAbsoluteJumpSize = 14;
        constexpr std::size_t kBranchSize = 5;

        // Redirects a function's first instructions to a_thunk and returns the address
        // of a gateway that runs them and resumes the original function. The moved
        // instructions must be exactly a_prologue, at least five bytes, and position
        // independent. Returns 0 without patching if the bytes differ, for example on
        // another runtime or when another plugin already patched the function.
        std::uintptr_t WriteEntryDetour(
            REL::ID a_id, std::span<const std::uint8_t> a_prologue, std::uintptr_t a_thunk, std::string_view a_name)
        {
            const auto target = REL::Relocation<std::uintptr_t>{ a_id }.address();
            if (a_prologue.size() < kBranchSize
                || std::memcmp(reinterpret_cast<const void*>(target), a_prologue.data(), a_prologue.size()) != 0)
            {
                logs::error("Did not install {} hook: unexpected entry bytes at ID {}", a_name, a_id.id());
                return 0;
            }

            auto& trampoline = SKSE::GetTrampoline();
            auto* gateway = static_cast<std::uint8_t*>(trampoline.allocate(a_prologue.size() + kAbsoluteJumpSize));
            std::memcpy(gateway, a_prologue.data(), a_prologue.size());
            const std::array<std::uint8_t, 6> jump{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
            const std::uintptr_t resume = target + a_prologue.size();
            std::memcpy(gateway + a_prologue.size(), jump.data(), jump.size());
            std::memcpy(gateway + a_prologue.size() + jump.size(), &resume, sizeof(resume));

            // Pad the rest of a longer prologue so the patched entry disassembles
            // cleanly; execution never reaches these bytes.
            for (auto offset = kBranchSize; offset < a_prologue.size(); ++offset)
            {
                REL::safe_write(target + offset, std::uint8_t{ 0x90 });
            }
            (void)trampoline.write_branch<5>(target, a_thunk);
            return reinterpret_cast<std::uintptr_t>(gateway);
        }

        // Redirects the 5-byte call at a_id + a_offset to a_thunk and returns the
        // previous destination, which may be another plugin's detour. Returns 0
        // without patching if the instruction there is not a relative call.
        std::uintptr_t WriteCallDetour(
            REL::ID a_id, std::ptrdiff_t a_offset, std::uintptr_t a_thunk, std::string_view a_name)
        {
            const auto site = REL::Relocation<std::uintptr_t>{ a_id }.address() + a_offset;
            if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8)
            {
                logs::error("Did not install {} hook: no call at ID {} + {:#x}", a_name, a_id.id(), a_offset);
                return 0;
            }
            return SKSE::GetTrampoline().write_call<5>(site, a_thunk);
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
        constexpr std::array<std::uint8_t, 5> kSpellFirePrologue{
            0x40, 0x56, // push rsi
            0x57, // push rdi
            0x41, 0x56 // push r14
        };
        constexpr std::array<std::uint8_t, 6> kWeaponSwingHitPrologue{
            0x48, 0x8B, 0xC4, // mov rax, rsp
            0x88, 0x50, 0x10 // mov [rax+10h], dl
        };

        // A gateway plus the absolute jump write_branch<5> allocates.
        constexpr std::size_t EntryDetourTrampolineSize(std::size_t a_prologueSize)
        {
            return a_prologueSize + 2 * kAbsoluteJumpSize;
        }

        static_assert(EntryDetourTrampolineSize(kHostilityPrologue.size())
                + EntryDetourTrampolineSize(kIsHostileToActorPrologue.size())
                + EntryDetourTrampolineSize(kStartCombatPrologue.size())
                + EntryDetourTrampolineSize(kSpellFirePrologue.size())
                + EntryDetourTrampolineSize(kWeaponSwingHitPrologue.size()) + kAbsoluteJumpSize
            == kTrampolineSize);

        // The engine's hostility decision (ID 37534 on 1.7.104): aggression and
        // faction reaction toward a target. Its wrappers (IDs 37532 and 37533) serve
        // combat group target handling, StartCombat, and many AI callers.
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

        // The melee hit frame (ID 38603 on 1.7.104): sweeps the attacker's weapon
        // reach and hits the actors and objects it finds, applying damage, stagger,
        // impulses, and hit effects. Reached from the attack animation's HitFrame
        // annotation, possibly after being queued to another update pass.
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

        // The call from a projectile impact (ID 44218) to the actor hit (ID 38627),
        // which builds HitData for the shooter and applies it to the struck actor.
        struct ProjectileHitHook
        {
            static void Thunk(
                RE::Actor* a_shooter, RE::TESObjectREFR* a_target, RE::Projectile* a_projectile, bool a_leftHand)
            {
                if (IsRemoteActorHit(a_shooter, a_target, "ProjectileHit"))
                {
                    return;
                }
                _original(a_shooter, a_target, a_projectile, a_leftHand);
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

        // A remote actor channels its placeholder spell for the animation only.
        // Reports that it can always cast, so magicka or the casting-disabled flag
        // never interrupt the animation. SpellFireHook keeps the spell unreleased.
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

        // The magic caster's spell fire (ID 34407 on 1.7.104): checks the cast,
        // spends the cost, applies the spell or launches its projectile, and enters
        // the casting state. Reached from the M?h_SpellFire_Event annotations and,
        // while a concentration cast is casting, from the caster update. The hand's
        // release action (StartCastImpl) that selects the cast clip stays native.
        // The annotation handlers are not hooked instead: a remote actor's cast
        // reached this function without passing through their vtables (observed
        // in game).
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
            logs::info("Remote actor release hooks: arrowRelease={} castSpellImmediate={} checkCast={}",
                ArrowReleaseHandlerHook::_original.address() != 0, CastSpellImmediateHook::_original.address() != 0,
                CheckCastHook::_original.address() != 0);
        }
    }

    void InstallHooks()
    {
        // Virtual-table hooks resolve through the address library on every runtime.
        InstallReleaseHooks();

        // The entry bytes are verified against 1.7.104 only.
        if (!REL::Module::IsAE())
        {
            logs::warn(
                "Remote actor combat isolation hooks are verified only on Skyrim AE; local enemies may target actors");
            return;
        }

        HostilityHook::_original = WriteEntryDetour(REL::ID(37534), kHostilityPrologue,
            reinterpret_cast<std::uintptr_t>(&HostilityHook::Thunk), "hostility decision");
        IsHostileToActorHook::_original = WriteEntryDetour(REL::ID(37537), kIsHostileToActorPrologue,
            reinterpret_cast<std::uintptr_t>(&IsHostileToActorHook::Thunk), "IsHostileToActor");
        StartCombatHook::_original = WriteEntryDetour(REL::ID(38561), kStartCombatPrologue,
            reinterpret_cast<std::uintptr_t>(&StartCombatHook::Thunk), "StartCombat");
        WeaponSwingHitHook::_original = WriteEntryDetour(REL::ID(38603), kWeaponSwingHitPrologue,
            reinterpret_cast<std::uintptr_t>(&WeaponSwingHitHook::Thunk), "weapon swing hit");
        SpellFireHook::_original = WriteEntryDetour(
            REL::ID(34407), kSpellFirePrologue, reinterpret_cast<std::uintptr_t>(&SpellFireHook::Thunk), "spell fire");
        ProjectileHitHook::_original = WriteCallDetour(
            REL::ID(44218), 0x90, reinterpret_cast<std::uintptr_t>(&ProjectileHitHook::Thunk), "projectile hit");
        logs::info(
            "Remote actor combat isolation hooks: hostility={} isHostileToActor={} startCombat={} weaponSwingHit={} "
            "spellFire={} projectileHit={}",
            HostilityHook::_original.address() != 0, IsHostileToActorHook::_original.address() != 0,
            StartCombatHook::_original.address() != 0, WeaponSwingHitHook::_original.address() != 0,
            SpellFireHook::_original.address() != 0, ProjectileHitHook::_original.address() != 0);
    }
}
