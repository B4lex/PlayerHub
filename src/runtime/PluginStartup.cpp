#include "runtime/PluginStartup.h"
#include "actor/ActorMount.h"
#include "actor/ActorFace.h"
#include "actor/ActorSkeleton.h"
#include "actor/AimPresentation.h"
#include "actor/AnimationFirewall.h"
#include "actor/CombatIsolation.h"
#include "actor/JumpSound.h"
#include "actor/PrecisionIsolation.h"
#include "actor/ReactionSuppression.h"
#include "actor/RemoteCollision.h"
#include "actor/ScenePackageSuppression.h"
#include "actor/SKEEInterfaces.h"
#include "actor/SpellIsolation.h"
#include "actor/SurvivalPolicy.h"
#include "config/PluginConfig.h"
#include "forms/BeastForms.h"
#include "forms/ItemForms.h"
#include "network/ServerClient.h"
#include "runtime/FurnitureActivationHook.h"
#include "runtime/GameReverbHook.h"
#include "runtime/MainLoopHook.h"
#include "runtime/PlayerGraphEventHook.h"
#include "runtime/RemoteActorActivation.h"
#include "runtime/VanillaIntro.h"
#include "sync/SyncCoordinator.h"
#include "sync/equipment/EquipmentSync.h"
#include "sync/look/DefaultLook.h"
#include "sync/presence/PresenceSync.h"
#include "sync/skeleton/PoseWindow.h"
#include "sync/trade/TradeSync.h"
#include "sync/voice/VoiceSync.h"
#include "sync/voicechat/VoiceChatSync.h"
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
            ActorFace::InstallHooks();
            ActorMount::InstallHooks();

            logs::info(
                "Installed remote-replica actor policy, damage/kill firewall, magic firewall, spell world isolation, "
                "animation firewall, aim targets, alias and scene package suppression, reaction suppression, "
                "pass-through collision, jump sound attenuation, skeleton pose and face replacement, and dragon seating");
        }

        void OnDataLoaded()
        {
            // Usually done already by the overlay's first frame.
            Text::Initialize();
            SurvivalPolicy::RegisterEventSinks();
            PrecisionIsolation::Initialize();
            Forms::BuildItemCatalog();
            Forms::ResolveBeastForms();
            DefaultLook::Initialize();
            EquipmentSync::RegisterEventSinks();
            PresenceSync::RegisterEventSinks();
            RemoteActorActivation::RegisterEventSink();
            TradeSync::RegisterEventSinks();
            VoiceSync::RegisterEventSinks();
            VanillaIntro::Initialize();
            Network::Initialize();
            VoiceChatSync::Initialize();
        }
    }

    std::size_t TrampolineSize()
    {
        return 14 + CombatIsolation::kTrampolineSize + SpellIsolation::kTrampolineSize
            + ReactionSuppression::kTrampolineSize
            + RemoteCollision::kTrampolineSize + ScenePackageSuppression::kTrampolineSize
            + JumpSound::kTrampolineSize + ActorSkeleton::kTrampolineSize + ActorFace::kTrampolineSize
            + UI::Overlay::kTrampolineSize;
    }

    void Install()
    {
        Config::Load();
        InstallRemoteActorHooks();
        MainLoopHook::Install();
        FurnitureActivationHook::Install();
        RemoteActorActivation::InstallHooks();
        PlayerGraphEventHook::Install();
        GameReverbHook::Install();
        UI::Overlay::Install();

        SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_message) {
            switch (a_message->type)
            {
                case SKSE::MessagingInterface::kPostPostLoad:
                    SKEEInterfaces::Initialize();
                    break;
                case SKSE::MessagingInterface::kDataLoaded:
                    OnDataLoaded();
                    break;
                case SKSE::MessagingInterface::kNewGame:
                    // SKSE passes the quest the new game starts.
                    VanillaIntro::OnNewGame(a_message->data);
                    PoseWindow::ReleaseHolds("new-game");
                    break;
                case SKSE::MessagingInterface::kPreLoadGame:
                    PoseWindow::ReleaseHolds("game-load");
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
