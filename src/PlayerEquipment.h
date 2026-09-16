#pragma once

#include "EquipmentState.h"
#include "NPCSpawner.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace PlayerEquipment
{
	// Registers the TESEquipEvent sink that marks local equipment dirty. Requires
	// Skyrim's data-loaded state.
	void RegisterEventSinks();

	// Captures the local player's equipment after equip events settle (and once
	// per second as a safety net) and publishes rate-limited revisions.
	// a_flush captures and publishes immediately, so a pending weapon change
	// reaches peers before an attack action. Must run on the bound main-loop thread.
	void UpdateLocalEquipment(RE::PlayerCharacter& a_player, float a_delta, bool a_flush);

	// An accepted remote entry resolved to a local form.
	struct ResolvedHand
	{
		RE::TESBoundObject* object{ nullptr };
		bool twoHanded{ false };
	};

	// Per remote player receive state. Accessed only by the main-loop pass.
	struct RemoteEquipment
	{
		std::optional<Multiplayer::PlayerEquipment> latest;
		// Local forms for resolvedRevision after the vanilla filter. Revision zero
		// means no equipment was received, which reconciles to an empty inventory.
		std::uint64_t resolvedRevision{ 0 };
		bool resolved{ false };
		std::vector<RE::TESObjectARMO*> armor;
		ResolvedHand rightHand;
		ResolvedHand leftHand;

		// Reconciliation state of the current puppet reference.
		float checkElapsed{ 0.0F };
		float handDeferralElapsed{ 0.0F };
		float handsUnsettledElapsed{ 0.0F };
		// Mutation passes since the live state last matched. Bounded per revision.
		std::uint32_t attempts{ 0 };
		bool exhausted{ false };
		bool armorMatches{ false };
		bool handsMatch{ false };
	};

	void AcceptRemoteEquipment(
		RemoteEquipment& a_remote,
		const std::string& a_playerID,
		Multiplayer::PlayerEquipment&& a_equipment);

	// A new reference starts with an unknown inventory.
	void OnPuppetSpawned(RemoteEquipment& a_remote);

	// Makes the puppet's inventory and equipment equal the resolved set.
	void UpdateSpawnedPuppet(
		RemoteEquipment& a_remote,
		const std::string& a_playerID,
		NPCSpawner::ControlledNPC& a_npc,
		float a_delta);

	// True when the hands have reached the latest equipment, or reconciliation
	// gave up on that revision. Attacks and weapon draws wait for this.
	[[nodiscard]] bool AreHandsSettled(const RemoteEquipment& a_remote);

	// Weapon draws wait for hand reconciliation, bounded so an unresolvable swap
	// cannot hold the source's stance indefinitely.
	[[nodiscard]] bool IsReadyForWeaponDraw(const RemoteEquipment& a_remote);
}
