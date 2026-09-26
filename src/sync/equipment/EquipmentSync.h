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
    // Registers the TESEquipEvent sink that marks local equipment dirty. Requires
    // Skyrim's data-loaded state.
    void RegisterEventSinks();

    // Captures the local player's equipment after equip events settle (and once
    // per second as a safety net) and publishes rate-limited revisions.
    // a_flush captures and publishes immediately, so a pending weapon change
    // reaches peers before an attack action. Local frame stage; must run on the
    // bound main-loop thread.
    void SyncEquipment(const LocalFrame& a_local, float a_delta, bool a_flush);

    // An accepted remote entry resolved to a local form.
    struct ResolvedHand
    {
        RE::TESBoundObject* object{ nullptr };
        bool twoHanded{ false };
        // The placeholder spell. It is equipped as a spell, never as an item.
        bool spell{ false };
    };

    // Per remote player receive state. Accessed only by the main-loop pass.
    struct RemoteState
    {
        std::optional<Multiplayer::PlayerEquipment> latest;
        // Local forms for resolvedRevision, with fallbacks for unresolved entries.
        // Revision zero means no equipment was received, which reconciles to an
        // empty inventory.
        std::uint64_t resolvedRevision{ 0 };
        bool resolved{ false };
        std::vector<RE::TESObjectARMO*> armor;
        ResolvedHand rightHand;
        ResolvedHand leftHand;
        RE::TESAmmo* ammo{ nullptr };

        // Reconciliation state of the current remote actor reference.
        float checkElapsed{ 0.0F };
        float inventoryCheckElapsed{ 0.0F };
        float handDeferralElapsed{ 0.0F };
        float handsUnsettledElapsed{ 0.0F };
        // Mutation passes since the live state last matched. Bounded per revision.
        std::uint32_t attempts{ 0 };
        bool exhausted{ false };
        bool armorMatches{ false };
        // Both hands and the equipped ammo, which a bow draw nocks.
        bool handsMatch{ false };
        // The last check held the hand swap back for a playing clip or transition.
        bool handsDeferred{ false };
    };

    void AcceptRemoteEquipment(
        RemoteState& a_remote, const std::string& a_playerID, Multiplayer::PlayerEquipment&& a_equipment);

    // A new reference starts with an unknown inventory.
    void OnActorSpawned(RemoteState& a_remote);

    // Remote frame stage: makes the actor's inventory and equipment equal the
    // resolved set.
    void SyncEquipment(RemoteState& a_remote, const std::string& a_playerID, RemoteActor& a_remoteActor, float a_delta);

    // True when the hands have reached the latest equipment, or reconciliation
    // gave up on that revision. Attacks and weapon draws wait for this.
    [[nodiscard]] bool AreHandsSettled(const RemoteState& a_remote);

    // Weapon draws wait for hand reconciliation, bounded so an unresolvable swap
    // cannot hold the source's stance indefinitely. The bound only counts time the
    // swap could apply, so a sheathe that defers it cannot release the draw with
    // the old weapon.
    [[nodiscard]] bool IsReadyForWeaponDraw(const RemoteState& a_remote);
}
