#include "runtime/PluginStartup.h"
#include "actor/AnimationFirewall.h"
#include "actor/CombatIsolation.h"
#include "actor/SurvivalPolicy.h"
#include "network/SocketIOClient.h"
#include "runtime/MainLoopHook.h"
#include "sync/SyncCoordinator.h"
#include "sync/equipment/EquipmentSync.h"
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

			logs::info(
				"Installed remote-replica actor policy, damage/kill firewall, magic firewall, and animation firewall");
		}

		void OnDataLoaded()
		{
			SurvivalPolicy::RegisterEventSinks();
			EquipmentSync::RegisterEventSinks();
			RE::ConsoleLog::GetSingleton()->Print("Skyrim Multiplayer initialized.");
			RE::ConsoleLog::GetSingleton()->Print(
				"SkyrimMP connecting to synchronization server.");
			Network::Initialize();
		}
	}

	void Install()
	{
		// Remote-actor entry detours and the UI overlay use the trampoline too,
		// so allocate first.
		SKSE::AllocTrampoline(
			14 + CombatIsolation::kTrampolineSize + UI::Overlay::kTrampolineSize);
		InstallRemoteActorHooks();
		MainLoopHook::Install();
		UI::Overlay::Install();

		SKSE::GetMessagingInterface()->RegisterListener(
			[](SKSE::MessagingInterface::Message* a_message) {
				switch (a_message->type) {
				case SKSE::MessagingInterface::kDataLoaded:
					OnDataLoaded();
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
