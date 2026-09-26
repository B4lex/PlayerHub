#include "sync/equipment/EquipmentSync.h"
#include "actor/ActorAnimation.h"
#include "actor/RemoteActor.h"
#include "forms/FormLookup.h"
#include "network/SocketIOClient.h"
#include "runtime/Profiling.h"
#include "sync/equipment/EquipmentFallback.h"

#include <algorithm>
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
        constexpr float kCaptureQuietInterval = 0.1F;
        constexpr float kCapturePollInterval = 1.0F;
        constexpr float kMinimumPublishInterval = 0.25F;
        constexpr float kReconcileCheckInterval = 0.25F;
        constexpr float kMaximumHandDeferral = 2.0F;
        constexpr float kMaximumWeaponDrawWait = 1.0F;
        constexpr std::uint32_t kMaximumReconcileAttempts = 4;
        constexpr std::uint32_t kMaximumTemplateDepth = 8;
        constexpr RE::FormID kPlayerReferenceFormID = 0x00000014;
        constexpr RE::FormID kRightHandEquipSlotFormID = 0x00013F42;
        constexpr RE::FormID kLeftHandEquipSlotFormID = 0x00013F43;
        // Enough arrows for the quiver to look filled. Remote bow shots never
        // consume them.
        constexpr std::int32_t kSynchronizedAmmoCount = 20;

        using Multiplayer::EquipmentArmor;
        using Multiplayer::EquipmentArmorType;
        using Multiplayer::EquipmentHand;
        using Multiplayer::EquipmentHandKind;
        using EquipmentData = Multiplayer::PlayerEquipment;

        // Healing Hands: a hand holding any spell shows this concentration spell,
        // whose hand effect and casting animation stand in for every spell. Remote
        // actors never release it.
        const Multiplayer::FormReference kPlaceholderSpell{ .plugin = "Skyrim.esm", .localFormID = 0x04D3F2 };

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
                // The event may fire off the main-loop thread and before the change is
                // complete. Only mark it; the main-loop pass captures after it settles.
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
        // Peers only see that base item.
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

        EquipmentArmorType GetArmorType(const RE::TESObjectARMO& a_armor)
        {
            switch (a_armor.GetArmorType())
            {
                case RE::BIPED_MODEL::ArmorType::kLightArmor:
                    return EquipmentArmorType::kLight;
                case RE::BIPED_MODEL::ArmorType::kHeavyArmor:
                    return EquipmentArmorType::kHeavy;
                default:
                    return EquipmentArmorType::kClothing;
            }
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
                // Bare fists are an empty hand. A two-handed weapon is reported by the
                // right hand only, even if the engine also returns it for the left.
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
                // Peers show a placeholder spell, so which spell it is does not matter.
                return EquipmentHand{ .kind = EquipmentHandKind::kSpell };
            }

            auto* light = a_form->As<RE::TESObjectLIGH>();
            if (!light || !a_leftHand || !light->CanBeCarried())
            {
                // Anything else is sent as an empty hand.
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
        // reports nothing for the player.
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
            // Transformations unequip everything. Keep the last playable equipment.
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
                equipment.armor.push_back(
                    EquipmentArmor{ .form = *reference, .slotMask = slotMask, .armorType = GetArmorType(*armor) });
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
            return equipment;
        }

        template <class T>
        struct Resolution
        {
            T* form{ nullptr };
            std::string_view dropReason;
        };

        // The single receive policy: resolve an entry to a local form of the
        // expected type, or return why it is unresolved. Any plugin the receiver
        // has loaded resolves. An unresolved entry gets a configured fallback.
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
            if (a_entry.kind == EquipmentHandKind::kSpell)
            {
                auto* spell = FormLookup::ResolveForm<RE::SpellItem>(kPlaceholderSpell);
                if (!spell)
                {
                    return { .dropReason = "missing-placeholder-spell" };
                }
                return { .form = spell };
            }
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

        std::string FormatFallback(std::span<RE::TESObjectARMO* const> a_forms)
        {
            std::string text;
            for (const auto* form : a_forms)
            {
                text += std::format("{}{:08X}", text.empty() ? "" : ",", form->GetFormID());
            }
            return text.empty() ? "none" : text;
        }

        std::string FormatFallback(const RE::TESForm* a_form)
        {
            return a_form ? std::format("{:08X}", a_form->GetFormID()) : "none";
        }

        void LogUnresolvedEntry(const std::string& a_playerID, std::uint64_t a_revision, std::string_view a_slot,
            const Multiplayer::FormReference& a_form, std::string_view a_reason, std::string_view a_fallback)
        {
            logs::warn(
                "Equipment sync could not resolve remote item: player={} revision={} slot={} item={}|{:06X} "
                "reason={} fallback={}",
                a_playerID, static_cast<unsigned long long>(a_revision), a_slot, a_form.plugin, a_form.localFormID,
                a_reason, a_fallback);
        }

        ResolvedHand ResolveHandEntry(const std::string& a_playerID, std::uint64_t a_revision, std::string_view a_slot,
            const std::optional<EquipmentHand>& a_entry)
        {
            if (!a_entry)
            {
                return {};
            }
            const auto resolution = ResolveHand(*a_entry);
            const bool spell = a_entry->kind == EquipmentHandKind::kSpell;
            auto* object = resolution.form;
            if (!object)
            {
                object = EquipmentFallback::FindHand(*a_entry);
                LogUnresolvedEntry(a_playerID, a_revision, a_slot, spell ? kPlaceholderSpell : a_entry->form,
                    resolution.dropReason, FormatFallback(object));
                if (!object)
                {
                    return {};
                }
            }
            const auto* weapon = object->As<RE::TESObjectWEAP>();
            return ResolvedHand{ .object = object,
                .twoHanded
                = weapon && Multiplayer::IsTwoHandedWeaponType(static_cast<std::uint8_t>(weapon->GetWeaponType())),
                .spell = spell };
        }

        void ResetReconciliation(RemoteState& a_remote)
        {
            a_remote.checkElapsed = kReconcileCheckInterval;
            a_remote.handDeferralElapsed = 0.0F;
            a_remote.handsUnsettledElapsed = 0.0F;
            a_remote.attempts = 0;
            a_remote.exhausted = false;
            a_remote.armorMatches = false;
            a_remote.handsMatch = false;
            a_remote.handsDeferred = false;
        }

        void EnsureResolved(RemoteState& a_remote, const std::string& a_playerID)
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
            // A newer revision does not make a swap that is already waiting less
            // overdue. Fast consecutive changes must not extend the deferral forever.
            const float handDeferralElapsed = a_remote.handDeferralElapsed;
            ResetReconciliation(a_remote);
            a_remote.handDeferralElapsed = handDeferralElapsed;
            if (!a_remote.latest)
            {
                return;
            }

            const auto& equipment = *a_remote.latest;
            // Resolved items take their slots first; fallbacks only fill what is left.
            std::uint32_t occupiedSlots = 0;
            std::vector<std::pair<const EquipmentArmor*, std::string_view>> unresolved;
            for (const auto& entry : equipment.armor)
            {
                const auto resolution = ResolveArmor(entry);
                if (!resolution.form)
                {
                    unresolved.emplace_back(&entry, resolution.dropReason);
                    continue;
                }
                if (std::ranges::find(a_remote.armor, resolution.form) == a_remote.armor.end())
                {
                    a_remote.armor.push_back(resolution.form);
                    occupiedSlots |= resolution.form->GetSlotMask().underlying();
                }
            }
            for (const auto& [entry, reason] : unresolved)
            {
                const auto firstFallback = a_remote.armor.size();
                EquipmentFallback::AppendArmor(*entry, occupiedSlots, a_remote.armor);
                LogUnresolvedEntry(a_playerID, revision, "armor", entry->form, reason,
                    FormatFallback(std::span{ a_remote.armor }.subspan(firstFallback)));
            }
            a_remote.rightHand = ResolveHandEntry(a_playerID, revision, "rightHand", equipment.rightHand);
            a_remote.leftHand = ResolveHandEntry(a_playerID, revision, "leftHand", equipment.leftHand);
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
                    // Ammo carries no type hint. The source's crossbow implies bolts.
                    const bool bolt = equipment.rightHand && equipment.rightHand->kind == EquipmentHandKind::kWeapon
                        && equipment.rightHand->weaponType == static_cast<std::uint8_t>(RE::WEAPON_TYPE::kCrossbow);
                    a_remote.ammo = EquipmentFallback::FindAmmo(bolt);
                    LogUnresolvedEntry(a_playerID, revision, "ammo", equipment.ammo, resolution.dropReason,
                        FormatFallback(a_remote.ammo));
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
            // Spells are not inventory items; they are deselected instead.
            return a_object && (a_object->IsWeapon() || a_object->Is(RE::FormType::Light));
        }

        RE::SpellItem* AsSpell(RE::TESBoundObject* a_object)
        {
            return a_object ? a_object->As<RE::SpellItem>() : nullptr;
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
        static float pollElapsed = kCapturePollInterval;
        static float publishElapsed = kMinimumPublishInterval;
        static std::optional<EquipmentData> published;
        static std::optional<EquipmentData> pending;
        static std::uint64_t revision = 0;
        static bool dirty = false;
        static std::uint64_t publishedConnectionGeneration = 0;

        const float delta = std::isfinite(a_delta) ? (std::max)(a_delta, 0.0F) : 0.0F;
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
            // The server forgets a connection's equipment when it drops.
            if (published)
            {
                logs::debug("Equipment sync republishing after connect: revision={} connectionGeneration={}",
                    static_cast<unsigned long long>(published->revision), static_cast<unsigned long long>(generation));
                Network::PublishLocalEquipment(*published);
                publishElapsed = 0.0F;
            }
        }

        const bool captureDue
            = a_flush || (dirty && quietElapsed >= kCaptureQuietInterval) || pollElapsed >= kCapturePollInterval;
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

        if (!pending || (!a_flush && publishElapsed < kMinimumPublishInterval))
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
            "leftHand={}:{}|{:06X} ammo={}|{:06X} skipped={} flush={}",
            static_cast<unsigned long long>(pending->revision), pending->armor.size(),
            pending->rightHand ? Multiplayer::GetEquipmentHandKindName(pending->rightHand->kind) : "none",
            pending->rightHand ? pending->rightHand->form.plugin : "",
            pending->rightHand ? pending->rightHand->form.localFormID : 0,
            pending->leftHand ? Multiplayer::GetEquipmentHandKindName(pending->leftHand->kind) : "none",
            pending->leftHand ? pending->leftHand->form.plugin : "",
            pending->leftHand ? pending->leftHand->form.localFormID : 0,
            pending->ammo.IsEmpty() ? "<none>" : pending->ammo.plugin, pending->ammo.localFormID, skipped, a_flush);
        Network::PublishLocalEquipment(*pending);
        published = std::move(pending);
        pending.reset();
        publishElapsed = 0.0F;
    }

    void AcceptRemoteEquipment(
        RemoteState& a_remote, const std::string& a_playerID, Multiplayer::PlayerEquipment&& a_equipment)
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

    void SyncEquipment(RemoteState& a_remote, const std::string& a_playerID, RemoteActor& a_remoteActor, float a_delta)
    {
        PROFILE_FUNCTION();
        const float delta = std::isfinite(a_delta) ? (std::max)(a_delta, 0.0F) : 0.0F;
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

        // Before the first mutation pass, check every frame so a new revision or a
        // released hand deferral applies immediately. After a pass, give the engine
        // an interval to apply it; matched states are re-verified at that interval
        // against autonomous engine changes. A deferred pass may already have added
        // the new hand objects, so its release is watched separately; a draw held for
        // the swap would otherwise wait out the interval.
        const bool deferralReleased = a_remote.handsDeferred && !IsHandSwapDeferred(*actor, a_remoteActor);
        const bool immediate = deferralReleased
            || (!(a_remote.armorMatches && a_remote.handsMatch) && !a_remote.exhausted && a_remote.attempts == 0);
        a_remote.checkElapsed += delta;
        if (!immediate && a_remote.checkElapsed < kReconcileCheckInterval)
        {
            return;
        }
        const float sinceLastCheck = a_remote.checkElapsed;
        a_remote.checkElapsed = 0.0F;

        // Measure the live state before mutating anything.
        auto inventory = actor->GetInventory();
        auto* equippedRight = GetEquippedHandObject(*actor, false);
        auto* equippedLeft = GetEquippedHandObject(*actor, true);
        auto* equippedAmmo = GetWornAmmo(*actor);
        const bool handsMatch = equippedRight == a_remote.rightHand.object && IsLeftHandMatched(a_remote, equippedLeft)
            && equippedAmmo == a_remote.ammo;
        const bool handsDeferred = !handsMatch && IsHandSwapDeferred(*actor, a_remoteActor)
            && a_remote.handDeferralElapsed < kMaximumHandDeferral;

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
            // Removing an equipped hand object forces an unequip, so a deferred swap
            // would leave the remote actor sheathing or drawing empty hands until it applies.
            // Keep the stale objects until the swap replaces them.
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

        // Hands only gate draws and attacks, so an unrelated inventory leftover must
        // not hold them.
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
                // Nothing else to do until the weapon transition or attack releases.
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

        // Remove everything that is not synchronized. Removed items are deleted,
        // never moved to another container.
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
            // A placeholder spell in a hand that should be empty has no item to
            // replace it. Deselecting may clear both hands; the equips below
            // restore a hand that still needs it.
            if (auto* spell = AsSpell(equippedRight); spell && !a_remote.rightHand.object)
            {
                actor->DeselectSpell(spell);
                swappedHands = true;
            }
            if (auto* spell = AsSpell(equippedLeft); spell && !a_remote.leftHand.object)
            {
                actor->DeselectSpell(spell);
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
        return AreHandsSettled(a_remote) || a_remote.handsUnsettledElapsed >= kMaximumWeaponDrawWait;
    }
}
