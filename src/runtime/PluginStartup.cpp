#include "runtime/PluginStartup.h"
#include "actor/AnimationFirewall.h"
#include "actor/CombatIsolation.h"
#include "actor/ReactionSuppression.h"
#include "actor/RemoteCollision.h"
#include "actor/ScenePackageSuppression.h"
#include "actor/SurvivalPolicy.h"
#include "config/PluginConfig.h"
#include "network/SocketIOClient.h"
#include "runtime/FurnitureActivationHook.h"
#include "runtime/MainLoopHook.h"
#include "sync/SyncCoordinator.h"
#include "sync/equipment/EquipmentFallback.h"
#include "sync/equipment/EquipmentSync.h"
#include "sync/presence/PresenceSync.h"
#include "ui/Overlay.h"

namespace PluginStartup
{
    namespace
    {
        void InstallRemoteActorHooks()
        {
            AnimationFirewall::InstallHooks();
            SurvivalPolicy::InstallHooks();
            CombatIsolation::InstallHooks();
            ReactionSuppression::InstallHooks();
            RemoteCollision::InstallHooks();
            ScenePackageSuppression::InstallHooks();

            logs::info(
                "Installed remote-replica actor policy, damage/kill firewall, magic firewall, animation firewall, "
                "alias and scene package suppression, reaction suppression, and pass-through collision");
        }

        void OnDataLoaded()
        {
            SurvivalPolicy::RegisterEventSinks();
            EquipmentFallback::Initialize();
            EquipmentSync::RegisterEventSinks();
            PresenceSync::RegisterEventSinks();
            RE::ConsoleLog::GetSingleton()->Print("Skyrim Multiplayer initialized.");
            RE::ConsoleLog::GetSingleton()->Print("SkyrimMP connecting to synchronization server.");
            Network::Initialize();
        }
    }

    void Install()
    {
        Config::Load();

        // Remote-actor entry detours and the UI overlay use the trampoline too,
        // so allocate first.
        SKSE::AllocTrampoline(14 + CombatIsolation::kTrampolineSize + ReactionSuppression::kTrampolineSize
            + RemoteCollision::kTrampolineSize + ScenePackageSuppression::kTrampolineSize
            + UI::Overlay::kTrampolineSize);
        InstallRemoteActorHooks();
        MainLoopHook::Install();
        FurnitureActivationHook::Install();
        UI::Overlay::Install();

        SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_message) {
            switch (a_message->type)
            {
                case SKSE::MessagingInterface::kDataLoaded:
                    OnDataLoaded();
                    break;
                case SKSE::MessagingInterface::kPreLoadGame:
                    PresenceSync::OnGameLoadStarted();
                    break;
                case SKSE::MessagingInterface::kSaveGame:
                    // Dispatched before the engine writes the save.
                    SyncCoordinator::OnGameSave();
                    break;
                default:
                    break;
            }
        });
    }
}
