#include "runtime/PluginStartup.h"
#include "actor/AnimationFirewall.h"
#include "actor/CombatIsolation.h"
#include "actor/JumpSound.h"
#include "actor/PrecisionIsolation.h"
#include "actor/ReactionSuppression.h"
#include "actor/RemoteCollision.h"
#include "actor/ScenePackageSuppression.h"
#include "actor/SurvivalPolicy.h"
#include "config/PluginConfig.h"
#include "forms/ItemForms.h"
#include "network/ServerClient.h"
#include "runtime/FurnitureActivationHook.h"
#include "runtime/MainLoopHook.h"
#include "runtime/RemoteActorActivation.h"
#include "sync/SyncCoordinator.h"
#include "sync/equipment/EquipmentSync.h"
#include "sync/outfit/DefaultOutfit.h"
#include "sync/presence/PresenceSync.h"
#include "sync/trade/TradeSync.h"
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
            JumpSound::InstallHooks();

            logs::info(
                "Installed remote-replica actor policy, damage/kill firewall, magic firewall, animation firewall, "
                "alias and scene package suppression, reaction suppression, pass-through collision, and jump sound "
                "attenuation");
        }

        void OnDataLoaded()
        {
            SurvivalPolicy::RegisterEventSinks();
            PrecisionIsolation::Initialize();
            Forms::BuildItemCatalog();
            DefaultOutfit::Initialize();
            EquipmentSync::RegisterEventSinks();
            PresenceSync::RegisterEventSinks();
            RemoteActorActivation::RegisterEventSink();
            TradeSync::RegisterEventSinks();
            Network::Initialize();
        }
    }

    void Install()
    {
        Config::Load();

        // The SKSE trampoline must be allocated before any hook writes through it.
        SKSE::AllocTrampoline(14 + CombatIsolation::kTrampolineSize + ReactionSuppression::kTrampolineSize
            + RemoteCollision::kTrampolineSize + ScenePackageSuppression::kTrampolineSize
            + JumpSound::kTrampolineSize + UI::Overlay::kTrampolineSize);
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
