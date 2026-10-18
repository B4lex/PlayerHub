#pragma once

#include "protocol/EquipmentState.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct RemoteActor;

namespace EquipmentSync
{
    void RegisterEventSinks();

    void SyncEquipment(const LocalFrame& a_local, float a_delta, bool a_flush);

    struct ResolvedHand
    {
        RE::TESBoundObject* object{ nullptr };
        bool twoHanded{ false };
        bool spell{ false };
    };

    struct RemoteState
    {
        std::optional<Multiplayer::PlayerEquipment> latest;
        std::uint64_t resolvedRevision{ 0 };
        bool resolved{ false };
        std::vector<RE::TESObjectARMO*> armor;
        ResolvedHand rightHand;
        ResolvedHand leftHand;
        RE::TESAmmo* ammo{ nullptr };
        ResolvedHand beastRightHand;
        ResolvedHand beastLeftHand;
        std::vector<RE::TESObjectARMO*> beastArmor;

        float checkElapsed{ 0.0F };
        float inventoryCheckElapsed{ 0.0F };
        float handDeferralElapsed{ 0.0F };
        float handsUnsettledElapsed{ 0.0F };
        std::uint32_t attempts{ 0 };
        bool exhausted{ false };
        bool armorMatches{ false };
        bool handsMatch{ false };
        bool handsDeferred{ false };
    };

    void AcceptRemoteEquipment(
        RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, Multiplayer::PlayerEquipment&& a_equipment);

    void OnActorSpawned(RemoteState& a_remote);

    void SyncEquipment(RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, RemoteActor& a_remoteActor, float a_delta);
    // A beast actor only.
    void SyncBeastEquipment(
        RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, RemoteActor& a_remoteActor, float a_delta);

    [[nodiscard]] bool AreHandsSettled(const RemoteState& a_remote);

    [[nodiscard]] bool IsReadyForWeaponDraw(const RemoteState& a_remote);
}
