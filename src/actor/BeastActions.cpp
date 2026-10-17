#include "actor/BeastActions.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace BeastActions
{
    namespace
    {
        using Multiplayer::BeastAction;

        struct Entry
        {
            BeastAction action;
            const char* eventName;
        };

        // Input events of the vanilla WerewolfBehavior.hkx and VampireLord.hkx that the
        // forms' actions, idles, and casts send (the race attack data and the IDLE
        // records of Skyrim.esm and Dawnguard.esm). Locomotion, weapon stance, and
        // levitation are not actions: the checkpoint carries them.
        constexpr std::array kEntries{
            Entry{ BeastAction::kWerewolfAttackRight, "AttackStartRight" },
            Entry{ BeastAction::kWerewolfAttackLeft, "AttackStartLeft" },
            Entry{ BeastAction::kWerewolfAttackRightPower, "AttackStartRightPower" },
            Entry{ BeastAction::kWerewolfAttackLeftPower, "AttackStartLeftPower" },
            Entry{ BeastAction::kWerewolfAttackDual, "AttackStartDual" },
            Entry{ BeastAction::kWerewolfAttackBackhand, "AttackStartBackHand" },
            Entry{ BeastAction::kWerewolfAttackRightSide, "AttackStartRightSide" },
            Entry{ BeastAction::kWerewolfAttackLeftSide, "AttackStartLeftSide" },
            Entry{ BeastAction::kWerewolfAttackRightRunningPower, "AttackStartRightRunningPower" },
            Entry{ BeastAction::kWerewolfAttackLeftRunningPower, "AttackStartLeftRunningPower" },
            Entry{ BeastAction::kWerewolfAttackDualRunning, "AttackStartDualRunning" },
            Entry{ BeastAction::kWerewolfAttackDualSprinting, "AttackStartDualSprinting" },
            Entry{ BeastAction::kWerewolfAttackRightSprinting, "AttackStartRightSprinting" },
            Entry{ BeastAction::kWerewolfAttackLeftSprinting, "AttackStartLeftSprinting" },
            Entry{ BeastAction::kWerewolfHowlStart, "HowlStart" },
            Entry{ BeastAction::kWerewolfHowlRelease, "HowlRelease" },
            Entry{ BeastAction::kWerewolfHowlEnd, "HowlEnd" },
            Entry{ BeastAction::kWerewolfFeed, "SpecialFeeding" },
            Entry{ BeastAction::kWerewolfFeedPaired, "pa_KillWerewolfPairedFeedingWithHuman" },
            Entry{ BeastAction::kVampireLordAttackRight, "AttackStartRight" },
            Entry{ BeastAction::kVampireLordAttackLeft, "AttackStartLeft" },
            Entry{ BeastAction::kVampireLordAttackPowerRight, "AttackPowerStart_Right" },
            Entry{ BeastAction::kVampireLordAttackPowerLeft, "AttackPowerStart_Left" },
            Entry{ BeastAction::kVampireLordBatsStart, "BatSprintStart" },
            Entry{ BeastAction::kVampireLordBatsStop, "batSprintStop" },
            Entry{ BeastAction::kVampireLordFeedFront, "pa_KillMoveVampireLordFeedFront" },
            Entry{ BeastAction::kVampireLordFeedBack, "pa_KillMoveVampireLordFeedBack" },
            Entry{ BeastAction::kVampireLordCastBeginLeft, "BeginCastLeft" },
            Entry{ BeastAction::kVampireLordCastBeginRight, "BeginCastRight" },
            Entry{ BeastAction::kVampireLordCastAimedLeft, "MLh_SpellAimedStart" },
            Entry{ BeastAction::kVampireLordCastAimedRight, "MRh_SpellAimedStart" },
            Entry{ BeastAction::kVampireLordCastConcentrationLeft, "MLh_SpellAimedConcentrationStart" },
            Entry{ BeastAction::kVampireLordCastConcentrationRight, "MRh_SpellAimedConcentrationStart" },
            Entry{ BeastAction::kVampireLordCastReadyLeft, "MLh_SpellReady_Event" },
            Entry{ BeastAction::kVampireLordCastReadyRight, "MRh_SpellReady_Event" },
            Entry{ BeastAction::kVampireLordCastReleaseLeft, "MLh_SpellRelease_Event" },
            Entry{ BeastAction::kVampireLordCastReleaseRight, "MRh_SpellRelease_Event" },
            Entry{ BeastAction::kVampireLordCastInterruptLeft, "MLh_Equipped_Event" },
            Entry{ BeastAction::kVampireLordCastInterruptRight, "MRh_Equipped_Event" },
        };

        static_assert(std::ranges::all_of(kEntries, [](const Entry& a_entry) {
            return Multiplayer::GetBeastActionGraph(a_entry.action) != Multiplayer::Shapeshift::kNone;
        }));

        // The string pool folds case, so a pooled name compares with any spelling the
        // engine sends by pointer.
        const std::array<RE::BSFixedString, kEntries.size()>& GetEventNames()
        {
            static const auto names = [] {
                std::array<RE::BSFixedString, kEntries.size()> result;
                for (std::size_t i = 0; i < kEntries.size(); ++i)
                {
                    result[i] = kEntries[i].eventName;
                }
                return result;
            }();
            return names;
        }
    }

    const RE::BSFixedString& GetEventName(Multiplayer::BeastAction a_action)
    {
        static const RE::BSFixedString none;
        const auto& names = GetEventNames();
        for (std::size_t i = 0; i < kEntries.size(); ++i)
        {
            if (kEntries[i].action == a_action)
            {
                return names[i];
            }
        }
        return none;
    }

    std::optional<Multiplayer::BeastAction> Find(Multiplayer::Shapeshift a_graph, const RE::BSFixedString& a_eventName)
    {
        const auto& names = GetEventNames();
        for (std::size_t i = 0; i < kEntries.size(); ++i)
        {
            if (names[i] == a_eventName && Multiplayer::GetBeastActionGraph(kEntries[i].action) == a_graph)
            {
                return kEntries[i].action;
            }
        }
        return std::nullopt;
    }
}
