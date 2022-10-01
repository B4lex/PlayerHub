#include "FrameUpdate.h"
#include "NPCSpawner.h"
#include "SocketIOClient.h"

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	FrameUpdate::Install();

	SKSE::GetMessagingInterface()->RegisterListener(
		[](SKSE::MessagingInterface::Message* a_message) {
			switch (a_message->type) {
			case SKSE::MessagingInterface::kDataLoaded:
				NPCSpawner::RegisterRemoteReplicaEventSinks();
				RE::ConsoleLog::GetSingleton()->Print("Skyrim Multiplayer initialized.");
				RE::ConsoleLog::GetSingleton()->Print(
					"SkyrimMP connecting to synchronization server.");
				Network::Initialize();
				break;
			default:
				break;
			}
		});

	return true;
}
