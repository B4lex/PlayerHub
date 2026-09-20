#include "actor/ActorFurniture.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "actor/AnimationFirewall.h"
#include "actor/RemoteActor.h"
#include "runtime/GameThread.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace ActorFurniture
{
    namespace
    {
        using ActorConstants::kMaximumFrameTime;

        constexpr float kFurnitureRetryInterval = 1.0F;
        constexpr float kFurnitureEntryFallbackSeconds = 10.0F;
        constexpr float kFurnitureExitFallbackSeconds = 10.0F;
        // A started entry that never records occupancy is a refusal Skyrim reported
        // as success; it is retried long before the budget for an entry that was
        // seated and then dropped.
        constexpr float kFurnitureEntryOccupancySeconds = 1.5F;
        // Occupancy on a reference that was never requested comes from the actor's
        // own package selection. Cancelling it is not always accepted on the first
        // pass, so bound the attempts before the interaction is abandoned.
        constexpr float kForeignFurnitureCancelSeconds = 3.0F;
        // A remote actor created in the pass the local cell finished loading has not
        // run one update with its do-nothing package, its equipment, or a settled AI
        // process. Native entry started there is accepted and then torn down by the
        // engine, which leaves the marker reserved and the actor off its marker.
        constexpr float kFurnitureEntrySpawnSettleSeconds = 1.5F;
        constexpr float kFurnitureEntryAlignmentDistance = 24.0F;
        // Native entry aligns the actor to the chosen entry point itself; this only
        // bounds how far that alignment may glide the remote actor.
        constexpr float kFurnitureEntryMarkerDistance = 96.0F;

        void SetFurnitureAnimationOwnership(RemoteActor& a_remote, bool a_owned)
        {
            a_remote.furniture.furnitureAnimationOwned = a_owned;
            ActorRegistry::SetFurnitureAnimationOwned(a_remote, a_owned);
        }

        RE::SIT_SLEEP_STATE GetActorSitSleepState(const RE::Actor& a_actor)
        {
            // Actor's ActorState base moved on AE. Calling ActorState members directly
            // on RE::Actor uses the SE base offset; AsActorState relocates it.
            const auto* actorState = a_actor.AsActorState();
            return actorState ? actorState->GetSitSleepState() : RE::SIT_SLEEP_STATE::kNormal;
        }

        bool SetActorSitSleepState(RE::Actor& a_actor, RE::SIT_SLEEP_STATE a_state)
        {
            auto* actorState = a_actor.AsActorState();
            return actorState && actorState->DoSetSitSleepState(a_state);
        }

        constexpr bool IsFurnitureEntryComplete(RE::SIT_SLEEP_STATE a_state)
        {
            return a_state == RE::SIT_SLEEP_STATE::kIsSitting || a_state == RE::SIT_SLEEP_STATE::kWantToStand
                || a_state == RE::SIT_SLEEP_STATE::kIsSleeping || a_state == RE::SIT_SLEEP_STATE::kWantToWake;
        }

        RE::FormID GetOccupiedFurnitureRefID(RE::Actor& a_actor)
        {
            auto furniture = a_actor.GetOccupiedFurniture().get();
            return furniture ? furniture->GetFormID() : 0;
        }

        // Marker reservations live on the furniture reference, keyed by actor handle
        // with an expiration. Skyrim's entry-point selection consults them, so a
        // reservation that outlives its actor blocks the furniture until it expires.
        std::uint32_t ReleaseFurnitureMarkerReservations(RE::Actor& a_actor, RE::TESObjectREFR& a_furniture)
        {
            auto* usedMarkers = a_furniture.extraList.GetByType<RE::ExtraUsedMarkers>();
            if (!usedMarkers)
            {
                return 0;
            }

            const auto actorHandle = a_actor.GetHandle();
            std::uint32_t releasedCount = 0;
            for (auto marker = usedMarkers->usedMarkers.begin(); marker != usedMarkers->usedMarkers.end();)
            {
                if (marker->actorinMarker == actorHandle)
                {
                    marker = usedMarkers->usedMarkers.erase(marker);
                    ++releasedCount;
                }
                else
                {
                    ++marker;
                }
            }
            return releasedCount;
        }

        // Logged when the marker probe and the native entry disagree: the probe
        // reports a free marker while entry-point selection refuses every one. The
        // reservations are the only state that can explain that disagreement.
        void LogFurnitureMarkerReservations(std::uint32_t a_actorHandle, const RE::TESObjectREFR& a_furniture)
        {
            const auto* usedMarkers = a_furniture.extraList.GetByType<RE::ExtraUsedMarkers>();
            if (!usedMarkers || usedMarkers->usedMarkers.empty())
            {
                logs::warn("Animation sync furniture marker reservations: actor={:08X} furniture={:08X} reservations=0",
                    a_actorHandle, a_furniture.GetFormID());
                return;
            }

            for (const auto& marker : usedMarkers->usedMarkers)
            {
                const auto reservedBy = marker.actorinMarker.get();
                logs::warn(
                    "Animation sync furniture marker reservation: actor={:08X} furniture={:08X} marker={} "
                    "reservedBy={:08X} reservedByForm={:08X} reservedByResolved={}",
                    a_actorHandle, a_furniture.GetFormID(), marker.markerID, marker.actorinMarker.native_handle(),
                    reservedBy ? reservedBy->GetFormID() : 0, reservedBy != nullptr);
            }
        }

        bool IsSameFurnitureCoordinateSpace(const RE::TESObjectREFR& a_actor, const RE::TESObjectREFR& a_furniture)
        {
            const auto* actorWorldspace = a_actor.GetWorldspace();
            const auto* furnitureWorldspace = a_furniture.GetWorldspace();
            if (actorWorldspace || furnitureWorldspace)
            {
                return actorWorldspace && actorWorldspace == furnitureWorldspace;
            }
            return a_actor.GetParentCell() && a_actor.GetParentCell() == a_furniture.GetParentCell();
        }

        // The furniture engine calls below are Address Library IDs verified only
        // against 1.7.104. Other runtimes never find a marker and never start entry.

        std::int32_t FindAvailableFurnitureMarker(RE::Actor& a_actor, RE::TESObjectREFR& a_furniture)
        {
            if (!REL::Module::IsAE())
            {
                return -1;
            }
            using func_t = std::int32_t(RE::TESObjectREFR*, RE::Actor*, bool);
            static REL::Relocation<func_t> func{ REL::ID(20162) };
            return func(std::addressof(a_furniture), std::addressof(a_actor), true);
        }

        std::optional<RE::NiPoint3> GetFurnitureMarkerPosition(
            const RE::TESObjectREFR& a_furniture, std::int32_t a_marker)
        {
            auto* root = a_furniture.Get3D2();
            const auto* markerNode = root ? RE::BSFurnitureMarkerNode::FindBSFurnitureMarkerNode(root) : nullptr;
            if (!markerNode || a_marker < 0 || static_cast<std::uint32_t>(a_marker) >= markerNode->markers.size())
            {
                return std::nullopt;
            }
            return root->world * markerNode->markers[a_marker].offset;
        }

        bool UseNativeFurniture(RE::Actor& a_actor, RE::TESObjectREFR& a_furniture)
        {
            // TESFurniture::Activate rejects every activator except the player, so use
            // the entry NPC Sit procedures use (ID 37330). It records the occupied
            // reference, lets ID 39923 choose an entry point (filling furnitureIdle,
            // furniturePathPoint and the entry angle) and reserves that marker.
            // UpdateNativeSitSleepState then plays the chosen entry idle. The quick
            // entry (ID 37324) is not used: it selects the Idle*EnterInstant records
            // and skips the transition.
            using UseFurniture = bool(RE::Actor*, RE::TESObjectREFR*);
            static REL::Relocation<UseFurniture> useFurniture{ REL::ID(37330) };
            return useFurniture(std::addressof(a_actor), std::addressof(a_furniture));
        }

        bool UseNativeFurnitureInstant(RE::Actor& a_actor, RE::TESObjectREFR& a_furniture)
        {
            // The quick entry (ID 37324) records the same occupancy and chooses the
            // same marker, but selects the Idle*EnterInstant records, so the actor
            // adopts the seated pose without the entry transition. Only correct when
            // the source's entry finished before this actor existed; the caller
            // falls back to the transition when this refuses.
            using UseFurnitureInstant = bool(RE::Actor*, RE::TESObjectREFR*);
            static REL::Relocation<UseFurnitureInstant> useFurnitureInstant{ REL::ID(37324) };
            return useFurnitureInstant(std::addressof(a_actor), std::addressof(a_furniture));
        }

        bool IsFurnitureEntryPointChosen(const RE::AIProcess& a_process)
        {
            // ID 39923 leaves the marker unset when it only requested a sheathe.
            return a_process.middleHigh
                && a_process.middleHigh->currentFurnitureMarkerID != static_cast<std::uint32_t>(-1);
        }

        bool StartNativeFurnitureEntry(
            RemoteActor& a_remote, RE::Actor& a_actor, RE::TESObjectREFR& a_furniture, float a_markerDistance)
        {
            auto* process = a_actor.GetActorRuntimeData().currentProcess;
            if (!REL::Module::IsAE() || !process || !process->middleHigh)
            {
                return false;
            }

            (void)SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
            bool started = false;
            bool instantEntry = false;
            if (a_remote.furniture.resumeWithoutEntryIdle)
            {
                started = UseNativeFurnitureInstant(a_actor, a_furniture);
                instantEntry = started;
                if (!started)
                {
                    // Occupancy is recorded before the entry point is chosen and is
                    // left behind when none is available. Clear it before the
                    // transition entry records its own.
                    process->ClearFurniture();
                }
            }
            if (!started)
            {
                started = UseNativeFurniture(a_actor, a_furniture);
            }
            const auto* entryIdle = process->middleHigh->furnitureIdle;
            if (!started)
            {
                process->ClearFurniture();
            }
            const auto* actorState = a_actor.AsActorState();
            logs::info(
                "Animation sync started furniture entry: actor={:08X} furniture={:08X} started={} instant={} "
                "marker={} markerDistance={:.2f} entryIdle={:08X} occupied={:08X} weaponState={} inCombat={}",
                a_remote.handle.native_handle(), a_furniture.GetFormID(), started, instantEntry,
                process->middleHigh->currentFurnitureMarkerID, a_markerDistance,
                started && entryIdle ? entryIdle->GetFormID() : 0, GetOccupiedFurnitureRefID(a_actor),
                actorState ? static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(actorState->GetWeaponState()) : 0,
                a_actor.IsInCombat());
            if (!started)
            {
                // The caller only reaches this after the marker probe reported a free
                // marker, so the reservations explain which owner refused the entry.
                LogFurnitureMarkerReservations(a_remote.handle.native_handle(), a_furniture);
            }
            return started;
        }

        void UpdateNativeSitSleepState(RE::Actor& a_actor)
        {
            // Actor::UpdateSitSleepState (ID 37332 -> AIProcess ID 39861) advances
            // Normal -> WantToSit -> WaitingForSitAnim and WantToStand -> Normal.
            // Skyrim calls it from package procedure branches that the remote actor's
            // do-nothing package never reaches.
            if (!REL::Module::IsAE())
            {
                return;
            }
            using func_t = void(RE::Actor*);
            static REL::Relocation<func_t> func{ REL::ID(37332) };
            func(std::addressof(a_actor));
        }

        void ResetFurnitureRequest(RemoteActor& a_remote)
        {
            a_remote.furniture.requestedFurnitureRefID = 0;
            a_remote.furniture.furnitureRetryRemaining = 0.0F;
            a_remote.furniture.furnitureTransitionElapsed = 0.0F;
            a_remote.furniture.occupancyObserved = false;
        }

        bool AdvanceDeferredFurnitureEntry(RE::Actor& a_actor, RemoteActor& a_remote, RE::AIProcess& a_process,
            RE::SIT_SLEEP_STATE a_sitSleepState, float a_deltaTime)
        {
            // With a weapon drawn, ID 39923 requests a sheathe and returns without an
            // entry point, so no entry idle can play. Skyrim's Sit procedure (ID 39648)
            // selects again on later updates while the marker is unset; do the same
            // once the sheathe has finished.
            a_remote.furniture.furnitureTransitionElapsed += a_deltaTime;
            auto furniture = a_actor.GetOccupiedFurniture().get();
            const auto* actorState = a_actor.AsActorState();
            if (furniture && actorState && actorState->GetWeaponState() == RE::WEAPON_STATE::kSheathed
                && a_remote.furniture.furnitureRetryRemaining <= 0.0F)
            {
                const bool started = UseNativeFurniture(a_actor, *furniture);
                const bool entryPointChosen = IsFurnitureEntryPointChosen(a_process);
                if (!entryPointChosen)
                {
                    a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
                }
                const auto* entryIdle = a_process.middleHigh ? a_process.middleHigh->furnitureIdle : nullptr;
                logs::info(
                    "Animation sync resumed deferred furniture entry: actor={:08X} furniture={:08X} started={} "
                    "marker={} entryIdle={:08X} sitSleepState={} elapsed={:.3f}",
                    a_remote.handle.native_handle(), furniture->GetFormID(), started,
                    a_process.middleHigh ? a_process.middleHigh->currentFurnitureMarkerID
                                         : static_cast<std::uint32_t>(-1),
                    entryIdle ? entryIdle->GetFormID() : 0,
                    static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(a_sitSleepState),
                    a_remote.furniture.furnitureTransitionElapsed);
                if (entryPointChosen)
                {
                    a_remote.furniture.furnitureRetryRemaining = 0.0F;
                    a_remote.furniture.furnitureTransitionElapsed = 0.0F;
                    return true;
                }
            }
            if (a_remote.furniture.furnitureTransitionElapsed < kFurnitureEntryFallbackSeconds)
            {
                return true;
            }

            logs::warn(
                "Animation sync abandoned deferred furniture entry for retry: actor={:08X} furniture={:08X} "
                "sitSleepState={} weaponState={} elapsed={:.3f}",
                a_remote.handle.native_handle(), a_remote.furniture.targetFurnitureRefID,
                static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(a_sitSleepState),
                actorState ? static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(actorState->GetWeaponState()) : 0,
                a_remote.furniture.furnitureTransitionElapsed);
            a_process.ClearFurniture();
            (void)SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
            a_actor.InitiateDoNothingPackage();
            ResetFurnitureRequest(a_remote);
            a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
            SetFurnitureAnimationOwnership(a_remote, false);
            return false;
        }
    }

    void SetFurnitureState(RemoteActor& a_remote, RE::FormID a_furnitureRefID)
    {
        if (!GameThread::Require("SetFurnitureState") || a_remote.furniture.targetFurnitureRefID == a_furnitureRefID)
        {
            return;
        }

        const auto previousFurnitureRefID = a_remote.furniture.targetFurnitureRefID;
        a_remote.furniture.targetFurnitureRefID = a_furnitureRefID;
        a_remote.furniture.furnitureRetryRemaining = 0.0F;
        // Occupancy of the previous target says nothing about the new one.
        a_remote.furniture.occupancyObserved = false;
        // Nothing the peer did while this actor was settling from its spawn was
        // visible locally: the pass it was created in is the pass the cell finished
        // loading. Resume such an interaction in its seated pose rather than
        // replaying an entry the source already completed.
        a_remote.furniture.resumeWithoutEntryIdle
            = a_furnitureRefID != 0 && a_remote.readyElapsed < kFurnitureEntrySpawnSettleSeconds;
        if (a_remote.furniture.requestedFurnitureRefID == 0)
        {
            // An in-progress exit keeps its elapsed time so that rapid target changes
            // cannot postpone the forced-exit fallback indefinitely.
            a_remote.furniture.furnitureTransitionElapsed = 0.0F;
        }
        logs::info(
            "Animation sync updated furniture request: actor={:08X} previous={:08X} desired={:08X} resume={} "
            "readyElapsed={:.3f}",
            a_remote.handle.native_handle(), previousFurnitureRefID, a_furnitureRefID,
            a_remote.furniture.resumeWithoutEntryIdle, a_remote.readyElapsed);
    }

    RE::FormID GetTargetFurniture(const RemoteActor& a_remote)
    {
        return a_remote.furniture.targetFurnitureRefID;
    }

    bool IsFurnitureInteractionPlaying(const RemoteActor& a_remote)
    {
        return a_remote.furniture.requestedFurnitureRefID != 0 || a_remote.furniture.furnitureAnimationOwned;
    }

    void ReleaseFurniture(RE::Actor& a_actor, RemoteActor& a_remote)
    {
        if (!GameThread::Require("ReleaseFurniture"))
        {
            return;
        }

        auto furniture = a_actor.GetOccupiedFurniture().get();
        const std::uint32_t releasedMarkerCount
            = furniture ? ReleaseFurnitureMarkerReservations(a_actor, *furniture) : 0;
        if (auto* process = a_actor.GetActorRuntimeData().currentProcess)
        {
            process->ClearFurniture();
        }
        (void)SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
        SetFurnitureAnimationOwnership(a_remote, false);
        ResetFurnitureRequest(a_remote);
        a_remote.furniture.targetFurnitureRefID = 0;
        a_remote.furniture.loggedSitSleepState = RE::SIT_SLEEP_STATE::kNormal;
        a_remote.furniture.entrySettleLogged = false;
        a_remote.furniture.resumeWithoutEntryIdle = false;
        if (furniture || releasedMarkerCount != 0)
        {
            logs::info(
                "Animation sync released furniture before retiring actor: actor={:08X} furniture={:08X} "
                "releasedMarkers={}",
                a_remote.handle.native_handle(), furniture ? furniture->GetFormID() : 0, releasedMarkerCount);
        }
    }

    bool AdvanceFurnitureInteraction(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
    {
        const auto targetFurnitureRefID = a_remote.furniture.targetFurnitureRefID;
        const auto occupiedFurnitureRefID = GetOccupiedFurnitureRefID(a_actor);
        const float deltaTime = std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        a_remote.furniture.furnitureRetryRemaining -= deltaTime;
        const auto sitSleepState = GetActorSitSleepState(a_actor);
        if (sitSleepState != a_remote.furniture.loggedSitSleepState)
        {
            logs::info(
                "Animation sync actor sit/sleep state changed: actor={:08X} previous={} current={} occupied={:08X} "
                "target={:08X}",
                a_remote.handle.native_handle(),
                static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(a_remote.furniture.loggedSitSleepState),
                static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState), occupiedFurnitureRefID,
                targetFurnitureRefID);
            a_remote.furniture.loggedSitSleepState = sitSleepState;
        }
        if (occupiedFurnitureRefID != 0)
        {
            UpdateNativeSitSleepState(a_actor);
        }

        // Entry, use, and the furniture graph are driven natively once the target
        // is occupied.
        if (targetFurnitureRefID != 0 && occupiedFurnitureRefID == targetFurnitureRefID)
        {
            SetFurnitureAnimationOwnership(a_remote, true);
            a_remote.furniture.requestedFurnitureRefID = targetFurnitureRefID;
            auto* process = a_actor.GetActorRuntimeData().currentProcess;
            if (!a_remote.furniture.occupancyObserved)
            {
                // The pass after the native call is the first one that can see what
                // it recorded. Entries that report success without ever reaching
                // here are refusals, not slow entries.
                a_remote.furniture.occupancyObserved = true;
                logs::info(
                    "Animation sync confirmed furniture occupancy: actor={:08X} furniture={:08X} marker={} "
                    "sitSleepState={} elapsed={:.3f}",
                    a_remote.handle.native_handle(), targetFurnitureRefID,
                    process && process->middleHigh ? process->middleHigh->currentFurnitureMarkerID
                                                   : static_cast<std::uint32_t>(-1),
                    static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState),
                    a_remote.furniture.furnitureTransitionElapsed);
            }
            if (process && !IsFurnitureEntryPointChosen(*process) && !IsFurnitureEntryComplete(sitSleepState))
            {
                return AdvanceDeferredFurnitureEntry(a_actor, a_remote, *process, sitSleepState, deltaTime);
            }
            a_remote.furniture.furnitureRetryRemaining = 0.0F;
            a_remote.furniture.furnitureTransitionElapsed = 0.0F;
            return true;
        }

        if (targetFurnitureRefID != 0 && a_remote.furniture.requestedFurnitureRefID == targetFurnitureRefID
            && occupiedFurnitureRefID == 0)
        {
            // Skyrim dropped a started entry, or never recorded the occupancy the
            // native call reported. Hold ownership for a bounded time before
            // releasing the request for a fresh attempt.
            SetFurnitureAnimationOwnership(a_remote, true);
            a_remote.furniture.furnitureTransitionElapsed += deltaTime;
            const bool occupancyObserved = a_remote.furniture.occupancyObserved;
            const float entryFallbackSeconds
                = occupancyObserved ? kFurnitureEntryFallbackSeconds : kFurnitureEntryOccupancySeconds;
            if (a_remote.furniture.furnitureTransitionElapsed < entryFallbackSeconds)
            {
                return true;
            }

            logs::warn(
                "Animation sync abandoned stuck furniture entry for retry: actor={:08X} furniture={:08X} "
                "sitSleepState={} occupancyObserved={} elapsed={:.3f}",
                a_remote.handle.native_handle(), targetFurnitureRefID,
                static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState), occupancyObserved,
                a_remote.furniture.furnitureTransitionElapsed);
            a_actor.InitiateDoNothingPackage();
            ResetFurnitureRequest(a_remote);
            SetFurnitureAnimationOwnership(a_remote, false);
            return false;
        }

        const bool mustExitCurrentFurniture
            = occupiedFurnitureRefID != 0 || a_remote.furniture.requestedFurnitureRefID != 0;
        const bool furnitureAnimationOwned = a_remote.furniture.furnitureAnimationOwned;
        if (mustExitCurrentFurniture || (targetFurnitureRefID == 0 && furnitureAnimationOwned))
        {
            SetFurnitureAnimationOwnership(a_remote, true);
            if (occupiedFurnitureRefID == 0 && GetActorSitSleepState(a_actor) == RE::SIT_SLEEP_STATE::kNormal)
            {
                const auto exitedFurnitureRefID = a_remote.furniture.requestedFurnitureRefID;
                a_actor.InitiateDoNothingPackage();
                ResetFurnitureRequest(a_remote);
                SetFurnitureAnimationOwnership(a_remote, false);
                if (a_remote.animation)
                {
                    // Furniture graphs do not consistently return to the locomotion
                    // variables which preceded entry. Reconstruct the current checkpoint.
                    ActorAnimation::InvalidateAnimationDispatch(*a_remote.animation);
                }
                const auto* actorState = a_actor.AsActorState();
                if (a_remote.animation->targetAnimationState.weaponDrawn && actorState
                    && actorState->GetWeaponState() == RE::WEAPON_STATE::kSheathed)
                {
                    // Sitting sheathes the weapon, but checkpoints are not staged while
                    // furniture owns the graph, so the request still holds the pre-entry
                    // drawn state. Dispatching it now would draw on the reconstruction
                    // update and then sheathe once the fresh checkpoint is staged.
                    auto sheathedState = a_remote.animation->targetAnimationState;
                    sheathedState.weaponDrawn = false;
                    ActorAnimation::SetAnimationState(a_remote, sheathedState);
                }
                logs::info(
                    "Animation sync completed furniture exit: actor={:08X} furniture={:08X} nextFurniture={:08X}",
                    a_remote.handle.native_handle(), exitedFurnitureRefID, targetFurnitureRefID);
                return false;
            }

            if (occupiedFurnitureRefID != 0 && !IsFurnitureEntryComplete(sitSleepState))
            {
                // The entry never reached a seated state, so there is no native exit
                // to request. Release the reservation; the next update completes exit.
                // Occupancy on a reference that was never requested comes from the
                // actor's own package selection, and clearing it is not always
                // accepted on the first pass, so this is bounded rather than retried
                // every pass.
                a_remote.furniture.furnitureTransitionElapsed += deltaTime;
                auto occupiedFurniture = a_actor.GetOccupiedFurniture().get();
                if (occupiedFurniture)
                {
                    (void)ReleaseFurnitureMarkerReservations(a_actor, *occupiedFurniture);
                }
                if (auto* process = a_actor.GetActorRuntimeData().currentProcess)
                {
                    process->ClearFurniture();
                }
                (void)SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
                if (a_remote.furniture.furnitureRetryRemaining <= 0.0F)
                {
                    a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
                    logs::info(
                        "Animation sync cancelled incomplete furniture entry: actor={:08X} occupied={:08X} "
                        "requested={:08X} target={:08X} sitSleepState={} elapsed={:.3f}",
                        a_remote.handle.native_handle(), occupiedFurnitureRefID,
                        a_remote.furniture.requestedFurnitureRefID, targetFurnitureRefID,
                        static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState),
                        a_remote.furniture.furnitureTransitionElapsed);
                }
                if (a_remote.furniture.furnitureTransitionElapsed < kForeignFurnitureCancelSeconds)
                {
                    return true;
                }

                logs::warn(
                    "Animation sync abandoned uncancellable furniture occupancy: actor={:08X} occupied={:08X} "
                    "target={:08X} elapsed={:.3f}",
                    a_remote.handle.native_handle(), occupiedFurnitureRefID, targetFurnitureRefID,
                    a_remote.furniture.furnitureTransitionElapsed);
                a_actor.InitiateDoNothingPackage();
                ResetFurnitureRequest(a_remote);
                SetFurnitureAnimationOwnership(a_remote, false);
                a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
                return false;
            }

            a_remote.furniture.furnitureTransitionElapsed += deltaTime;
            if (a_remote.furniture.furnitureRetryRemaining <= 0.0F)
            {
                const bool sleeping = sitSleepState == RE::SIT_SLEEP_STATE::kIsSleeping
                    || sitSleepState == RE::SIT_SLEEP_STATE::kWantToWake;
                a_actor.InitiateGetUpPackage();
                const bool stateAccepted = SetActorSitSleepState(
                    a_actor, sleeping ? RE::SIT_SLEEP_STATE::kWantToWake : RE::SIT_SLEEP_STATE::kWantToStand);
                static const RE::BSFixedString furnitureExitEvent{ "IdleFurnitureExit" };
                const bool eventAccepted = AnimationFirewall::NotifyNetworkAnimationGraph(a_actor, furnitureExitEvent);
                a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
                logs::info(
                    "Animation sync requested furniture exit: actor={:08X} occupied={:08X} target={:08X} "
                    "sitSleepState={} stateAccepted={} eventAccepted={}",
                    a_remote.handle.native_handle(), occupiedFurnitureRefID, targetFurnitureRefID,
                    static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState), stateAccepted,
                    eventAccepted);
            }

            if (a_remote.furniture.furnitureTransitionElapsed >= kFurnitureExitFallbackSeconds)
            {
                if (auto* process = a_actor.GetActorRuntimeData().currentProcess)
                {
                    process->ClearFurniture();
                }
                (void)SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
                static const RE::BSFixedString defaultStateEvent{ "IdleForceDefaultState" };
                (void)AnimationFirewall::NotifyNetworkAnimationGraph(a_actor, defaultStateEvent);
                a_actor.InitiateDoNothingPackage();
                a_remote.furniture.furnitureTransitionElapsed = 0.0F;
                logs::warn("Animation sync forced stuck furniture exit: actor={:08X} occupied={:08X} target={:08X}",
                    a_remote.handle.native_handle(), occupiedFurnitureRefID, targetFurnitureRefID);
            }
            return true;
        }
        if (targetFurnitureRefID == 0)
        {
            return false;
        }

        // The target is a FormID resolved locally; Skyrim chooses the entry point,
        // idle, and marker for the remote actor.
        auto* furniture = RE::TESForm::LookupByID<RE::TESObjectREFR>(targetFurnitureRefID);
        const auto* furnitureBase = furniture ? furniture->GetBaseObject() : nullptr;
        const bool isFurniture = furnitureBase && furnitureBase->As<RE::TESFurniture>();
        const bool loaded3D = furniture && furniture->Is3DLoaded();
        const bool sameCoordinateSpace = furniture && IsSameFurnitureCoordinateSpace(a_actor, *furniture);
        if (!furniture || !isFurniture || !loaded3D || !sameCoordinateSpace)
        {
            if (a_remote.furniture.furnitureRetryRemaining <= 0.0F)
            {
                a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
                logs::warn(
                    "Animation sync waiting for furniture reference: actor={:08X} furniture={:08X} resolved={} "
                    "furnitureBase={} loaded3D={} sameSpace={}",
                    a_remote.handle.native_handle(), targetFurnitureRefID, furniture != nullptr, isFurniture, loaded3D,
                    sameCoordinateSpace);
            }
            // The latest transform remains authoritative while the reference is
            // unavailable. Do not strand the remote actor at its pre-interaction position.
            return false;
        }
        if (a_remote.ready && a_remote.readyElapsed < kFurnitureEntrySpawnSettleSeconds)
        {
            if (!a_remote.furniture.entrySettleLogged)
            {
                a_remote.furniture.entrySettleLogged = true;
                logs::info(
                    "Animation sync deferring furniture entry until the spawn settles: actor={:08X} furniture={:08X} "
                    "readyElapsed={:.3f}",
                    a_remote.handle.native_handle(), targetFurnitureRefID, a_remote.readyElapsed);
            }
            // The transform stays authoritative meanwhile, so the actor follows the
            // source to the furniture instead of waiting beside its spawn point.
            return false;
        }
        if (!a_remote.ready || !ActorAnimation::IsAnimationStateSettled(a_remote)
            || ActorAnimation::IsFurnitureEntryBlocked(a_remote) || a_remote.furniture.furnitureRetryRemaining > 0.0F)
        {
            return false;
        }

        // The source reports occupancy when it activates, then travels to the entry
        // point before its entry idle moves it onto the marker. Waiting for the
        // source to settle on the marker delays the remote actor by the whole entry idle.
        // Start once the remote actor follows the source close to the furniture; the
        // native entry aligns it to the chosen entry point.
        const auto actorPosition = a_actor.GetPosition();
        if ((a_remote.movement.targetPosition - actorPosition).Length() > kFurnitureEntryAlignmentDistance)
        {
            return false;
        }
        const auto marker = FindAvailableFurnitureMarker(a_actor, *furniture);
        if (marker < 0)
        {
            a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
            logs::warn("Animation sync found no available furniture marker: actor={:08X} furniture={:08X}",
                a_remote.handle.native_handle(), targetFurnitureRefID);
            return false;
        }
        const auto markerPosition = GetFurnitureMarkerPosition(*furniture, marker).value_or(furniture->GetPosition());
        const float markerDistance = std::hypot(markerPosition.x - actorPosition.x, markerPosition.y - actorPosition.y);
        if (markerDistance > kFurnitureEntryMarkerDistance)
        {
            return false;
        }

        // Remove residual interpolation error before Skyrim aligns the actor.
        a_actor.SetPosition(a_remote.movement.targetPosition, true);
        if (std::isfinite(a_remote.movement.targetHeading))
        {
            a_actor.SetHeading(a_remote.movement.targetHeading);
        }
        SetFurnitureAnimationOwnership(a_remote, true);
        a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
        if (!StartNativeFurnitureEntry(a_remote, a_actor, *furniture, markerDistance))
        {
            // Do not freeze translation or leave the graph open to local events.
            SetFurnitureAnimationOwnership(a_remote, false);
            return false;
        }
        a_remote.furniture.requestedFurnitureRefID = targetFurnitureRefID;
        a_remote.furniture.furnitureTransitionElapsed = 0.0F;
        if (auto* process = a_actor.GetActorRuntimeData().currentProcess;
            process && !IsFurnitureEntryPointChosen(*process))
        {
            // Entry is waiting for a sheathe; resume as soon as it completes.
            a_remote.furniture.furnitureRetryRemaining = 0.0F;
        }
        return true;
    }
}
