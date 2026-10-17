#include "actor/ActorFurniture.h"
#include "actor/ActorRegistry.h"
#include "actor/AnimationFirewall.h"
#include "actor/RemoteActor.h"
#include "actor/SurvivalPolicy.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace ActorFurniture
{
    namespace
    {

        constexpr float kFurnitureRetryIntervalSeconds = 1.0F;
        constexpr float kFurnitureEntryFallbackSeconds = 10.0F;
        constexpr float kFurnitureExitFallbackSeconds = 10.0F;
        // The engine releases the furniture when a native exit clip ends: about 1.1
        // seconds for a chair and 3.3 for a table in vanilla.
        constexpr float kNativeExitReleaseSeconds = 5.0F;
        // Vanilla bed and bedroll wake clips run 6.8 to 8.3 seconds.
        constexpr float kNativeWakeReleaseSeconds = 12.0F;
        // A table's exit releases the furniture about 0.2 seconds before its clip
        // stops driving the actor.
        constexpr float kNativeExitClipTailSeconds = 2.0F;
        constexpr float kExitSettleCutDistance = 32.0F;
        // Skyrim can report a started entry as success without ever recording the occupancy.
        constexpr float kFurnitureEntryOccupancySeconds = 1.5F;
        // Occupancy on a reference that was never requested comes from the actor's
        // own package selection. Cancelling it is not always accepted on the first
        // pass.
        constexpr float kForeignFurnitureCancelSeconds = 3.0F;
        // A remote actor created in the pass the local cell finished loading has not
        // run one update with its do-nothing package, its equipment, or a settled AI
        // process. Native entry started there is accepted and then torn down by the
        // engine, which leaves the marker reserved and the actor off its marker.
        constexpr float kFurnitureEntrySpawnSettleSeconds = 1.5F;
        constexpr float kFurnitureEntryAlignmentDistance = 24.0F;
        // Native entry aligns the actor to the chosen entry point itself.
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

        // The entry idle once ID 39923 has chosen one.
        RE::FormID GetFurnitureIdleFormID(RE::Actor& a_actor)
        {
            const auto* process = a_actor.GetActorRuntimeData().currentProcess;
            const auto* furnitureIdle = process && process->middleHigh ? process->middleHigh->furnitureIdle : nullptr;
            return furnitureIdle ? furnitureIdle->GetFormID() : 0;
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

        void LogFurnitureMarkerReservations(std::uint32_t a_actorHandle, const RE::TESObjectREFR& a_furniture)
        {
            const auto* usedMarkers = a_furniture.extraList.GetByType<RE::ExtraUsedMarkers>();
            if (!usedMarkers || usedMarkers->usedMarkers.empty())
            {
                logs::debug("Animation sync furniture marker reservations: actor={:08X} furniture={:08X} reservations=0",
                    a_actorHandle, a_furniture.GetFormID());
                return;
            }

            for (const auto& marker : usedMarkers->usedMarkers)
            {
                const auto reservedBy = marker.actorinMarker.get();
                logs::debug(
                    "Animation sync furniture marker reservation: actor={:08X} furniture={:08X} marker={} "
                    "reservedBy={:08X} reservedByForm={:08X} reservedByResolved={}",
                    a_actorHandle, a_furniture.GetFormID(), marker.markerID, marker.actorinMarker.native_handle(),
                    reservedBy ? reservedBy->GetFormID() : 0, reservedBy != nullptr);
            }
        }

        bool IsReservedByOtherActor(RE::Actor& a_actor, const RE::TESObjectREFR& a_furniture)
        {
            const auto* usedMarkers = a_furniture.extraList.GetByType<RE::ExtraUsedMarkers>();
            if (!usedMarkers)
            {
                return false;
            }
            const auto actorHandle = a_actor.GetHandle();
            return std::ranges::any_of(usedMarkers->usedMarkers, [&](const auto& a_marker) {
                if (a_marker.actorinMarker == actorHandle)
                {
                    return false;
                }
                const auto holder = a_marker.actorinMarker.get();
                return holder && !holder->IsDisabled();
            });
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

        // The comments below name 1.7.104 IDs. Each SE ID was matched against the
        // 1.5.97 disassembly by its code and the functions it calls.

        std::int32_t FindAvailableFurnitureMarker(RE::Actor& a_actor, RE::TESObjectREFR& a_furniture)
        {
            using func_t = std::int32_t(RE::TESObjectREFR*, RE::Actor*, bool);
            static REL::Relocation<func_t> func{ RELOCATION_ID(19767, 20162) };
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

        struct FurnitureMarkerDistance
        {
            std::int32_t marker{ -1 };
            float distance{ std::numeric_limits<float>::infinity() };
        };

        FurnitureMarkerDistance FindNearestFurnitureMarker(
            const RE::TESObjectREFR& a_furniture, const RE::NiPoint3& a_position)
        {
            auto* root = a_furniture.Get3D2();
            const auto* markerNode = root ? RE::BSFurnitureMarkerNode::FindBSFurnitureMarkerNode(root) : nullptr;
            FurnitureMarkerDistance nearest;
            if (!markerNode || markerNode->markers.empty())
            {
                const auto furniturePosition = a_furniture.GetPosition();
                nearest.distance = std::hypot(furniturePosition.x - a_position.x, furniturePosition.y - a_position.y);
                return nearest;
            }

            for (std::uint32_t marker = 0; marker < markerNode->markers.size(); ++marker)
            {
                const auto markerPosition = root->world * markerNode->markers[marker].offset;
                const float distance = std::hypot(markerPosition.x - a_position.x, markerPosition.y - a_position.y);
                if (distance < nearest.distance)
                {
                    nearest
                        = FurnitureMarkerDistance{ .marker = static_cast<std::int32_t>(marker), .distance = distance };
                }
            }
            return nearest;
        }

        bool EnterEntryGate(RemoteActor& a_remote, EntryGate a_gate)
        {
            if (a_remote.furniture.loggedEntryGate == a_gate)
            {
                return false;
            }
            a_remote.furniture.loggedEntryGate = a_gate;
            return true;
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
            static REL::Relocation<UseFurniture> useFurniture{ RELOCATION_ID(36340, 37330) };
            return useFurniture(std::addressof(a_actor), std::addressof(a_furniture));
        }

        bool IsFurnitureEntryPointChosen(const RE::AIProcess& a_process)
        {
            // ID 39923 leaves the marker unset when it only requested a sheathe.
            return a_process.middleHigh
                && a_process.middleHigh->currentFurnitureMarkerID != static_cast<std::uint32_t>(-1);
        }

        bool UseNativeFurnitureInstant(RE::Actor& a_actor, RE::TESObjectREFR& a_furniture, RE::AIProcess& a_process)
        {
            // Mirrors how Skyrim reseats an actor whose 3D loads in furniture (ID
            // 40438): record the occupancy (ID 39955), choose an entry point (ID
            // 39923), and pass its marker to the quick entry (ID 37324). The quick
            // entry reserves that marker, plays the Idle*EnterInstant record, sets
            // IsSitting or IsSleeping, and reports whether it could snap the actor
            // onto the marker.
            using SetOccupiedFurniture = void(RE::AIProcess*, RE::Actor*, const RE::ObjectRefHandle&);
            using ChooseFurnitureEntryPoint = bool(RE::AIProcess*, RE::Actor*);
            using UseFurnitureInstant = bool(RE::Actor*, RE::TESObjectREFR*, std::uint32_t);
            static REL::Relocation<SetOccupiedFurniture> setOccupiedFurniture{ RELOCATION_ID(38909, 39955) };
            static REL::Relocation<ChooseFurnitureEntryPoint> chooseFurnitureEntryPoint{ RELOCATION_ID(38882, 39923) };
            static REL::Relocation<UseFurnitureInstant> useFurnitureInstant{ RELOCATION_ID(36334, 37324) };
            setOccupiedFurniture(std::addressof(a_process), std::addressof(a_actor), a_furniture.GetHandle());
            if (!chooseFurnitureEntryPoint(std::addressof(a_process), std::addressof(a_actor))
                || !IsFurnitureEntryPointChosen(a_process))
            {
                return false;
            }
            return useFurnitureInstant(
                std::addressof(a_actor), std::addressof(a_furniture), a_process.middleHigh->currentFurnitureMarkerID);
        }

        bool StartNativeFurnitureEntry(
            RemoteActor& a_remote, RE::Actor& a_actor, RE::TESObjectREFR& a_furniture, float a_markerDistance)
        {
            auto* process = a_actor.GetActorRuntimeData().currentProcess;
            if (!process || !process->middleHigh)
            {
                return false;
            }

            (void)SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
            bool instant = false;
            if (a_remote.furniture.resumeFurnitureRefID == a_furniture.GetFormID())
            {
                // A seated pose whose snap onto the marker failed is still seated.
                instant = UseNativeFurnitureInstant(a_actor, a_furniture, *process)
                    || IsFurnitureEntryComplete(GetActorSitSleepState(a_actor));
                if (!instant)
                {
                    const auto releasedMarkerCount = ReleaseFurnitureMarkerReservations(a_actor, a_furniture);
                    process->ClearFurniture(std::addressof(a_actor));
                    (void)SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
                    logs::warn(
                        "Animation sync instant furniture entry refused; using the entry idle: actor={:08X} "
                        "furniture={:08X} releasedMarkers={}",
                        a_remote.handle.native_handle(), a_furniture.GetFormID(), releasedMarkerCount);
                }
            }
            const bool started = instant || UseNativeFurniture(a_actor, a_furniture);
            const auto* entryIdle = process->middleHigh->furnitureIdle;
            if (!started)
            {
                // Occupancy is recorded before the entry point is chosen and is left
                // behind when none is available.
                process->ClearFurniture(std::addressof(a_actor));
            }
            const auto* actorState = a_actor.AsActorState();
            logs::debug(
                "Animation sync started furniture entry: actor={:08X} furniture={:08X} started={} instant={} "
                "marker={} markerDistance={:.2f} entryIdle={:08X} occupied={:08X} sitSleepState={} weaponState={} "
                "inCombat={}",
                a_remote.handle.native_handle(), a_furniture.GetFormID(), started, instant,
                process->middleHigh->currentFurnitureMarkerID, a_markerDistance,
                started && entryIdle ? entryIdle->GetFormID() : 0, GetOccupiedFurnitureRefID(a_actor),
                static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(GetActorSitSleepState(a_actor)),
                actorState ? static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(actorState->GetWeaponState()) : 0,
                a_actor.IsInCombat());
            if (!started)
            {
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
            using func_t = void(RE::Actor*);
            static REL::Relocation<func_t> func{ RELOCATION_ID(36342, 37332) };
            func(std::addressof(a_actor));
        }

        bool IsAnimationDriven(RE::Actor& a_actor)
        {
            static const RE::BSFixedString animationDriven{ "bAnimationDriven" };
            bool value = false;
            return a_actor.GetGraphVariableBool(animationDriven, value) && value;
        }

        // ID 39865 is the get-up NPC package procedures run, which the do-nothing
        // package never reaches. From IsSitting or IsSleeping it performs
        // ActionActivate on the occupied furniture, so the exit idle is chosen from
        // the local idle records by their GetSitting/GetSleeping and
        // IsFurnitureEntryType conditions, then it sets WantToStand or WantToWake.
        // Setting that state directly skips the idle selection.
        bool StartNativeFurnitureExit(RE::Actor& a_actor, RemoteActor& a_remote, RE::SIT_SLEEP_STATE a_sitSleepState,
            RE::FormID a_occupiedFurnitureRefID)
        {
            auto* process = a_actor.GetActorRuntimeData().currentProcess;
            if (!process || !process->middleHigh)
            {
                return false;
            }

            const auto seatedIdle = GetFurnitureIdleFormID(a_actor);
            (void)ActorRegistry::ConsumeFurnitureExitClipEnds(a_remote);
            using StandUp = bool(RE::AIProcess*, RE::Actor*);
            static REL::Relocation<StandUp> standUp{ RELOCATION_ID(38835, 39865) };
            const bool result = standUp(process, std::addressof(a_actor));
            const auto sitSleepState = GetActorSitSleepState(a_actor);
            const bool started = sitSleepState == RE::SIT_SLEEP_STATE::kWantToStand
                || sitSleepState == RE::SIT_SLEEP_STATE::kWantToWake;
            a_remote.furniture.exitPhase = started ? ExitPhase::kNative : ExitPhase::kForced;
            if (started)
            {
                a_remote.furniture.furnitureTransitionElapsed = 0.0F;
                ActorRegistry::SetNativeFurnitureExit(a_remote, true);
            }
            logs::debug(
                "Animation sync started native furniture exit: actor={:08X} furniture={:08X} started={} result={} "
                "previousSitSleepState={} sitSleepState={} occupied={:08X} marker={} seatedIdle={:08X} "
                "furnitureIdle={:08X} animationDriven={}",
                a_remote.handle.native_handle(), a_occupiedFurnitureRefID, started, result,
                static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(a_sitSleepState),
                static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState),
                GetOccupiedFurnitureRefID(a_actor), process->middleHigh->currentFurnitureMarkerID, seatedIdle,
                GetFurnitureIdleFormID(a_actor), IsAnimationDriven(a_actor));
            return started;
        }

        void ResetFurnitureRequest(RemoteActor& a_remote)
        {
            a_remote.furniture.requestedFurnitureRefID = 0;
            a_remote.furniture.furnitureRetryRemaining = 0.0F;
            a_remote.furniture.furnitureTransitionElapsed = 0.0F;
            a_remote.furniture.occupancyObserved = false;
            a_remote.furniture.furnitureExitGraphForced = false;
            if (a_remote.furniture.exitPhase != ExitPhase::kNone)
            {
                ActorRegistry::SetNativeFurnitureExit(a_remote, false);
                a_remote.furniture.exitPhase = ExitPhase::kNone;
            }
            a_remote.furniture.exitReleaseElapsed = 0.0F;
            a_remote.furniture.exitClipEnded = false;
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
                // ID 39861 records WantToSit and reserves the process's marker only
                // from Normal, without checking that a marker was chosen. While the
                // entry waited, it may have done so with marker -1; an entry point
                // chosen afterwards would then never reach that step.
                const auto releasedMarkerCount = ReleaseFurnitureMarkerReservations(a_actor, *furniture);
                (void)SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
                const bool started = UseNativeFurniture(a_actor, *furniture);
                const bool entryPointChosen = IsFurnitureEntryPointChosen(a_process);
                if (!entryPointChosen)
                {
                    a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryIntervalSeconds;
                }
                const auto* entryIdle = a_process.middleHigh ? a_process.middleHigh->furnitureIdle : nullptr;
                logs::debug(
                    "Animation sync resumed deferred furniture entry: actor={:08X} furniture={:08X} started={} "
                    "marker={} entryIdle={:08X} previousSitSleepState={} releasedMarkers={} elapsed={:.3f}",
                    a_remote.handle.native_handle(), furniture->GetFormID(), started,
                    a_process.middleHigh ? a_process.middleHigh->currentFurnitureMarkerID
                                         : static_cast<std::uint32_t>(-1),
                    entryIdle ? entryIdle->GetFormID() : 0,
                    static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(a_sitSleepState), releasedMarkerCount,
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
            a_process.ClearFurniture(std::addressof(a_actor));
            (void)SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
            SurvivalPolicy::AssertDoNothingPackage(a_actor);
            ResetFurnitureRequest(a_remote);
            a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryIntervalSeconds;
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
        a_remote.furniture.targetHeldByOtherActor = false;
        a_remote.furniture.loggedEntryGate = EntryGate::kNone;
        if (a_furnitureRefID != a_remote.furniture.resumeFurnitureRefID)
        {
            // The source left the furniture it used at spawn; entering it again is an entry
            // the local player can watch.
            a_remote.furniture.resumeFurnitureRefID = 0;
        }
        if (a_remote.furniture.requestedFurnitureRefID == 0)
        {
            // An in-progress exit keeps its elapsed time, so rapid target changes cannot
            // postpone the forced-exit fallback indefinitely.
            a_remote.furniture.furnitureTransitionElapsed = 0.0F;
            a_remote.furniture.furnitureExitGraphForced = false;
        }
        logs::debug(
            "Animation sync updated furniture request: actor={:08X} previous={:08X} desired={:08X} readyElapsed={:.3f}",
            a_remote.handle.native_handle(), previousFurnitureRefID, a_furnitureRefID, a_remote.readyElapsed);
    }

    void ResumeFurniture(RemoteActor& a_remote, RE::FormID a_furnitureRefID)
    {
        a_remote.furniture.resumeFurnitureRefID = a_furnitureRefID;
    }

    RE::FormID GetTargetFurniture(const RemoteActor& a_remote)
    {
        return a_remote.furniture.targetFurnitureRefID;
    }

    bool IsFurnitureInteractionPlaying(const RemoteActor& a_remote)
    {
        return a_remote.furniture.requestedFurnitureRefID != 0 || a_remote.furniture.furnitureAnimationOwned;
    }

    bool IsTargetHeldByOtherActor(const RemoteActor& a_remote)
    {
        return a_remote.furniture.targetHeldByOtherActor;
    }

    void ReleaseFurniture(RE::Actor& a_actor, RemoteActor& a_remote)
    {
        if (!GameThread::Require("ReleaseFurniture"))
        {
            return;
        }

        auto furniture = a_actor.GetOccupiedFurniture().get();
        std::uint32_t releasedMarkerCount = furniture ? ReleaseFurnitureMarkerReservations(a_actor, *furniture) : 0;
        // An entry Skyrim dropped keeps the reservation without the occupancy.
        const auto requestedFurnitureRefID = a_remote.furniture.requestedFurnitureRefID;
        if (requestedFurnitureRefID != 0 && (!furniture || furniture->GetFormID() != requestedFurnitureRefID))
        {
            if (auto* requestedFurniture = RE::TESForm::LookupByID<RE::TESObjectREFR>(requestedFurnitureRefID))
            {
                releasedMarkerCount += ReleaseFurnitureMarkerReservations(a_actor, *requestedFurniture);
            }
        }
        if (auto* process = a_actor.GetActorRuntimeData().currentProcess)
        {
            process->ClearFurniture(std::addressof(a_actor));
        }
        (void)SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
        SetFurnitureAnimationOwnership(a_remote, false);
        ActorRegistry::SetHeldFurniture(a_remote, 0);
        ResetFurnitureRequest(a_remote);
        a_remote.furniture.targetFurnitureRefID = 0;
        a_remote.furniture.loggedSitSleepState = RE::SIT_SLEEP_STATE::kNormal;
        a_remote.furniture.entrySettleLogged = false;
        a_remote.furniture.loggedEntryGate = EntryGate::kNone;
        if (furniture || releasedMarkerCount != 0)
        {
            logs::debug(
                "Animation sync released actor furniture: actor={:08X} furniture={:08X} "
                "releasedMarkers={}",
                a_remote.handle.native_handle(), furniture ? furniture->GetFormID() : 0, releasedMarkerCount);
        }
    }

    bool HoldsFurniture(RE::Actor& a_actor, const RemoteActor& a_remote, RE::TESObjectREFR& a_furniture)
    {
        const auto furnitureRefID = a_furniture.GetFormID();
        if (GetOccupiedFurnitureRefID(a_actor) == furnitureRefID
            || a_remote.furniture.requestedFurnitureRefID == furnitureRefID)
        {
            return true;
        }
        return HasMarkerReservation(a_actor, a_furniture);
    }

    bool HasMarkerReservation(RE::Actor& a_actor, RE::TESObjectREFR& a_furniture)
    {
        const auto* usedMarkers = a_furniture.extraList.GetByType<RE::ExtraUsedMarkers>();
        if (!usedMarkers)
        {
            return false;
        }
        const auto actorHandle = a_actor.GetHandle();
        return std::ranges::any_of(
            usedMarkers->usedMarkers, [&](const auto& a_marker) { return a_marker.actorinMarker == actorHandle; });
    }

    bool HasAvailableMarker(RE::Actor& a_user, RE::TESObjectREFR& a_furniture)
    {
        return FindAvailableFurnitureMarker(a_user, a_furniture) >= 0;
    }

    bool HasAvailableMarkerBeside(RE::Actor& a_user, RE::Actor& a_occupant, RE::TESObjectREFR& a_furniture)
    {
        const auto marker = FindAvailableFurnitureMarker(a_user, a_furniture);
        if (marker < 0)
        {
            return false;
        }
        // A seated occupant is not always visible to the probe, and native entry
        // onto its marker is taken back, so that marker never counts as free.
        const auto* process = a_occupant.GetActorRuntimeData().currentProcess;
        return !process || !process->middleHigh
            || process->middleHigh->currentFurnitureMarkerID != static_cast<std::uint32_t>(marker);
    }

    bool AdvanceFurnitureInteraction(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
    {
        PROFILE_FUNCTION();
        const auto targetFurnitureRefID = a_remote.furniture.targetFurnitureRefID;
        const auto occupiedFurnitureRefID = GetOccupiedFurnitureRefID(a_actor);
        // Published before this pass advances it; the activation reading it runs on a worker thread anyway.
        ActorRegistry::SetHeldFurniture(a_remote,
            occupiedFurnitureRefID != 0 ? occupiedFurnitureRefID : a_remote.furniture.requestedFurnitureRefID);
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        a_remote.furniture.furnitureRetryRemaining -= deltaTime;
        const auto sitSleepState = GetActorSitSleepState(a_actor);
        if (sitSleepState != a_remote.furniture.loggedSitSleepState)
        {
            logs::debug(
                "Animation sync actor sit/sleep state changed: actor={:08X} previous={} current={} occupied={:08X} "
                "target={:08X} furnitureIdle={:08X}",
                a_remote.handle.native_handle(),
                static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(a_remote.furniture.loggedSitSleepState),
                static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState), occupiedFurnitureRefID,
                targetFurnitureRefID, GetFurnitureIdleFormID(a_actor));
            a_remote.furniture.loggedSitSleepState = sitSleepState;
        }
        if (occupiedFurnitureRefID != 0)
        {
            UpdateNativeSitSleepState(a_actor);
        }

        const bool exiting = a_remote.furniture.exitPhase != ExitPhase::kNone;

        if (!exiting && targetFurnitureRefID != 0 && occupiedFurnitureRefID == targetFurnitureRefID)
        {
            SetFurnitureAnimationOwnership(a_remote, true);
            a_remote.furniture.requestedFurnitureRefID = targetFurnitureRefID;
            auto* process = a_actor.GetActorRuntimeData().currentProcess;
            if (!a_remote.furniture.occupancyObserved)
            {
                // The pass after the native call is the first one that can see what it recorded.
                a_remote.furniture.occupancyObserved = true;
                logs::debug(
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
            a_remote.furniture.furnitureExitGraphForced = false;
            return true;
        }

        if (!exiting && targetFurnitureRefID != 0 && a_remote.furniture.requestedFurnitureRefID == targetFurnitureRefID
            && occupiedFurnitureRefID == 0)
        {
            // Hold ownership for a bounded time before releasing the request for a fresh attempt.
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
            SurvivalPolicy::AssertDoNothingPackage(a_actor);
            ResetFurnitureRequest(a_remote);
            SetFurnitureAnimationOwnership(a_remote, false);
            if (!occupancyObserved)
            {
                // Retrying on the next pass restarts the entry idle from its first frame.
                a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryIntervalSeconds;
                // The native entry can record occupancy that the engine takes back
                // before the next pass, and still report success.
                if (auto* furniture = RE::TESForm::LookupByID<RE::TESObjectREFR>(targetFurnitureRefID))
                {
                    LogFurnitureMarkerReservations(a_remote.handle.native_handle(), *furniture);
                }
            }
            return false;
        }

        // An interrupted entry idle can still reach its seated node after the engine
        // bookkeeping was cleared, which leaves the actor sitting with no occupancy,
        // request, or target.
        const bool residualSitSleepState = sitSleepState != RE::SIT_SLEEP_STATE::kNormal;
        const bool mustExitCurrentFurniture
            = occupiedFurnitureRefID != 0 || a_remote.furniture.requestedFurnitureRefID != 0 || residualSitSleepState;
        const bool furnitureAnimationOwned = a_remote.furniture.furnitureAnimationOwned;
        if (mustExitCurrentFurniture || furnitureAnimationOwned)
        {
            if (residualSitSleepState && occupiedFurnitureRefID == 0 && a_remote.furniture.requestedFurnitureRefID == 0
                && !furnitureAnimationOwned && a_remote.furniture.furnitureRetryRemaining <= 0.0F)
            {
                logs::warn(
                    "Animation sync recovering furniture pose without occupancy: actor={:08X} target={:08X} "
                    "sitSleepState={}",
                    a_remote.handle.native_handle(), targetFurnitureRefID,
                    static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState));
            }
            SetFurnitureAnimationOwnership(a_remote, true);
            const auto exitPhase = a_remote.furniture.exitPhase;
            const bool nativeExit = exitPhase == ExitPhase::kNative;
            if (nativeExit && !a_remote.furniture.exitClipEnded
                && ActorRegistry::ConsumeFurnitureExitClipEnds(a_remote) != 0)
            {
                a_remote.furniture.exitClipEnded = true;
            }
            // The clip ends about 0.2 s before the engine's get-up settle does, and
            // locomotion only plays after that settle.
            const bool cutExitSettle = nativeExit && a_remote.furniture.exitClipEnded
                && a_actor.GetPosition().GetDistance(a_remote.movement.targetPosition) > kExitSettleCutDistance;
            // A native exit counts as released once the state is Normal, even with occupancy
            // left: cancelling it as an incomplete entry would force the default graph state
            // over the exit clip.
            if (cutExitSettle
                || ((occupiedFurnitureRefID == 0 || nativeExit)
                    && GetActorSitSleepState(a_actor) == RE::SIT_SLEEP_STATE::kNormal))
            {
                if (nativeExit && !cutExitSettle && IsAnimationDriven(a_actor)
                    && a_remote.furniture.exitReleaseElapsed < kNativeExitClipTailSeconds)
                {
                    // A table's exit returns the actor to Normal while the tail of its
                    // clip still moves it. Locomotion dispatched then is accepted
                    // and never played, until a sprint or jump forces a transition.
                    a_remote.furniture.furnitureTransitionElapsed += deltaTime;
                    a_remote.furniture.exitReleaseElapsed += deltaTime;
                    return true;
                }
                if (nativeExit && !cutExitSettle && a_remote.furniture.exitReleaseElapsed >= kNativeExitClipTailSeconds)
                {
                    logs::warn(
                        "Animation sync furniture exit clip still animation-driven after release: actor={:08X} "
                        "furniture={:08X} releaseElapsed={:.3f}",
                        a_remote.handle.native_handle(), a_remote.furniture.requestedFurnitureRefID,
                        a_remote.furniture.exitReleaseElapsed);
                }
                const auto exitedFurnitureRefID = a_remote.furniture.requestedFurnitureRefID;
                const auto exitElapsed = a_remote.furniture.furnitureTransitionElapsed + deltaTime;
                const auto exitReleaseElapsed = a_remote.furniture.exitReleaseElapsed;
                std::uint32_t releasedMarkerCount = 0;
                if (auto occupiedFurniture = a_actor.GetOccupiedFurniture().get())
                {
                    releasedMarkerCount = ReleaseFurnitureMarkerReservations(a_actor, *occupiedFurniture);
                    if (auto* process = a_actor.GetActorRuntimeData().currentProcess)
                    {
                        process->ClearFurniture(std::addressof(a_actor));
                    }
                }
                const auto cutSitSleepState = GetActorSitSleepState(a_actor);
                bool settleCutAccepted = false;
                if (cutExitSettle)
                {
                    // The engine may not have released the furniture yet; its exit
                    // handler ignores an actor that is already Normal.
                    (void)SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
                    settleCutAccepted = AnimationFirewall::ForceDefaultGraphState(a_actor);
                }
                SurvivalPolicy::AssertDoNothingPackage(a_actor);
                ResetFurnitureRequest(a_remote);
                SetFurnitureAnimationOwnership(a_remote, false);
                // Furniture graphs do not consistently return to the locomotion
                // variables which preceded entry.
                ActorAnimation::InvalidateAnimationDispatch(a_remote.animation);
                const auto* actorState = a_actor.AsActorState();
                if (a_remote.animation.targetAnimationState.weaponDrawn && actorState
                    && actorState->GetWeaponState() == RE::WEAPON_STATE::kSheathed)
                {
                    // Sitting sheathes the weapon, but the staged request still holds the pre-entry drawn state.
                    auto sheathedState = a_remote.animation.targetAnimationState;
                    sheathedState.weaponDrawn = false;
                    ActorAnimation::SetAnimationState(a_remote, sheathedState);
                }
                logs::debug(
                    "Animation sync completed furniture exit: actor={:08X} furniture={:08X} nextFurniture={:08X} "
                    "exitPhase={} leftoverOccupied={:08X} releasedMarkers={} animationDriven={} elapsed={:.3f} "
                    "releaseElapsed={:.3f} settleCut={} settleCutAccepted={} sitSleepStateAtCut={}",
                    a_remote.handle.native_handle(), exitedFurnitureRefID, targetFurnitureRefID,
                    static_cast<std::underlying_type_t<ExitPhase>>(exitPhase), occupiedFurnitureRefID,
                    releasedMarkerCount, IsAnimationDriven(a_actor), exitElapsed, exitReleaseElapsed, cutExitSettle,
                    settleCutAccepted, static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(cutSitSleepState));
                return false;
            }

            if (sitSleepState == RE::SIT_SLEEP_STATE::kWaitingForSitAnim && occupiedFurnitureRefID != 0
                && occupiedFurnitureRefID == a_remote.furniture.requestedFurnitureRefID
                && a_remote.furniture.furnitureTransitionElapsed < kFurnitureEntryFallbackSeconds)
            {
                // A source stands only once seated, so an exit arriving mid-entry finishes the entry first.
                a_remote.furniture.furnitureTransitionElapsed += deltaTime;
                return true;
            }

            if (occupiedFurnitureRefID != 0 && !IsFurnitureEntryComplete(sitSleepState))
            {
                // Release the reservation; the next update completes the exit.
                a_remote.furniture.furnitureTransitionElapsed += deltaTime;
                auto occupiedFurniture = a_actor.GetOccupiedFurniture().get();
                if (occupiedFurniture)
                {
                    (void)ReleaseFurnitureMarkerReservations(a_actor, *occupiedFurniture);
                }
                if (auto* process = a_actor.GetActorRuntimeData().currentProcess)
                {
                    process->ClearFurniture(std::addressof(a_actor));
                }
                (void)SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
                // Clearing the engine's bookkeeping leaves the graph in the entry
                // idle it is already playing. That idle then runs to its seated node
                // and the engine sets IsSitting on an actor which occupies nothing,
                // where no target or request is left for the exit path to notice.
                const bool defaultStateAccepted = AnimationFirewall::ForceDefaultGraphState(a_actor);
                if (a_remote.furniture.furnitureRetryRemaining <= 0.0F)
                {
                    a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryIntervalSeconds;
                    logs::debug(
                        "Animation sync cancelled incomplete furniture entry: actor={:08X} occupied={:08X} "
                        "requested={:08X} target={:08X} sitSleepState={} defaultStateAccepted={} elapsed={:.3f}",
                        a_remote.handle.native_handle(), occupiedFurnitureRefID,
                        a_remote.furniture.requestedFurnitureRefID, targetFurnitureRefID,
                        static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState), defaultStateAccepted,
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
                SurvivalPolicy::AssertDoNothingPackage(a_actor);
                ResetFurnitureRequest(a_remote);
                SetFurnitureAnimationOwnership(a_remote, false);
                a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryIntervalSeconds;
                return false;
            }

            a_remote.furniture.furnitureTransitionElapsed += deltaTime;
            if (a_remote.furniture.exitPhase == ExitPhase::kNone && occupiedFurnitureRefID != 0
                && (sitSleepState == RE::SIT_SLEEP_STATE::kIsSitting
                    || sitSleepState == RE::SIT_SLEEP_STATE::kIsSleeping)
                && StartNativeFurnitureExit(a_actor, a_remote, sitSleepState, occupiedFurnitureRefID))
            {
                return true;
            }
            if (a_remote.furniture.exitPhase == ExitPhase::kNative)
            {
                const float releaseSeconds = sitSleepState == RE::SIT_SLEEP_STATE::kWantToWake
                    ? kNativeWakeReleaseSeconds
                    : kNativeExitReleaseSeconds;
                if (a_remote.furniture.furnitureTransitionElapsed < releaseSeconds)
                {
                    return true;
                }
                logs::warn(
                    "Animation sync native furniture exit never released the furniture: actor={:08X} "
                    "occupied={:08X} sitSleepState={} furnitureIdle={:08X} elapsed={:.3f}",
                    a_remote.handle.native_handle(), occupiedFurnitureRefID,
                    static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState),
                    GetFurnitureIdleFormID(a_actor), a_remote.furniture.furnitureTransitionElapsed);
                // The wake window outlasts the stuck-exit fallback, which therefore
                // counts from the forced exit.
                a_remote.furniture.furnitureTransitionElapsed = 0.0F;
            }
            a_remote.furniture.exitPhase = ExitPhase::kForced;

            const bool standingUp = sitSleepState == RE::SIT_SLEEP_STATE::kWantToStand
                || sitSleepState == RE::SIT_SLEEP_STATE::kWantToWake;
            if (a_remote.furniture.furnitureRetryRemaining <= 0.0F)
            {
                bool stateAccepted = standingUp;
                if (!standingUp)
                {
                    // No get-up package: ID 39861 ignores WantToStand entirely, so it
                    // drives nothing for a remote actor, and it installs a run-once
                    // package whose procedure can never finish once the actor is out
                    // of the furniture. That package then stays selected, and combat
                    // sanitization retakes it every pass for the rest of the session
                    // while the engine keeps moving the actor off its marker.
                    const bool sleeping = sitSleepState == RE::SIT_SLEEP_STATE::kIsSleeping;
                    stateAccepted = SetActorSitSleepState(
                        a_actor, sleeping ? RE::SIT_SLEEP_STATE::kWantToWake : RE::SIT_SLEEP_STATE::kWantToStand);
                }
                a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryIntervalSeconds;
                logs::debug(
                    "Animation sync requested furniture exit: actor={:08X} occupied={:08X} target={:08X} "
                    "sitSleepState={} stateAccepted={} furnitureIdle={:08X} elapsed={:.3f}",
                    a_remote.handle.native_handle(), occupiedFurnitureRefID, targetFurnitureRefID,
                    static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState), stateAccepted,
                    GetFurnitureIdleFormID(a_actor), a_remote.furniture.furnitureTransitionElapsed);
            }

            if (!a_remote.furniture.furnitureExitGraphForced)
            {
                // No native exit idle is playing, so the graph node is left on the first forced pass.
                a_remote.furniture.furnitureExitGraphForced = true;
                static const RE::BSFixedString furnitureExitEvent{ "IdleFurnitureExit" };
                const bool eventAccepted = AnimationFirewall::NotifyNetworkAnimationGraph(a_actor, furnitureExitEvent);
                logs::debug(
                    "Animation sync forced furniture exit graph event: actor={:08X} occupied={:08X} sitSleepState={} "
                    "furnitureIdle={:08X} eventAccepted={} elapsed={:.3f}",
                    a_remote.handle.native_handle(), occupiedFurnitureRefID,
                    static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState),
                    GetFurnitureIdleFormID(a_actor), eventAccepted, a_remote.furniture.furnitureTransitionElapsed);
            }

            if (a_remote.furniture.furnitureTransitionElapsed >= kFurnitureExitFallbackSeconds)
            {
                if (auto* process = a_actor.GetActorRuntimeData().currentProcess)
                {
                    process->ClearFurniture(std::addressof(a_actor));
                }
                (void)SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
                (void)AnimationFirewall::ForceDefaultGraphState(a_actor);
                SurvivalPolicy::AssertDoNothingPackage(a_actor);
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

        auto* furniture = RE::TESForm::LookupByID<RE::TESObjectREFR>(targetFurnitureRefID);
        const auto* furnitureBase = furniture ? furniture->GetBaseObject() : nullptr;
        const bool isFurniture = furnitureBase && furnitureBase->As<RE::TESFurniture>();
        const bool loaded3D = furniture && furniture->Is3DLoaded();
        const bool sameCoordinateSpace = furniture && IsSameFurnitureCoordinateSpace(a_actor, *furniture);
        if (!furniture || !isFurniture || !loaded3D || !sameCoordinateSpace)
        {
            if (a_remote.furniture.furnitureRetryRemaining <= 0.0F)
            {
                a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryIntervalSeconds;
                logs::warn(
                    "Animation sync waiting for furniture reference: actor={:08X} furniture={:08X} resolved={} "
                    "furnitureBase={} loaded3D={} sameSpace={}",
                    a_remote.handle.native_handle(), targetFurnitureRefID, furniture != nullptr, isFurniture, loaded3D,
                    sameCoordinateSpace);
            }
            return false;
        }
        const bool resuming = a_remote.furniture.resumeFurnitureRefID == targetFurnitureRefID;
        if (a_remote.ready
            && (resuming ? a_remote.readyElapsed <= 0.0F : a_remote.readyElapsed < kFurnitureEntrySpawnSettleSeconds))
        {
            if (!a_remote.furniture.entrySettleLogged)
            {
                a_remote.furniture.entrySettleLogged = true;
                logs::debug(
                    "Animation sync deferring furniture entry until the spawn settles: actor={:08X} furniture={:08X} "
                    "resuming={} readyElapsed={:.3f}",
                    a_remote.handle.native_handle(), targetFurnitureRefID, resuming, a_remote.readyElapsed);
            }
            // The transform stays authoritative meanwhile, so the actor follows the source to the furniture.
            return false;
        }
        if (!a_remote.ready || a_remote.furniture.furnitureRetryRemaining > 0.0F)
        {
            return false;
        }
        if (!ActorAnimation::IsAnimationStateSettled(a_remote))
        {
            if (EnterEntryGate(a_remote, EntryGate::kAnimationUnsettled))
            {
                logs::debug(
                    "Animation sync furniture entry waiting for settled locomotion: actor={:08X} furniture={:08X}",
                    a_remote.handle.native_handle(), targetFurnitureRefID);
            }
            return false;
        }
        if (ActorAnimation::IsFurnitureEntryBlocked(a_remote))
        {
            if (EnterEntryGate(a_remote, EntryGate::kAnimationBlocked))
            {
                logs::debug(
                    "Animation sync furniture entry waiting for an attack, weapon, or graph transition: actor={:08X} "
                    "furniture={:08X}",
                    a_remote.handle.native_handle(), targetFurnitureRefID);
            }
            return false;
        }

        // The source reports occupancy when it activates, then travels to the entry
        // point before its entry idle moves it onto the marker.
        const auto actorPosition = a_actor.GetPosition();
        const float alignmentDistance = (a_remote.movement.targetPosition - actorPosition).Length();
        if (alignmentDistance > kFurnitureEntryAlignmentDistance)
        {
            if (EnterEntryGate(a_remote, EntryGate::kAlignment))
            {
                const auto& targetPosition = a_remote.movement.targetPosition;
                logs::debug(
                    "Animation sync furniture entry waiting for the actor to reach the checkpoint: actor={:08X} "
                    "furniture={:08X} actorPosition=({:.1f},{:.1f},{:.1f}) target=({:.1f},{:.1f},{:.1f}) "
                    "distance={:.1f}",
                    a_remote.handle.native_handle(), targetFurnitureRefID, actorPosition.x, actorPosition.y,
                    actorPosition.z, targetPosition.x, targetPosition.y, targetPosition.z, alignmentDistance);
            }
            return false;
        }
        const auto marker = FindAvailableFurnitureMarker(a_actor, *furniture);
        a_remote.furniture.targetHeldByOtherActor = marker < 0 && IsReservedByOtherActor(a_actor, *furniture);
        if (marker < 0)
        {
            a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryIntervalSeconds;
            logs::debug(
                "Animation sync found no available furniture marker: actor={:08X} furniture={:08X} heldByOther={}",
                a_remote.handle.native_handle(), targetFurnitureRefID, a_remote.furniture.targetHeldByOtherActor);
            if (a_remote.furniture.targetHeldByOtherActor)
            {
                LogFurnitureMarkerReservations(a_remote.handle.native_handle(), *furniture);
            }
            return false;
        }
        // The probe answers whether any marker is free, with the first free index.
        // The native entry chooses its own entry point, so on a bench the probe's
        // marker can lie at the far end while the actor stands beside another seat.
        const auto nearestMarker = FindNearestFurnitureMarker(*furniture, actorPosition);
        const float markerDistance = nearestMarker.distance;
        if (markerDistance > kFurnitureEntryMarkerDistance)
        {
            if (EnterEntryGate(a_remote, EntryGate::kMarkerDistance))
            {
                const auto probePosition
                    = GetFurnitureMarkerPosition(*furniture, marker).value_or(furniture->GetPosition());
                logs::debug(
                    "Animation sync furniture entry waiting for the actor to approach a marker: actor={:08X} "
                    "furniture={:08X} nearestMarker={} nearestDistance={:.1f} freeMarker={} freeDistance={:.1f}",
                    a_remote.handle.native_handle(), targetFurnitureRefID, nearestMarker.marker, markerDistance, marker,
                    std::hypot(probePosition.x - actorPosition.x, probePosition.y - actorPosition.y));
            }
            return false;
        }
        a_remote.furniture.loggedEntryGate = EntryGate::kNone;

        // Remove residual interpolation error before Skyrim aligns the actor.
        a_actor.SetPosition(a_remote.movement.targetPosition, true);
        if (std::isfinite(a_remote.movement.targetHeading))
        {
            a_actor.SetHeading(a_remote.movement.targetHeading);
        }
        SetFurnitureAnimationOwnership(a_remote, true);
        a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryIntervalSeconds;
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
