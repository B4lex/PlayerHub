#include "runtime/PluginStartup.h"
#include "actor/ActorMount.h"
#include "actor/ActorSkeleton.h"
#include "actor/AimPresentation.h"
#include "actor/AnimationFirewall.h"
#include "actor/CombatIsolation.h"
#include "actor/JumpSound.h"
#include "actor/PrecisionIsolation.h"
#include "actor/RaceMenuOverlays.h"
#include "actor/ReactionSuppression.h"
#include "actor/RemoteCollision.h"
#include "actor/ScenePackageSuppression.h"
#include "actor/SpellIsolation.h"
#include "actor/SurvivalPolicy.h"
#include "config/PluginConfig.h"
#include "forms/BeastForms.h"
#include "forms/ItemForms.h"
#include "network/ServerClient.h"
#include "runtime/FurnitureActivationHook.h"
#include "runtime/MainLoopHook.h"
#include "runtime/PlayerGraphEventHook.h"
#include "runtime/RemoteActorActivation.h"
#include "runtime/VanillaIntro.h"
#include "sync/SyncCoordinator.h"
#include "sync/equipment/EquipmentSync.h"
#include "sync/outfit/DefaultOutfit.h"
#include "sync/presence/PresenceSync.h"
#include "sync/trade/TradeSync.h"
#include "sync/voice/VoiceSync.h"
#include "text/Text.h"
#include "ui/Overlay.h"

namespace PluginStartup
{
    namespace
    {
        void InstallRemoteActorHooks()
        {
            AnimationFirewall::InstallHooks();
            AimPresentation::InstallHooks();
            SurvivalPolicy::InstallHooks();
            CombatIsolation::InstallHooks();
            SpellIsolation::InstallHooks();
            ReactionSuppression::InstallHooks();
            RemoteCollision::InstallHooks();
            ScenePackageSuppression::InstallHooks();
            JumpSound::InstallHooks();
            ActorSkeleton::InstallHooks();
            ActorMount::InstallHooks();

            logs::info(
                "Installed remote-replica actor policy, damage/kill firewall, magic firewall, spell world isolation, "
                "animation firewall, aim targets, alias and scene package suppression, reaction suppression, "
                "pass-through collision, jump sound attenuation, skeleton pose replacement, and dragon seating");
        }

        void OnDataLoaded()
        {
            // Usually done already by the overlay's first frame.
            Text::Initialize();
            SurvivalPolicy::RegisterEventSinks();
            PrecisionIsolation::Initialize();
            Forms::BuildItemCatalog();
            Forms::ResolveBeastForms();
            DefaultOutfit::Initialize();
            EquipmentSync::RegisterEventSinks();
            PresenceSync::RegisterEventSinks();
            RemoteActorActivation::RegisterEventSink();
            TradeSync::RegisterEventSinks();
            VoiceSync::RegisterEventSinks();
            VanillaIntro::Initialize();
            Network::Initialize();
        }
    }

    std::size_t TrampolineSize()
    {
        return 14 + CombatIsolation::kTrampolineSize + SpellIsolation::kTrampolineSize
            + ReactionSuppression::kTrampolineSize
            + RemoteCollision::kTrampolineSize + ScenePackageSuppression::kTrampolineSize
            + JumpSound::kTrampolineSize + ActorSkeleton::kTrampolineSize + UI::Overlay::kTrampolineSize;
    }

    void Install()
    {
        Config::Load();
        InstallRemoteActorHooks();
        MainLoopHook::Install();
        FurnitureActivationHook::Install();
        RemoteActorActivation::InstallHooks();
        PlayerGraphEventHook::Install();
        UI::Overlay::Install();

        SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_message) {
            switch (a_message->type)
            {
                case SKSE::MessagingInterface::kPostPostLoad:
                    RaceMenuOverlays::Initialize();
                    break;
                case SKSE::MessagingInterface::kDataLoaded:
                    OnDataLoaded();
                    break;
                case SKSE::MessagingInterface::kNewGame:
                    // SKSE passes the quest the new game starts.
                    VanillaIntro::OnNewGame(a_message->data);
                    break;
                case SKSE::MessagingInterface::kPreLoadGame:
                    PresenceSync::OnGameLoadStarted();
                    VanillaIntro::OnGameLoadStarted();
                    break;
                case SKSE::MessagingInterface::kPostLoadGame:
                    VanillaIntro::OnGameLoaded();
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
