#include "actor/SurvivalPolicy.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace SurvivalPolicy
{
    namespace
    {
        using ActorConstants::ShouldLogOccurrence;

        bool ApplyAuthoritativeJumpState(RE::Actor& a_actor, bool a_jumping)
        {
            static const RE::BSFixedString inJumpStateVariable{ "bInJumpState" };
            return a_actor.SetGraphVariableBool(inJumpStateVariable, a_jumping);
        }

        bool ShouldSuppressRemoteActorEffect(const RE::MagicItem* a_magicItem, const RE::Effect* a_effect)
        {
            // Effects propagated through projectiles or explosions do not always
            // retain a caster.
            if (a_magicItem && a_magicItem->GetSpellType() == RE::MagicSystem::SpellType::kVoicePower)
            {
                return true;
            }

            if (!a_effect || !a_effect->baseEffect)
            {
                return false;
            }

            return a_effect->IsHostile() || a_effect->baseEffect->IsDetrimental();
        }

        std::pair<std::uint32_t, std::uint32_t> BlockMagicEffect(
            RE::MagicTarget* a_target, const RE::MagicTarget::AddTargetData& a_targetData)
        {
            if (!a_target || !ShouldSuppressRemoteActorEffect(a_targetData.magicItem, a_targetData.effect))
            {
                return {};
            }

            return ActorRegistry::BlockMagicEffect(a_target);
        }

        std::uint32_t DispelSuppressedMagicEffects(RE::Actor& a_actor)
        {
            auto* magicTarget = a_actor.AsMagicTarget();
            if (!magicTarget)
            {
                return 0;
            }

            auto* activeEffects = magicTarget->GetActiveEffectList();
            if (!activeEffects)
            {
                return 0;
            }

            std::vector<RE::ActiveEffect*> effectsToDispel;
            for (auto* activeEffect : *activeEffects)
            {
                if (!activeEffect || activeEffect->flags.any(RE::ActiveEffect::Flag::kDispelled))
                {
                    continue;
                }

                if (ShouldSuppressRemoteActorEffect(activeEffect->spell, activeEffect->effect))
                {
                    effectsToDispel.push_back(activeEffect);
                }
            }

            for (auto* activeEffect : effectsToDispel)
            {
                const auto* baseEffect = activeEffect->GetBaseObject();
                logs::debug(
                    "Dispelling magic effect that bypassed the remote actor firewall: actor={:08X} spell={:08X} "
                    "effect={:08X}",
                    a_actor.GetHandle().native_handle(), activeEffect->spell ? activeEffect->spell->GetFormID() : 0,
                    baseEffect ? baseEffect->GetFormID() : 0);
                activeEffect->Dispel(true);
            }

            return static_cast<std::uint32_t>(effectsToDispel.size());
        }

        void LogBlockedHealthDamage(
            std::uint32_t a_actorHandle, std::uint32_t a_blockedDamageCount, std::string_view a_path, float a_damage)
        {
            if (a_blockedDamageCount != 0 && ShouldLogOccurrence(a_blockedDamageCount))
            {
                logs::debug("Blocked local health damage to remote actor: actor={:08X} path={} damage={:.3f} count={}",
                    a_actorHandle, a_path, a_damage, a_blockedDamageCount);
            }
        }

        class HitEventSink final : public RE::BSTEventSink<RE::TESHitEvent>
        {
        public:
            static HitEventSink* GetSingleton()
            {
                static HitEventSink singleton;
                return std::addressof(singleton);
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
            {
                if (!a_event || !a_event->target)
                {
                    return RE::BSEventNotifyControl::kContinue;
                }

                auto* actor = a_event->target->As<RE::Actor>();
                const auto [actorHandle, requestCount] = ActorRegistry::RequestHitObservation(actor);
                if (requestCount != 0 && ShouldLogOccurrence(requestCount))
                {
                    logs::debug(
                        "Observed local hit on remote actor; watching for an animation reaction: actor={:08X} "
                        "aggressor={:08X} source={:08X} projectile={:08X} count={}",
                        actorHandle, a_event->cause ? a_event->cause->GetFormID() : 0, a_event->source,
                        a_event->projectile, requestCount);
                }

                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            HitEventSink() = default;
        };

        class MagicEffectEventSink final : public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>
        {
        public:
            static MagicEffectEventSink* GetSingleton()
            {
                static MagicEffectEventSink singleton;
                return std::addressof(singleton);
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESMagicEffectApplyEvent* a_event, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override
            {
                if (!a_event || !a_event->target)
                {
                    return RE::BSEventNotifyControl::kContinue;
                }

                const auto* effect = RE::TESForm::LookupByID<RE::EffectSetting>(a_event->magicEffect);
                if (!effect || (!effect->IsHostile() && !effect->IsDetrimental()))
                {
                    return RE::BSEventNotifyControl::kContinue;
                }

                auto* actor = a_event->target->As<RE::Actor>();
                const auto [actorHandle, requestCount] = ActorRegistry::RequestHitObservation(actor);
                if (requestCount != 0 && ShouldLogOccurrence(requestCount))
                {
                    logs::debug(
                        "Observed harmful magic effect on remote actor; watching for an animation reaction: "
                        "actor={:08X} caster={:08X} effect={:08X} count={}",
                        actorHandle, a_event->caster ? a_event->caster->GetFormID() : 0, a_event->magicEffect,
                        requestCount);
                }

                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            MagicEffectEventSink() = default;
        };

        struct MagicTargetHook
        {
            static bool Thunk(RE::MagicTarget* a_target, RE::MagicTarget::AddTargetData& a_targetData)
            {
                const auto [actorHandle, blockedEffectCount] = BlockMagicEffect(a_target, a_targetData);
                if (blockedEffectCount == 0)
                {
                    return _original(a_target, a_targetData);
                }

                if (ShouldLogOccurrence(blockedEffectCount))
                {
                    logs::debug(
                        "Blocked magic effect before it reached remote actor: actor={:08X} caster={:08X} spell={:08X} "
                        "effect={:08X} count={}",
                        actorHandle, a_targetData.caster ? a_targetData.caster->GetFormID() : 0,
                        a_targetData.magicItem ? a_targetData.magicItem->GetFormID() : 0,
                        a_targetData.effect && a_targetData.effect->baseEffect
                            ? a_targetData.effect->baseEffect->GetFormID()
                            : 0,
                        blockedEffectCount);
                }

                // Archetypes and scripts, such as Ice Form's freeze, act through the ActiveEffect itself.
                return false;
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        struct HealthDamageHook
        {
            static void Thunk(RE::Actor* a_actor, RE::Actor* a_attacker, float a_damage)
            {
                const auto [actorHandle, blockedDamageCount] = ActorRegistry::BlockHealthDamage(a_actor);
                if (blockedDamageCount != 0)
                {
                    LogBlockedHealthDamage(actorHandle, blockedDamageCount, "HandleHealthDamage", a_damage);
                    return;
                }

                _original(a_actor, a_attacker, a_damage);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        struct ActorValueHook
        {
            static void Thunk(RE::ActorValueOwner* a_actorValueOwner, RE::ACTOR_VALUE_MODIFIER a_modifier,
                RE::ActorValue a_actorValue, float a_value)
            {
                if (a_modifier == RE::ACTOR_VALUE_MODIFIER::kDamage && a_actorValue == RE::ActorValue::kHealth
                    && a_value < 0.0F)
                {
                    const auto [actorHandle, blockedDamageCount]
                        = ActorRegistry::BlockHealthActorValueDamage(a_actorValueOwner);
                    if (blockedDamageCount != 0)
                    {
                        LogBlockedHealthDamage(actorHandle, blockedDamageCount, "ModActorValue", -a_value);
                        return;
                    }
                }

                _original(a_actorValueOwner, a_modifier, a_actorValue, a_value);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        struct KillDyingHook
        {
            static void Thunk(RE::Actor* a_actor)
            {
                const auto [actorHandle, blockedDamageCount] = ActorRegistry::BlockHealthDamage(a_actor);
                if (blockedDamageCount != 0)
                {
                    LogBlockedHealthDamage(actorHandle, blockedDamageCount, "KillDying", 0.0F);
                    return;
                }

                _original(a_actor);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        struct KillHook
        {
            static void Thunk(
                RE::Actor* a_actor, RE::Actor* a_attacker, float a_damage, bool a_sendEvent, bool a_ragdollInstant)
            {
                const auto [actorHandle, blockedDamageCount] = ActorRegistry::BlockHealthDamage(a_actor);
                if (blockedDamageCount != 0)
                {
                    LogBlockedHealthDamage(actorHandle, blockedDamageCount, "KillImpl", a_damage);
                    return;
                }

                _original(a_actor, a_attacker, a_damage, a_sendEvent, a_ragdollInstant);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        struct AliasPackageHook
        {
            static RE::TESPackage* Thunk(RE::Actor* a_actor)
            {
                // Quest and scene aliases recruit bystanders, and their packages
                // outrank the do-nothing package and are reselected every pass.
                if (!ActorRegistry::IsRegistered(a_actor))
                {
                    return _original(a_actor);
                }

                // Called from Skyrim's package evaluation, which is not confined to
                // the main loop.
                static std::atomic<std::uint32_t> s_refusedAliasPackageCount{ 0 };
                const auto refusedCount = ++s_refusedAliasPackageCount;
                if (ShouldLogOccurrence(refusedCount))
                {
                    logs::debug("Refused local alias package for remote actor: actor={:08X} count={}",
                        a_actor ? a_actor->GetHandle().native_handle() : 0, refusedCount);
                }
                return nullptr;
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        bool IsLocalReactionPackage(RE::PACKAGE_TYPE a_packageType)
        {
            switch (a_packageType)
            {
                case RE::PACKAGE_TYPE::kFleeNotCombat:
                case RE::PACKAGE_TYPE::kAlarm:
                case RE::PACKAGE_TYPE::kFlee:
                case RE::PACKAGE_TYPE::kSpectator:
                case RE::PACKAGE_TYPE::kSearchForAttacker:
                case RE::PACKAGE_TYPE::kAvoidPlayer:
                    return true;
                default:
                    return false;
            }
        }

        void EnforceRemoteActorSurvival(RE::Actor& a_actor)
        {
            auto& runtimeData = a_actor.GetActorRuntimeData();
            runtimeData.boolFlags.set(RE::Actor::BOOL_FLAGS::kEssential, RE::Actor::BOOL_FLAGS::kProtected);

            const auto* actorState = a_actor.AsActorState();
            const auto previousLifeState = actorState ? actorState->GetLifeState() : RE::ACTOR_LIFE_STATE::kAlive;
            bool recoveredLifeState = false;
            switch (previousLifeState)
            {
                case RE::ACTOR_LIFE_STATE::kDying:
                case RE::ACTOR_LIFE_STATE::kDead:
                case RE::ACTOR_LIFE_STATE::kRecycle:
                    a_actor.Resurrect(false, a_actor.Is3DLoaded());
                    recoveredLifeState = true;
                    break;
                case RE::ACTOR_LIFE_STATE::kEssentialDown:
                case RE::ACTOR_LIFE_STATE::kBleedout:
                    a_actor.SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);
                    a_actor.PotentiallyFixRagdollState();
                    recoveredLifeState = true;
                    break;
                default:
                    break;
            }

            auto* actorValueOwner = a_actor.AsActorValueOwner();
            const float currentHealth
                = actorValueOwner ? actorValueOwner->GetActorValue(RE::ActorValue::kHealth) : 0.0F;
            const float maximumHealth = a_actor.GetActorValueMax(RE::ActorValue::kHealth);
            const float missingHealth = maximumHealth - currentHealth;
            const bool restoredHealth = actorValueOwner && std::isfinite(missingHealth) && missingHealth > 0.0F;
            if (restoredHealth)
            {
                actorValueOwner->RestoreActorValue(RE::ActorValue::kHealth, missingHealth);
            }

            if (recoveredLifeState || restoredHealth)
            {
                const auto [actorHandle, interventionCount] = ActorRegistry::BlockHealthDamage(&a_actor);
                std::uint32_t observationRequestCount = 0;
                if (recoveredLifeState)
                {
                    const auto [observationActorHandle, requestCount] = ActorRegistry::RequestHitObservation(&a_actor);
                    (void)observationActorHandle;
                    observationRequestCount = requestCount;
                }
                if (ShouldLogOccurrence(interventionCount))
                {
                    logs::debug(
                        "Reasserted remote actor survival state: actor={:08X} previousLifeState={} "
                        "health={:.3f}/{:.3f} resurrected={} observationRequest={}",
                        actorHandle, static_cast<std::underlying_type_t<RE::ACTOR_LIFE_STATE>>(previousLifeState),
                        currentHealth, maximumHealth, recoveredLifeState, observationRequestCount);
                }
            }
        }

        struct RemoteActorUpdateHook
        {
            static void Thunk(RE::Actor* a_actor, float a_delta)
            {
                // Reasserted around Skyrim's actor update, since the local grounded
                // controller overwrites bInJumpState during it.
                std::optional<ActorRegistry::UpdatePolicy> policy;
                if (GameThread::IsAuthoritative())
                {
                    policy = ActorRegistry::GetUpdatePolicy(a_actor);
                }
                const bool protectRemoteActor = policy && !policy->dead;
                if (protectRemoteActor)
                {
                    ApplyRemoteReplicaPolicy(*a_actor);
                    (void)ApplyAuthoritativeJumpState(*a_actor, policy->authoritativeJumping);
                }
                _original(a_actor, a_delta);
                if (protectRemoteActor)
                {
                    (void)ApplyAuthoritativeJumpState(*a_actor, policy->authoritativeJumping);
                    EnforceRemoteActorSurvival(*a_actor);
                }
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };
    }

    void InstallHooks()
    {
        REL::Relocation<std::uintptr_t> characterVTable{ RE::VTABLE_Character[0] };
        const auto actorUpdateIndex = REL::Relocate(0xAD, 0xAD, 0xAF);
        RemoteActorUpdateHook::_original = characterVTable.write_vfunc(actorUpdateIndex, RemoteActorUpdateHook::Thunk);

        // TESObjectREFR contributes vtables 0-3. Character vtable 4 is the
        // MagicTarget subobject, whose vfunc 1 attaches a new ActiveEffect.
        REL::Relocation<std::uintptr_t> characterMagicTargetVTable{ RE::VTABLE_Character[4] };
        MagicTargetHook::_original = characterMagicTargetVTable.write_vfunc(0x1, MagicTargetHook::Thunk);

        const auto handleHealthDamageIndex = REL::Relocate(0x104, 0x104, 0x106);
        HealthDamageHook::_original = characterVTable.write_vfunc(handleHealthDamageIndex, HealthDamageHook::Thunk);

        const auto killDyingIndex = REL::Relocate(0xAA, 0xAA, 0xAC);
        KillDyingHook::_original = characterVTable.write_vfunc(killDyingIndex, KillDyingHook::Thunk);

        const auto killIndex = REL::Relocate(0x10E, 0x10E, 0x110);
        KillHook::_original = characterVTable.write_vfunc(killIndex, KillHook::Thunk);

        // Character vtable 5 is its ActorValueOwner subobject. This catches health
        // damage applied directly by traps, scripts, hazards, or engine paths that do
        // not pass through Actor::HandleHealthDamage.
        REL::Relocation<std::uintptr_t> characterActorValueOwnerVTable{ RE::VTABLE_Character[5] };
        ActorValueHook::_original = characterActorValueOwnerVTable.write_vfunc(0x6, ActorValueHook::Thunk);

        // Actor::CheckForCurrentAliasPackage. Its index is below the first entry
        // VR shifts, so it is the same on every runtime.
        AliasPackageHook::_original = characterVTable.write_vfunc(0x49, AliasPackageHook::Thunk);
        logs::debug("Remote actor package suppression hook: aliasPackage=true");
    }

    void RegisterEventSinks()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }

        auto* eventSources = RE::ScriptEventSourceHolder::GetSingleton();
        if (!eventSources)
        {
            logs::critical("Unable to register the remote-replica hit event sink");
            return;
        }

        eventSources->AddEventSink(HitEventSink::GetSingleton());
        eventSources->AddEventSink(MagicEffectEventSink::GetSingleton());
        registered = true;
        logs::debug("Registered remote-replica hit and magic observation event sinks");
    }

    void ApplyRemoteReplicaPolicy(RE::Actor& a_actor)
    {
        PROFILE_FUNCTION();
        // This reference-local flag reduces normal friendly-hit presentation.
        // Survival is also reasserted because some concrete engine and console
        // routines bypass virtual damage hooks entirely.
        a_actor.formFlags |= RE::TESObjectREFR::RecordFlags::kIgnoreFriendlyHits;
        // The hostility decision treats a player teammate as the player, so every
        // enemy of the local player would also attack the remote actor.
        a_actor.GetActorRuntimeData().boolBits.reset(RE::Actor::BOOL_BITS::kPlayerTeammate);
        EnforceRemoteActorSurvival(a_actor);
        const auto dispelledEffects = DispelSuppressedMagicEffects(a_actor);
        if (dispelledEffects != 0)
        {
            const auto [actorHandle, requestCount] = ActorRegistry::RequestHitObservation(&a_actor);
            logs::debug(
                "Removed suppressed magic from remote actor; watching for an animation reaction: actor={:08X} "
                "effects={} request={}",
                actorHandle, dispelledEffects, requestCount);
        }

        auto& runtimeData = a_actor.GetActorRuntimeData();
        auto& flags = runtimeData.boolFlags;
        flags.reset(RE::Actor::BOOL_FLAGS::kCanDoFavor, RE::Actor::BOOL_FLAGS::kCanSpeak,
            RE::Actor::BOOL_FLAGS::kAttackOnSight, RE::Actor::BOOL_FLAGS::kAngryWithPlayer);
        flags.set(RE::Actor::BOOL_FLAGS::kAttackingDisabled, RE::Actor::BOOL_FLAGS::kDoNotShowOnStealthMeter);
        if (ActorRegistry::IsCastAnimationOwned(&a_actor))
        {
            flags.reset(RE::Actor::BOOL_FLAGS::kCastingDisabled);
        }
        else
        {
            flags.set(RE::Actor::BOOL_FLAGS::kCastingDisabled);
        }

        // A do-nothing package does not itself opt out of combat. In particular,
        // CombatBehaviorDodgeThreat can still run for an unaggressive actor when a
        // nearby attack is detected. Disable that live process/controller path while
        // leaving ordinary actor and animation updates enabled.
        if (runtimeData.currentProcess)
        {
            runtimeData.currentProcess->ignoringCombat = true;
        }
        if (auto* middleHighProcess = a_actor.GetMiddleHighProcess())
        {
            middleHighProcess->preventCombat = true;
        }
        if (runtimeData.combatController)
        {
            runtimeData.combatController->ignoringCombat = true;
            runtimeData.combatController->inactive = true;
        }
    }

    void AssertDoNothingPackage(RE::Actor& a_actor)
    {
        a_actor.InitiateDoNothingPackage();

        // The packages quest and scene systems assign to a bystander outrank the
        // do-nothing package and win reselection on every pass. A run-once package
        // outranks those and survives reselection.
        auto* process = a_actor.GetActorRuntimeData().currentProcess;
        if (!process)
        {
            return;
        }

        auto* doNothingPackage = process->currentPackage.package;
        if (doNothingPackage && doNothingPackage->packData.packType.get() == RE::PACKAGE_TYPE::kDoNothing)
        {
            process->SetRunOncePackage(doNothingPackage, std::addressof(a_actor));
        }
    }

    void SanitizeRemoteCombatState(RE::Actor& a_actor, bool a_mayResetPackage)
    {
        PROFILE_FUNCTION();
        auto& runtimeData = a_actor.GetActorRuntimeData();
        const auto cowerRequests = ActorRegistry::ConsumeCombatSanitizationRequests(a_actor);

        const auto* currentPackage = a_actor.GetCurrentPackage();
        const auto packageType = currentPackage ? currentPackage->packData.packType.get() : RE::PACKAGE_TYPE::kNone;
        const bool reactionPackage = IsLocalReactionPackage(packageType);
        // A hit makes the engine re-evaluate packages. It then picks
        // DefaultMasterPackage (kPackage), which is not a reaction package, so
        // nothing above catches it, and which sheathes the weapon it has no reason
        // to hold, fighting the authoritative weapon state for as long as it runs.
        // A remote actor is driven by the do-nothing package alone.
        const bool unownedPackage = a_mayResetPackage && currentPackage && packageType != RE::PACKAGE_TYPE::kDoNothing;
        const bool wasInCombat = a_actor.IsInCombat();
        const bool hadCombatTarget = static_cast<bool>(runtimeData.currentCombatTarget);

        bool wasFleeing = false;
        if (const auto* middleHighProcess = a_actor.GetMiddleHighProcess())
        {
            wasFleeing = middleHighProcess->isFleeing;
        }
        if (runtimeData.combatController && runtimeData.combatController->state)
        {
            wasFleeing = wasFleeing || runtimeData.combatController->state->isFleeing;
        }

        const bool needsAIReset = cowerRequests != 0 || reactionPackage || wasInCombat || hadCombatTarget || wasFleeing;
        if (needsAIReset)
        {
            if (wasInCombat || hadCombatTarget || runtimeData.combatController)
            {
                a_actor.StopCombat();
            }
            runtimeData.currentCombatTarget.reset();
            if (auto* middleHighProcess = a_actor.GetMiddleHighProcess())
            {
                middleHighProcess->isFleeing = false;
            }
            if (runtimeData.combatController && runtimeData.combatController->state)
            {
                runtimeData.combatController->state->isFleeing = false;
            }
            if (reactionPackage || cowerRequests != 0)
            {
                a_actor.StopAlarmOnActor();
            }
            if (a_actor.GetEmotionType() == RE::EmotionType::kFear)
            {
                a_actor.SetEmotionType(RE::EmotionType::kNeutral);
            }
        }
        if (needsAIReset || unownedPackage)
        {
            AssertDoNothingPackage(a_actor);
        }
        if (unownedPackage)
        {
            // Main-loop thread only, like the rest of this function.
            static std::uint32_t s_packageTakeoverCount = 0;
            const auto takeoverCount = ++s_packageTakeoverCount;
            if (ShouldLogOccurrence(takeoverCount))
            {
                logs::debug("Took back local actor package: actor={:08X} package={} packageForm={:08X} count={}",
                    a_actor.GetHandle().native_handle(),
                    static_cast<std::underlying_type_t<RE::PACKAGE_TYPE>>(packageType), currentPackage->GetFormID(),
                    takeoverCount);
            }
        }

        const auto* actorState = a_actor.AsActorState();
        const auto weaponState = actorState ? actorState->GetWeaponState() : RE::WEAPON_STATE::kSheathed;
        const bool authoritativeWeaponDrawn = ActorRegistry::GetAuthoritativeWeaponDrawnState(a_actor).value_or(false);
        const bool weaponTransitionPending = actorState
            && (authoritativeWeaponDrawn ? weaponState != RE::WEAPON_STATE::kDrawn
                                         : weaponState != RE::WEAPON_STATE::kSheathed);
        std::uint32_t observationRequestCount = 0;
        if (cowerRequests != 0)
        {
            const auto [actorHandle, requestCount] = ActorRegistry::RequestHitObservation(&a_actor);
            (void)actorHandle;
            observationRequestCount = requestCount;
        }

        if (needsAIReset || unownedPackage || weaponTransitionPending)
        {
            const auto [actorHandle, sanitizationCount] = ActorRegistry::RecordCombatSanitization(a_actor);
            if (cowerRequests != 0 || ShouldLogOccurrence(sanitizationCount))
            {
                logs::debug(
                    "Sanitized local actor combat presentation: actor={:08X} cowerEvents={} package={} "
                    "unownedPackage={} combat={} target={} fleeing={} weaponState={} desiredWeaponDrawn={} "
                    "sanitization={} observationRequest={}",
                    actorHandle, cowerRequests, static_cast<std::underlying_type_t<RE::PACKAGE_TYPE>>(packageType),
                    unownedPackage, wasInCombat, hadCombatTarget, wasFleeing,
                    static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(weaponState), authoritativeWeaponDrawn,
                    sanitizationCount, observationRequestCount);
            }
        }
    }
}
