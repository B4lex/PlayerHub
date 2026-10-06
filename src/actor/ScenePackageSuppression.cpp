#include "actor/ScenePackageSuppression.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"

#include <atomic>
#include <cstdint>
#include <string_view>

namespace ScenePackageSuppression
{
    namespace
    {
        using ActorConstants::kBlockedEventLogInterval;
        using ActorConstants::kInitialBlockedEventLogs;

        // A scene package action installs its package on the alias's actor directly
        // (ID 39149), bypassing package evaluation and so the alias package hook in
        // SurvivalPolicy. During a bard song it reinstalls BardAudienceTravelToLoc2
        // every pass, and BardAudienceStayInLocationAndApplaud once the song ends,
        // which takes furniture occupancy back.
        void LogRefusedScenePackage(std::string_view a_path, RE::Actor& a_actor,
            const RE::BGSSceneActionPackage& a_action, const RE::BGSScene* a_scene)
        {
            // Scene updates are not confined to the main loop.
            static std::atomic<std::uint32_t> s_refusedCount{ 0 };
            const auto refusedCount = ++s_refusedCount;
            if (refusedCount > kInitialBlockedEventLogs && refusedCount % kBlockedEventLogInterval != 0)
            {
                return;
            }

            const auto* quest = a_scene ? a_scene->parentQuest : nullptr;
            const auto* package = a_action.packages.empty() ? nullptr : a_action.packages.front();
            logs::debug(
                "Refused local scene package for remote actor: path={} actor={:08X} scene={:08X} quest={:08X} "
                "alias={} package={:08X} packages={} count={}",
                a_path, a_actor.GetHandle().native_handle(), a_scene ? a_scene->GetFormID() : 0,
                quest ? quest->GetFormID() : 0, a_action.actorID, package ? package->GetFormID() : 0,
                a_action.packages.size(), refusedCount);
        }

        struct StartActionHook
        {
            // BGSSceneActionPackage vfunc 0x10 (ID 24244): resolves the alias's actor
            // through the scene's quest and installs the first package whose
            // conditions it meets.
            static void Thunk(RE::BGSSceneActionPackage* a_action, RE::BGSScene* a_scene)
            {
                const auto* quest = a_action && a_scene ? a_scene->parentQuest : nullptr;
                RE::NiPointer<RE::TESObjectREFR> reference;
                if (quest)
                {
                    reference = quest->GetAliasedRef(a_action->actorID).get();
                }
                auto* actor = reference ? reference->As<RE::Actor>() : nullptr;
                if (!ActorRegistry::IsRegistered(actor))
                {
                    _original(a_action, a_scene);
                    return;
                }

                LogRefusedScenePackage("start", *actor, *a_action, a_scene);
                // What ID 24244 does when the alias has no actor to run the package:
                // the action is marked started and then ended (vfunc 0x11, ID 24245,
                // which marks a non-looping action done), so the scene moves on.
                a_action->unk14 |= 1;
                a_action->Unk_11();
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // ID 24252, which scene updates (IDs 24130 and 24173) call for each
        // running package action with the alias's actor, and which reinstalls the
        // package whenever the actor runs another one. One instance per call site,
        // since another plugin may already have chained either of them.
        template <std::uint64_t CallerID, std::ptrdiff_t CallOffset>
        struct UpdateActionHook
        {
            static void Thunk(RE::BGSSceneActionPackage* a_action, RE::BGSScene* a_scene, RE::Actor* a_actor)
            {
                if (!a_action || !ActorRegistry::IsRegistered(a_actor))
                {
                    _original(a_action, a_scene, a_actor);
                    return;
                }

                LogRefusedScenePackage("update", *a_actor, *a_action, a_scene);
            }

            static bool Install()
            {
                const auto site = REL::Relocation<std::uintptr_t>{ REL::ID(CallerID) }.address() + CallOffset;
                if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8)
                {
                    logs::error("Did not install remote actor scene package hook: no call at ID {} + {:#x}", CallerID,
                        CallOffset);
                    return false;
                }

                _original = SKSE::GetTrampoline().write_call<5>(site, reinterpret_cast<std::uintptr_t>(&Thunk));
                return true;
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };
    }

    void InstallHooks()
    {
        // The vtable slot and call sites are verified against 1.7.104 only.
        if (!REL::Module::IsAE())
        {
            logs::warn(
                "Remote actor scene package suppression is verified only on Skyrim AE; local scenes may "
                "move remote actors");
            return;
        }

        REL::Relocation<std::uintptr_t> actionVTable{ RE::VTABLE_BGSSceneActionPackage[0] };
        StartActionHook::_original = actionVTable.write_vfunc(0x10, StartActionHook::Thunk);
        const bool firstUpdateSite = UpdateActionHook<24130, 0x9A>::Install();
        const bool secondUpdateSite = UpdateActionHook<24173, 0x61>::Install();
        logs::debug("Remote actor scene package suppression hooks: startAction=true updateAction={}/{}",
            firstUpdateSite, secondUpdateSite);
    }
}
