#pragma once

#include "AppearanceState.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <string>

namespace PlayerAppearance
{
	// Captures the local player's appearance about once per second and publishes
	// debounced, rate-limited revisions. Re-publishes after every reconnect.
	// Must run on the bound main-loop thread.
	void UpdateLocalAppearance(RE::PlayerCharacter& a_player, float a_delta);

	// Per remote player receive state. Accessed only by the main-loop pass.
	struct RemoteAppearance
	{
		std::optional<Multiplayer::PlayerAppearance> latest;
		// Runtime actor base built for baseRevision. Bases are never freed: a
		// retired reference may still point at its base for a few frames.
		RE::TESNPC* base{ nullptr };
		std::uint64_t baseRevision{ 0 };
		std::uint64_t failedRevision{ 0 };
		// Revision the current puppet was spawned with; zero for the fallback base.
		std::uint64_t spawnedRevision{ 0 };
		RE::TESNPC* spawnedBase{ nullptr };
		float appearanceWaitElapsed{ 0.0F };
		float respawnCooldown{ 0.0F };
		bool colorsApplied{ false };
	};

	void AcceptRemoteAppearance(
		RemoteAppearance& a_remote,
		const std::string& a_playerID,
		Multiplayer::PlayerAppearance&& a_appearance);

	void AdvanceRemoteAppearance(RemoteAppearance& a_remote, float a_delta);

	// Returns the actor base a new puppet should use, building it for the latest
	// revision if needed. Returns nullptr while a first appearance is still
	// expected; afterwards falls back to the local player base.
	[[nodiscard]] RE::TESNPC* GetSpawnBase(
		RemoteAppearance& a_remote,
		const std::string& a_playerID,
		RE::TESNPC* a_localPlayerBase);

	void OnPuppetSpawned(RemoteAppearance& a_remote, RE::TESNPC* a_spawnedBase);

	// True when a newer appearance than the spawned one should replace the puppet.
	[[nodiscard]] bool RequiresRespawn(const RemoteAppearance& a_remote);

	// Applies post-3D presentation (hair and skin color) once per spawned actor.
	void UpdateSpawnedPuppet(
		RemoteAppearance& a_remote,
		const std::string& a_playerID,
		RE::Actor& a_actor,
		bool a_actorReady);
}
