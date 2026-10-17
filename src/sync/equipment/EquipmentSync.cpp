#include "sync/equipment/EquipmentSync.h"
#include "actor/ActorAnimation.h"
#include "actor/RemoteActor.h"
#include "forms/FormLookup.h"
#include "network/ServerClient.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"
#include "sync/equipment/FakeLight.h"
#include "sync/equipment/SpellStandIn.h"
#include "sync/outfit/DefaultOutfit.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <span>
#include <string_view>
#include <utility>

namespace EquipmentSync
{
    namespace
    {
        constexpr float kCaptureQuietIntervalSeconds = 0.1F;
        constexpr float kCapturePollIntervalSeconds = 1.0F;
        constexpr float kMinimumPublishIntervalSeconds = 0.25F;
        constexpr float kReconcileCheckIntervalSeconds = 0.25F;
        constexpr float kMatchedInventoryCheckIntervalSeconds = 5.0F;
        constexpr float kMaximumHandDeferralSeconds = 2.0F;
        constexpr float kMaximumWeaponDrawWaitSeconds = 1.0F;
        constexpr std::uint32_t kMaximumReconcileAttempts = 4;
        constexpr std::uint32_t kMaximumTemplateDepth = 8;
        constexpr RE::FormID kPlayerReferenceFormID = 0x00000014;
        constexpr RE::FormID kRightHandEquipSlotFormID = 0x00013F42;
        constexpr RE::FormID kLeftHandEquipSlotFormID = 0x00013F43;
        constexpr std::int32_t kSynchronizedAmmoCount = 20;

        using Multiplayer::EquipmentArmor;
        using Multiplayer::EquipmentHand;
        using Multiplayer::EquipmentHandKind;
        using EquipmentData = Multiplayer::PlayerEquipment;

        std::atomic_bool localEquipmentDirty{ true };

        class EquipEventSink final : public RE::BSTEventSink<RE::TESEquipEvent>
        {
        public:
            static EquipEventSink* GetSingleton()
            {
                static EquipEventSink singleton;
                return std::addressof(singleton);
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
            {
                // The event may fire off the main-loop thread and before the change is complete.
                if (a_event && a_event->actor && a_event->actor->GetFormID() == kPlayerReferenceFormID)
                {
                    localEquipmentDirty.store(true, std::memory_order_release);
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        bool IsPlayableRace(const RE::TESRace* a_race)
        {
            return a_race && a_race->data.flags.all(RE::RACE_DATA::Flag::kPlayable);
        }

        // Enchanted variants point at their unenchanted item through a template.
        template <class T>
        T* GetTemplateRoot(T* a_form, T* T::* a_template)
        {
            for (std::uint32_t depth = 0; a_form && a_form->*a_template && depth < kMaximumTemplateDepth; ++depth)
            {
                a_form = a_form->*a_template;
            }
            return a_form;
        }

        constexpr bool IsSynchronizedWeaponType(RE::WEAPON_TYPE a_type)
        {
            return a_type >= RE::WEAPON_TYPE::kOneHandSword && a_type <= RE::WEAPON_TYPE::kCrossbow;
        }

        std::optional<EquipmentHand> CaptureHand(RE::TESForm* a_form, bool a_leftHand, std::uint32_t& a_skipped)
        {
            if (!a_form)
            {
                return std::nullopt;
            }

            if (auto* weapon = a_form->As<RE::TESObjectWEAP>())
            {
                const auto type = weapon->GetWeaponType();
                // The engine may also return a two-handed weapon for the left hand.
                if (!IsSynchronizedWeaponType(type)
                    || (a_leftHand && Multiplayer::IsTwoHandedWeaponType(static_cast<std::uint8_t>(type))))
                {
                    return std::nullopt;
                }
                weapon = GetTemplateRoot(weapon, &RE::TESObjectWEAP::templateWeapon);
                const auto reference = FormLookup::MakeFormReference(weapon);
                if (!reference)
                {
                    ++a_skipped;
                    return std::nullopt;
                }
                return EquipmentHand{ .form = *reference,
                    .kind = EquipmentHandKind::kWeapon,
                    .weaponType = static_cast<std::uint8_t>(type) };
            }

            if (a_form->Is(RE::FormType::Spell) || a_form->Is(RE::FormType::Scroll))
            {
                return SpellStandIn::Capture(*a_form->As<RE::MagicItem>(), a_skipped);
            }

            auto* light = a_form->As<RE::TESObjectLIGH>();
            if (!light || !a_leftHand || !light->CanBeCarried())
            {
                return std::nullopt;
            }
            const auto reference = FormLookup::MakeFormReference(light);
            if (!reference)
            {
                ++a_skipped;
                return std::nullopt;
            }
            return EquipmentHand{ .form = *reference, .kind = EquipmentHandKind::kTorch };
        }

        // Equipped arrows or bolts are a worn inventory entry. Actor::GetCurrentAmmo
        // reads MiddleHighProcessData::bothHands, which is absent below the
        // middle-high process level.
        RE::TESAmmo* GetWornAmmo(RE::Actor& a_actor)
        {
            const auto inventory = a_actor.GetInventory([](RE::TESBoundObject& a_object) { return a_object.IsAmmo(); });
            for (const auto& [object, data] : inventory)
            {
                const auto& [count, entry] = data;
                if (count > 0 && entry && entry->IsWorn())
                {
                    return object->As<RE::TESAmmo>();
                }
            }
            return nullptr;
        }

        std::optional<EquipmentData> CaptureLocalEquipment(RE::PlayerCharacter& a_player, std::uint32_t& a_skipped)
        {
            // Transformations unequip everything.
            if (!IsPlayableRace(a_player.GetRace()))
            {
                return std::nullopt;
            }

            EquipmentData equipment;
            const auto inventory
                = a_player.GetInventory([](RE::TESBoundObject& a_object) { return a_object.IsArmor(); });
            for (const auto& [object, data] : inventory)
            {
                const auto& [count, entry] = data;
                auto* armor = object ? object->As<RE::TESObjectARMO>() : nullptr;
                if (!armor || count <= 0 || !entry || !entry->IsWorn()
                    || (armor->GetFormFlags() & RE::TESObjectARMO::RecordFlags::kNonPlayable) != 0)
                {
                    continue;
                }

                armor = GetTemplateRoot(armor, &RE::TESObjectARMO::templateArmor);
                const auto reference = FormLookup::MakeFormReference(armor);
                const auto slotMask = armor->GetSlotMask().underlying();
                if (!reference || slotMask == 0)
                {
                    ++a_skipped;
                    continue;
                }
                const bool duplicate = std::ranges::any_of(equipment.armor,
                    [&reference](const EquipmentArmor& a_entry) { return a_entry.form == *reference; });
                if (duplicate)
                {
                    continue;
                }
                if (equipment.armor.size() >= EquipmentData::kMaximumArmor)
                {
                    ++a_skipped;
                    continue;
                }
                equipment.armor.push_back(EquipmentArmor{ .form = *reference, .slotMask = slotMask });
            }

            equipment.rightHand = CaptureHand(a_player.GetEquippedObject(false), false, a_skipped);
            equipment.leftHand = CaptureHand(a_player.GetEquippedObject(true), true, a_skipped);
            if (equipment.rightHand && Multiplayer::IsTwoHandedWeaponType(equipment.rightHand->weaponType))
            {
                equipment.leftHand.reset();
            }
            if (auto* ammo = GetWornAmmo(a_player))
            {
                if (auto reference = FormLookup::MakeFormReference(ammo))
                {
                    equipment.ammo = std::move(*reference);
                }
                else
                {
                    ++a_skipped;
                }
            }
            DefaultOutfit::Select(equipment);
            return equipment;
        }

        template <class T>
        struct Resolution
        {
            T* form{ nullptr };
            std::string_view dropReason;
        };

        Resolution<RE::TESObjectARMO> ResolveArmor(const EquipmentArmor& a_entry)
        {
            auto* form = FormLookup::ResolveForm(a_entry.form);
            if (!form)
            {
                return { .dropReason = "missing" };
            }
            auto* armor = form->As<RE::TESObjectARMO>();
            if (!armor)
            {
                return { .dropReason = "wrong-type" };
            }
            return { .form = armor };
        }

        Resolution<RE::TESAmmo> ResolveAmmo(const Multiplayer::FormReference& a_reference)
        {
            auto* form = FormLookup::ResolveForm(a_reference);
            if (!form)
            {
                return { .dropReason = "missing" };
            }
            auto* ammo = form->As<RE::TESAmmo>();
            if (!ammo)
            {
                return { .dropReason = "wrong-type" };
            }
            return { .form = ammo };
        }

        Resolution<RE::TESBoundObject> ResolveHand(const EquipmentHand& a_entry)
        {
            auto* form = FormLookup::ResolveForm(a_entry.form);
            if (!form)
            {
                return { .dropReason = "missing" };
            }
            if (a_entry.kind == EquipmentHandKind::kTorch)
            {
                auto* light = form->As<RE::TESObjectLIGH>();
                if (!light || !light->CanBeCarried())
                {
                    return { .dropReason = "wrong-type" };
                }
                return { .form = light };
            }
            auto* weapon = form->As<RE::TESObjectWEAP>();
            if (!weapon || !IsSynchronizedWeaponType(weapon->GetWeaponType()))
            {
                return { .dropReason = "wrong-type" };
            }
            return { .form = weapon };
        }

        std::string FormatStandIn(std::span<RE::TESObjectARMO* const> a_forms)
        {
            std::string text;
            for (const auto* form : a_forms)
            {
                text += std::format("{}{:08X}", text.empty() ? "" : ",", form->GetFormID());
            }
            return text.empty() ? "none" : text;
        }

        std::string FormatStandIn(const RE::TESForm* a_form)
        {
            return a_form ? std::format("{:08X}", a_form->GetFormID()) : "none";
        }

        void LogUnresolvedEntry(Multiplayer::PeerSlot a_playerID, std::uint64_t a_revision, std::string_view a_slot,
            const Multiplayer::FormReference& a_form, std::string_view a_reason, std::string_view a_standIn)
        {
            logs::warn(
                "Equipment sync could not resolve remote item: player={} revision={} slot={} item={}|{:06X} "
                "reason={} standIn={}",
                a_playerID, static_cast<unsigned long long>(a_revision), a_slot, a_form.plugin, a_form.localFormID,
                a_reason, a_standIn);
        }

        ResolvedHand ResolveHandEntry(Multiplayer::PeerSlot a_playerID, std::uint64_t a_revision, std::string_view a_slot,
            const std::optional<EquipmentHand>& a_entry, const Multiplayer::DefaultOutfit& a_outfit)
        {
            if (!a_entry)
            {
                return {};
            }
            if (a_entry->kind == EquipmentHandKind::kSpell)
            {
                const auto resolution = SpellStandIn::Resolve(*a_entry);
                if (!resolution.dropReason.empty())
                {
                    LogUnresolvedEntry(a_playerID, a_revision, a_slot, a_entry->form, resolution.dropReason,
                        std::format(
                            "{}:{}", resolution.spell ? resolution.source : "none", FormatStandIn(resolution.spell)));
                }
                return resolution.spell ? ResolvedHand{ .object = resolution.spell, .spell = true } : ResolvedHand{};
            }
            const auto resolution = ResolveHand(*a_entry);
            auto* object = resolution.form;
            if (!object)
            {
                object = DefaultOutfit::FindHand(a_outfit, *a_entry);
                LogUnresolvedEntry(
                    a_playerID, a_revision, a_slot, a_entry->form, resolution.dropReason, FormatStandIn(object));
                if (!object)
                {
                    return {};
                }
            }
            if (auto* light = object->As<RE::TESObjectLIGH>())
            {
                object = std::addressof(FakeLight::Get(*light));
            }
            const auto* weapon = object->As<RE::TESObjectWEAP>();
            return ResolvedHand{ .object = object,
                .twoHanded
                = weapon && Multiplayer::IsTwoHandedWeaponType(static_cast<std::uint8_t>(weapon->GetWeaponType())) };
        }

        void ResetReconciliation(RemoteState& a_remote)
        {
            a_remote.checkElapsed = kReconcileCheckIntervalSeconds;
            a_remote.inventoryCheckElapsed = 0.0F;
            a_remote.handDeferralElapsed = 0.0F;
            a_remote.handsUnsettledElapsed = 0.0F;
            a_remote.attempts = 0;
            a_remote.exhausted = false;
            a_remote.armorMatches = false;
            a_remote.handsMatch = false;
            a_remote.handsDeferred = false;
        }

        void EnsureResolved(RemoteState& a_remote, Multiplayer::PeerSlot a_playerID)
        {
            const auto revision = a_remote.latest ? a_remote.latest->revision : 0;
            if (a_remote.resolved && a_remote.resolvedRevision == revision)
            {
                return;
            }

            a_remote.resolved = true;
            a_remote.resolvedRevision = revision;
            a_remote.armor.clear();
            a_remote.rightHand = {};
            a_remote.leftHand = {};
            a_remote.ammo = nullptr;
            const float handDeferralElapsed = a_remote.handDeferralElapsed;
            ResetReconciliation(a_remote);
            a_remote.handDeferralElapsed = handDeferralElapsed;
            if (!a_remote.latest)
            {
                return;
            }

            const auto& equipment = *a_remote.latest;
            std::vector<RE::TESObjectARMO*> resolvedArmor;
            std::uint32_t missingSlots = 0;
            std::vector<std::pair<const EquipmentArmor*, std::string_view>> unresolved;
            for (const auto& entry : equipment.armor)
            {
                const auto resolution = ResolveArmor(entry);
                if (!resolution.form)
                {
                    unresolved.emplace_back(&entry, resolution.dropReason);
                    missingSlots |= entry.slotMask;
                    continue;
                }
                if (std::ranges::find(resolvedArmor, resolution.form) == resolvedArmor.end())
                {
                    resolvedArmor.push_back(resolution.form);
                }
            }
            const auto standIns
                = DefaultOutfit::SelectArmor(equipment.defaultOutfit, resolvedArmor, missingSlots, a_remote.armor);
            for (const auto& [entry, reason] : unresolved)
            {
                std::array<RE::TESObjectARMO*, Multiplayer::DefaultOutfit::kMaximumArmor> entryStandIns{};
                std::size_t entryStandInCount = 0;
                for (std::size_t piece = 0; piece < standIns.size(); ++piece)
                {
                    if (standIns[piece] && (equipment.defaultOutfit.armor[piece].slotMask & entry->slotMask) != 0)
                    {
                        entryStandIns[entryStandInCount++] = standIns[piece];
                    }
                }
                LogUnresolvedEntry(a_playerID, revision, "armor", entry->form, reason,
                    FormatStandIn(std::span{ entryStandIns }.first(entryStandInCount)));
            }
            for (const auto* armor : resolvedArmor)
            {
                if (std::ranges::find(a_remote.armor, armor) == a_remote.armor.end())
                {
                    logs::warn("Equipment sync left out resolved armor overlapping a higher slot: player={} "
                               "revision={} armor={:08X}",
                        a_playerID, static_cast<unsigned long long>(revision), armor->GetFormID());
                }
            }
            a_remote.rightHand
                = ResolveHandEntry(a_playerID, revision, "rightHand", equipment.rightHand, equipment.defaultOutfit);
            a_remote.leftHand
                = ResolveHandEntry(a_playerID, revision, "leftHand", equipment.leftHand, equipment.defaultOutfit);
            if (a_remote.rightHand.twoHanded && a_remote.leftHand.object)
            {
                logs::warn(
                    "Equipment sync dropped left hand under a local two-handed weapon: player={} revision={} "
                    "rightHand={:08X} leftHand={:08X}",
                    a_playerID, static_cast<unsigned long long>(revision), a_remote.rightHand.object->GetFormID(),
                    a_remote.leftHand.object->GetFormID());
                a_remote.leftHand = {};
            }
            if (!equipment.ammo.IsEmpty())
            {
                const auto resolution = ResolveAmmo(equipment.ammo);
                a_remote.ammo = resolution.form;
                if (!a_remote.ammo)
                {
                    a_remote.ammo = DefaultOutfit::FindAmmo(equipment.defaultOutfit);
                    LogUnresolvedEntry(a_playerID, revision, "ammo", equipment.ammo, resolution.dropReason,
                        FormatStandIn(a_remote.ammo));
                }
            }

            logs::debug(
                "Equipment sync resolved remote equipment: player={} revision={} armor={}/{} rightHand={:08X} "
                "leftHand={:08X} ammo={:08X}",
                a_playerID, static_cast<unsigned long long>(revision), a_remote.armor.size(), equipment.armor.size(),
                a_remote.rightHand.object ? a_remote.rightHand.object->GetFormID() : 0,
                a_remote.leftHand.object ? a_remote.leftHand.object->GetFormID() : 0,
                a_remote.ammo ? a_remote.ammo->GetFormID() : 0);
        }

        const RE::BGSEquipSlot* GetHandSlot(bool a_leftHand)
        {
            // Vanilla LeftHand/RightHand equip slots. BGSDefaultObjectManager::GetObject
            // is unusable here because windows.h defines GetObject as a macro.
            return RE::TESForm::LookupByID<RE::BGSEquipSlot>(
                a_leftHand ? kLeftHandEquipSlotFormID : kRightHandEquipSlotFormID);
        }

        // Unarmed is reported as nothing or as a hand-to-hand weapon; both are empty.
        RE::TESBoundObject* GetEquippedHandObject(const RE::Actor& a_actor, bool a_leftHand)
        {
            auto* form = a_actor.GetEquippedObject(a_leftHand);
            if (!form)
            {
                return nullptr;
            }
            if (const auto* weapon = form->As<RE::TESObjectWEAP>();
                weapon && weapon->GetWeaponType() == RE::WEAPON_TYPE::kHandToHandMelee)
            {
                return nullptr;
            }
            return form->As<RE::TESBoundObject>();
        }

        bool IsLeftHandMatched(const RemoteState& a_remote, const RE::TESBoundObject* a_equippedLeft)
        {
            if (a_remote.rightHand.twoHanded)
            {
                // The engine may report the two-handed weapon for both hands.
                return !a_equippedLeft || a_equippedLeft == a_remote.rightHand.object;
            }
            return a_equippedLeft == a_remote.leftHand.object;
        }

        bool IsPresentationMatched(RE::Actor& a_actor, const RemoteState& a_remote)
        {
            if (GetEquippedHandObject(a_actor, false) != a_remote.rightHand.object
                || !IsLeftHandMatched(a_remote, GetEquippedHandObject(a_actor, true)))
            {
                return false;
            }

            auto* changes = a_actor.GetInventoryChanges();
            if (!changes || !changes->entryList)
            {
                return false;
            }
            std::size_t wornArmor = 0;
            RE::TESBoundObject* wornAmmo = nullptr;
            for (const auto* entry : *changes->entryList)
            {
                if (!entry || !entry->object || !entry->IsWorn())
                {
                    continue;
                }
                if (auto* armor = entry->object->As<RE::TESObjectARMO>())
                {
                    if (std::ranges::find(a_remote.armor, armor) == a_remote.armor.end())
                    {
                        return false;
                    }
                    ++wornArmor;
                }
                else if (entry->object->IsAmmo() && !wornAmmo)
                {
                    wornAmmo = entry->object;
                }
            }
            return wornArmor == a_remote.armor.size() && wornAmmo == a_remote.ammo;
        }

        bool IsHandSwapDeferred(const RE::Actor& a_actor, const RemoteActor& a_remoteActor)
        {
            if (ActorAnimation::IsAnimationActionPlaying(a_remoteActor)
                || ActorAnimation::IsHandPresentationPlaying(a_remoteActor)
                || ActorAnimation::IsWeaponTransitionPlaying(a_remoteActor))
            {
                return true;
            }
            const auto* actorState = a_actor.AsActorState();
            if (!actorState)
            {
                return false;
            }
            switch (actorState->GetWeaponState())
            {
                case RE::WEAPON_STATE::kWantToDraw:
                case RE::WEAPON_STATE::kDrawing:
                case RE::WEAPON_STATE::kWantToSheathe:
                case RE::WEAPON_STATE::kSheathing:
                    return true;
                default:
                    return false;
            }
        }

        bool CanUnequipHandObject(const RE::TESBoundObject* a_object)
        {
            // Spells are not inventory items; UnequipSpell removes them instead.
            return a_object && (a_object->IsWeapon() || a_object->Is(RE::FormType::Light));
        }

        RE::SpellItem* AsSpell(RE::TESBoundObject* a_object)
        {
            return a_object ? a_object->As<RE::SpellItem>() : nullptr;
        }

        // ActorEquipManager::UnequipSpell, the target of Papyrus Actor.UnequipSpell,
        // which CommonLibSSE-NG does not expose. Like EquipSpell it runs the equip
        // pipeline, whose ActionForceEquip moves a drawn hand out of its spell pose;
        // Actor::DeselectSpell only clears the selection and leaves the pose.
        // ID 38902 on 1.7.104; SE 37946 was matched against the 1.5.97 disassembly
        // by its code and its callers.
        void UnequipSpell(RE::ActorEquipManager& a_equipManager, RE::Actor& a_actor, RE::SpellItem* a_spell,
            const RE::BGSEquipSlot* a_slot)
        {
            using func_t = void(RE::ActorEquipManager*, RE::Actor*, RE::SpellItem*, const RE::BGSEquipSlot*);
            static REL::Relocation<func_t> func{ RELOCATION_ID(37946, 38902) };
            func(std::addressof(a_equipManager), std::addressof(a_actor), a_spell, a_slot);
        }
    }

    void RegisterEventSinks()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }

        auto* eventSources = RE::ScriptEventSourceHolder::GetSingleton();
        if (!eventSources)
        {
            logs::critical("Equipment sync could not register the equip event sink");
            return;
        }
        eventSources->AddEventSink<RE::TESEquipEvent>(EquipEventSink::GetSingleton());
        registered = true;
        logs::debug("Registered local equipment event sink");
    }

    void SyncEquipment(const LocalFrame& a_local, float a_delta, bool a_flush)
    {
        PROFILE_FUNCTION();
        static float quietElapsed = 0.0F;
        static float pollElapsed = kCapturePollIntervalSeconds;
        static float publishElapsed = kMinimumPublishIntervalSeconds;
        static std::optional<EquipmentData> published;
        static std::optional<EquipmentData> pending;
        static std::uint64_t revision = 0;
        static bool dirty = false;
        static std::uint64_t publishedConnectionGeneration = 0;

        const float delta = FrameTime::Clamp(a_delta);
        if (localEquipmentDirty.exchange(false, std::memory_order_acq_rel))
        {
            // Coalesce multi-event changes such as a favorites swap or outfit change.
            dirty = true;
            quietElapsed = 0.0F;
        }
        else
        {
            quietElapsed += delta;
        }
        pollElapsed += delta;
        publishElapsed += delta;

        if (!Network::IsConnected())
        {
            return;
        }
        if (const auto generation = Network::GetConnectionGeneration(); generation != publishedConnectionGeneration)
        {
            publishedConnectionGeneration = generation;
            if (published)
            {
                logs::debug("Equipment sync republishing after connect: revision={} connectionGeneration={}",
                    static_cast<unsigned long long>(published->revision), static_cast<unsigned long long>(generation));
                Network::PublishLocalEquipment(*published);
                publishElapsed = 0.0F;
            }
        }

        const bool captureDue = a_flush || (dirty && quietElapsed >= kCaptureQuietIntervalSeconds)
            || pollElapsed >= kCapturePollIntervalSeconds;
        std::uint32_t skipped = 0;
        if (captureDue)
        {
            pollElapsed = 0.0F;
            if (auto captured = CaptureLocalEquipment(a_local.player, skipped))
            {
                dirty = false;
                if (published && published->HasSameItems(*captured))
                {
                    pending.reset();
                }
                else
                {
                    pending = std::move(captured);
                }
            }
        }

        if (!pending || (!a_flush && publishElapsed < kMinimumPublishIntervalSeconds))
        {
            return;
        }
        if (revision >= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        {
            logs::critical("Equipment sync cannot advance revision beyond the protocol limit");
            return;
        }
        pending->revision = ++revision;
        if (!Multiplayer::IsValidEquipment(*pending))
        {
            logs::warn("Equipment sync skipped invalid local equipment: revision={}",
                static_cast<unsigned long long>(revision));
            pending.reset();
            return;
        }

        logs::debug(
            "Equipment sync publishing local equipment: revision={} armor={} rightHand={}:{}|{:06X} "
            "leftHand={}:{}|{:06X} ammo={}|{:06X} outfit={}/{}/{} skipped={} flush={}",
            static_cast<unsigned long long>(pending->revision), pending->armor.size(),
            pending->rightHand ? Multiplayer::GetEquipmentHandKindName(pending->rightHand->kind) : "none",
            pending->rightHand ? pending->rightHand->form.plugin : "",
            pending->rightHand ? pending->rightHand->form.localFormID : 0,
            pending->leftHand ? Multiplayer::GetEquipmentHandKindName(pending->leftHand->kind) : "none",
            pending->leftHand ? pending->leftHand->form.plugin : "",
            pending->leftHand ? pending->leftHand->form.localFormID : 0,
            pending->ammo.IsEmpty() ? "<none>" : pending->ammo.plugin, pending->ammo.localFormID,
            pending->defaultOutfit.armor.size(), pending->defaultOutfit.hands.size(),
            !pending->defaultOutfit.ammo.IsEmpty(), skipped, a_flush);
        Network::PublishLocalEquipment(*pending);
        published = std::move(pending);
        pending.reset();
        publishElapsed = 0.0F;
    }

    void AcceptRemoteEquipment(
        RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, Multiplayer::PlayerEquipment&& a_equipment)
    {
        if (a_remote.latest && a_equipment.revision <= a_remote.latest->revision)
        {
            return;
        }

        logs::debug("Equipment sync accepted remote equipment: player={} revision={} armor={} rightHand={} leftHand={}",
            a_playerID, static_cast<unsigned long long>(a_equipment.revision), a_equipment.armor.size(),
            a_equipment.rightHand.has_value(), a_equipment.leftHand.has_value());
        a_remote.latest = std::move(a_equipment);
    }

    void OnActorSpawned(RemoteState& a_remote)
    {
        ResetReconciliation(a_remote);
    }

    void SyncEquipment(RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, RemoteActor& a_remoteActor, float a_delta)
    {
        PROFILE_FUNCTION();
        const float delta = FrameTime::Clamp(a_delta);
        EnsureResolved(a_remote, a_playerID);
        if (a_remote.handsMatch || a_remote.exhausted)
        {
            a_remote.handsUnsettledElapsed = 0.0F;
        }
        else if (!a_remote.handsDeferred)
        {
            a_remote.handsUnsettledElapsed += delta;
        }

        auto actorPtr = a_remoteActor.handle.get();
        auto* actor = actorPtr.get();
        if (!actor || !a_remoteActor.ready || !actor->Is3DLoaded())
        {
            return;
        }

        const bool deferralReleased = a_remote.handsDeferred && !IsHandSwapDeferred(*actor, a_remoteActor);
        const bool immediate = deferralReleased
            || (!(a_remote.armorMatches && a_remote.handsMatch) && !a_remote.exhausted && a_remote.attempts == 0);
        a_remote.checkElapsed += delta;
        if (!immediate && a_remote.checkElapsed < kReconcileCheckIntervalSeconds)
        {
            return;
        }
        const float sinceLastCheck = a_remote.checkElapsed;
        a_remote.checkElapsed = 0.0F;
        a_remote.inventoryCheckElapsed += sinceLastCheck;
        if (a_remote.armorMatches && a_remote.handsMatch
            && a_remote.inventoryCheckElapsed < kMatchedInventoryCheckIntervalSeconds
            && IsPresentationMatched(*actor, a_remote))
        {
            return;
        }
        a_remote.inventoryCheckElapsed = 0.0F;

        auto inventory = actor->GetInventory();
        auto* equippedRight = GetEquippedHandObject(*actor, false);
        auto* equippedLeft = GetEquippedHandObject(*actor, true);
        auto* equippedAmmo = GetWornAmmo(*actor);
        const bool handsMatch = equippedRight == a_remote.rightHand.object && IsLeftHandMatched(a_remote, equippedLeft)
            && equippedAmmo == a_remote.ammo;
        const bool handsDeferred = !handsMatch && IsHandSwapDeferred(*actor, a_remoteActor)
            && a_remote.handDeferralElapsed < kMaximumHandDeferralSeconds;

        std::map<RE::TESBoundObject*, std::int32_t> required;
        for (auto* armor : a_remote.armor)
        {
            required[armor] = 1;
        }
        for (const auto* hand : { &a_remote.rightHand, &a_remote.leftHand })
        {
            if (hand->object && !hand->spell)
            {
                ++required[hand->object];
            }
        }
        if (a_remote.ammo)
        {
            required[a_remote.ammo] = kSynchronizedAmmoCount;
        }
        if (handsDeferred)
        {
            // Removing an equipped hand object forces an unequip, so stale hand objects stay until a deferred
            // swap replaces them.
            for (RE::TESBoundObject* equipped :
                { equippedRight, equippedLeft, static_cast<RE::TESBoundObject*>(equippedAmmo) })
            {
                const auto entry = inventory.find(equipped);
                if ((CanUnequipHandObject(equipped) || (equipped && equipped == equippedAmmo))
                    && entry != inventory.end())
                {
                    auto& count = required[equipped];
                    count = (std::max)(count, entry->second.first);
                }
            }
        }

        bool inventoryMatches = true;
        for (const auto& [object, data] : inventory)
        {
            const auto requiredCount = required.contains(object) ? required[object] : 0;
            if ((std::max)(data.first, 0) != requiredCount)
            {
                inventoryMatches = false;
                break;
            }
        }
        for (const auto& [object, count] : required)
        {
            if (!inventory.contains(object) && count != 0)
            {
                inventoryMatches = false;
                break;
            }
        }
        const auto isWorn = [&inventory](RE::TESObjectARMO* a_armor) {
            const auto entry = inventory.find(a_armor);
            return entry != inventory.end() && entry->second.first > 0 && entry->second.second
                && entry->second.second->IsWorn();
        };
        const bool armorWorn = std::ranges::all_of(a_remote.armor, isWorn);

        // Hands only gate draws and attacks, so an unrelated inventory leftover must not hold them.
        a_remote.armorMatches = inventoryMatches && armorWorn;
        a_remote.handsMatch = handsMatch;
        a_remote.handsDeferred = handsDeferred;
        if (a_remote.armorMatches && a_remote.handsMatch)
        {
            if (a_remote.attempts != 0 || a_remote.exhausted)
            {
                logs::debug("Equipment sync reconciled actor: player={} actor={:08X} revision={} attempts={}",
                    a_playerID, a_remoteActor.handle.native_handle(),
                    static_cast<unsigned long long>(a_remote.resolvedRevision), a_remote.attempts);
            }
            a_remote.attempts = 0;
            a_remote.exhausted = false;
            a_remote.handDeferralElapsed = 0.0F;
            return;
        }
        if (a_remote.exhausted)
        {
            return;
        }

        if (handsDeferred)
        {
            a_remote.handDeferralElapsed += sinceLastCheck;
            if (inventoryMatches && armorWorn)
            {
                return;
            }
        }

        if (a_remote.attempts >= kMaximumReconcileAttempts)
        {
            a_remote.exhausted = true;
            logs::warn(
                "Equipment sync gave up reconciling actor: player={} actor={:08X} revision={} inventoryMatches={} "
                "armorWorn={} rightHand={:08X}/{:08X} leftHand={:08X}/{:08X}",
                a_playerID, a_remoteActor.handle.native_handle(),
                static_cast<unsigned long long>(a_remote.resolvedRevision), inventoryMatches, armorWorn,
                equippedRight ? equippedRight->GetFormID() : 0,
                a_remote.rightHand.object ? a_remote.rightHand.object->GetFormID() : 0,
                equippedLeft ? equippedLeft->GetFormID() : 0,
                a_remote.leftHand.object ? a_remote.leftHand.object->GetFormID() : 0);
            return;
        }
        ++a_remote.attempts;

        auto* equipManager = RE::ActorEquipManager::GetSingleton();
        if (!equipManager)
        {
            return;
        }

        std::uint32_t removed = 0;
        std::uint32_t added = 0;
        for (const auto& [object, data] : inventory)
        {
            const auto requiredCount = required.contains(object) ? required[object] : 0;
            if (data.first > requiredCount)
            {
                actor->RemoveItem(
                    object, data.first - requiredCount, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                ++removed;
            }
        }
        for (const auto& [object, count] : required)
        {
            const auto entry = inventory.find(object);
            const auto current = entry != inventory.end() ? (std::max)(entry->second.first, 0) : 0;
            if (current < count)
            {
                actor->AddObjectToContainer(object, nullptr, count - current, nullptr);
                ++added;
            }
        }

        std::uint32_t equippedArmor = 0;
        const auto wornInventory = actor->GetInventory([](RE::TESBoundObject& a_object) { return a_object.IsArmor(); });
        for (auto* armor : a_remote.armor)
        {
            const auto entry = wornInventory.find(armor);
            if (entry != wornInventory.end() && entry->second.second && entry->second.second->IsWorn())
            {
                continue;
            }
            equipManager->EquipObject(actor, armor, nullptr, 1, nullptr, false, true, false);
            ++equippedArmor;
        }

        bool swappedHands = false;
        if (!handsDeferred)
        {
            a_remote.handDeferralElapsed = 0.0F;
            const auto* rightSlot = GetHandSlot(false);
            const auto* leftSlot = GetHandSlot(true);
            // Removal above may already have unequipped stale hand objects.
            equippedRight = GetEquippedHandObject(*actor, false);
            equippedLeft = GetEquippedHandObject(*actor, true);
            if (equippedRight != a_remote.rightHand.object && CanUnequipHandObject(equippedRight))
            {
                equipManager->UnequipObject(actor, equippedRight, nullptr, 1, rightSlot, false, true, false);
                swappedHands = true;
            }
            if (!IsLeftHandMatched(a_remote, equippedLeft) && CanUnequipHandObject(equippedLeft))
            {
                equipManager->UnequipObject(actor, equippedLeft, nullptr, 1, leftSlot, false, true, false);
                swappedHands = true;
            }
            if (auto* spell = AsSpell(equippedRight); spell && !a_remote.rightHand.object)
            {
                UnequipSpell(*equipManager, *actor, spell, rightSlot);
                swappedHands = true;
            }
            if (auto* spell = AsSpell(equippedLeft); spell && !a_remote.leftHand.object)
            {
                UnequipSpell(*equipManager, *actor, spell, leftSlot);
                swappedHands = true;
            }
            equippedRight = GetEquippedHandObject(*actor, false);
            equippedLeft = GetEquippedHandObject(*actor, true);

            const auto equipHand = [&](const ResolvedHand& a_hand, const RE::BGSEquipSlot* a_slot) {
                if (a_hand.spell)
                {
                    equipManager->EquipSpell(actor, AsSpell(a_hand.object), a_slot);
                    return;
                }
                // Two-handed weapons use their own both-hands slot.
                equipManager->EquipObject(
                    actor, a_hand.object, nullptr, 1, a_hand.twoHanded ? nullptr : a_slot, false, true, false);
            };
            if (a_remote.rightHand.object && equippedRight != a_remote.rightHand.object)
            {
                equipHand(a_remote.rightHand, rightSlot);
                swappedHands = true;
            }
            if (a_remote.leftHand.object && equippedLeft != a_remote.leftHand.object)
            {
                equipHand(a_remote.leftHand, leftSlot);
                swappedHands = true;
            }
            if (a_remote.ammo && GetWornAmmo(*actor) != a_remote.ammo)
            {
                equipManager->EquipObject(
                    actor, a_remote.ammo, nullptr, kSynchronizedAmmoCount, nullptr, false, true, false);
                swappedHands = true;
            }
        }

        logs::debug(
            "Equipment sync reconciling actor: player={} actor={:08X} revision={} attempt={} removed={} added={} "
            "equippedArmor={} swappedHands={} handsDeferred={} rightHand={:08X} leftHand={:08X} ammo={:08X}",
            a_playerID, a_remoteActor.handle.native_handle(),
            static_cast<unsigned long long>(a_remote.resolvedRevision), a_remote.attempts, removed, added,
            equippedArmor, swappedHands, handsDeferred,
            a_remote.rightHand.object ? a_remote.rightHand.object->GetFormID() : 0,
            a_remote.leftHand.object ? a_remote.leftHand.object->GetFormID() : 0,
            a_remote.ammo ? a_remote.ammo->GetFormID() : 0);
    }

    bool AreHandsSettled(const RemoteState& a_remote)
    {
        const auto latestRevision = a_remote.latest ? a_remote.latest->revision : 0;
        return a_remote.resolved && a_remote.resolvedRevision == latestRevision
            && (a_remote.handsMatch || a_remote.exhausted);
    }

    bool IsReadyForWeaponDraw(const RemoteState& a_remote)
    {
        return AreHandsSettled(a_remote) || a_remote.handsUnsettledElapsed >= kMaximumWeaponDrawWaitSeconds;
    }
}
