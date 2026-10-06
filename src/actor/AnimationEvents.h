#pragma once

#include <cstddef>
#include <string_view>

namespace AnimationEvents
{
    constexpr bool IsWeaponTransitionTraceEvent(std::string_view a_eventName) noexcept
    {
        return a_eventName.find("Equip") != std::string_view::npos
            || a_eventName.find("equip") != std::string_view::npos
            || a_eventName.find("Weapon") != std::string_view::npos
            || a_eventName.find("weapon") != std::string_view::npos;
    }

    static_assert(IsWeaponTransitionTraceEvent("BeginWeaponSheathe"));
    static_assert(IsWeaponTransitionTraceEvent("Unequip_OutMoving"));
    static_assert(IsWeaponTransitionTraceEvent("weaponDraw"));
    static_assert(!IsWeaponTransitionTraceEvent("FootLeft"));
    constexpr bool IsWeaponTransitionEndEvent(std::string_view a_eventName, bool a_weaponDrawn) noexcept
    {
        // Spell hands draw and sheathe through the magic equip branch instead.
        return a_weaponDrawn ? (a_eventName == "WeapEquip_Out" || a_eventName == "WeapEquip_OutMoving"
                                   || a_eventName == "Magic_Equip_Out" || a_eventName == "Magic_Equip_OutMoving")
                             : (a_eventName == "Unequip_Out" || a_eventName == "Unequip_OutMoving"
                                   || a_eventName == "Magic_Unequip_Out" || a_eventName == "Magic_Unequip_OutMoving");
    }

    constexpr bool EqualsIgnoringCase(std::string_view a_lhs, std::string_view a_rhs) noexcept
    {
        const auto lower = [](char a_char) {
            return a_char >= 'A' && a_char <= 'Z' ? static_cast<char>(a_char - 'A' + 'a') : a_char;
        };
        if (a_lhs.size() != a_rhs.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < a_lhs.size(); ++index)
        {
            if (lower(a_lhs[index]) != lower(a_rhs[index]))
            {
                return false;
            }
        }
        return true;
    }

    constexpr bool ContainsIgnoringCase(std::string_view a_text, std::string_view a_part) noexcept
    {
        for (std::size_t start = 0; start + a_part.size() <= a_text.size(); ++start)
        {
            if (EqualsIgnoringCase(a_text.substr(start, a_part.size()), a_part))
            {
                return true;
            }
        }
        return false;
    }

    constexpr bool StartsWithIgnoringCase(std::string_view a_text, std::string_view a_prefix) noexcept
    {
        return a_text.size() >= a_prefix.size() && EqualsIgnoringCase(a_text.substr(0, a_prefix.size()), a_prefix);
    }

    constexpr bool EndsWithIgnoringCase(std::string_view a_text, std::string_view a_suffix) noexcept
    {
        return a_text.size() >= a_suffix.size()
            && EqualsIgnoringCase(a_text.substr(a_text.size() - a_suffix.size()), a_suffix);
    }

    // Graph event names are case-insensitive, and the left release idle sends MLH_SpellRelease_event.
    constexpr bool IsCastAnimationEvent(std::string_view a_eventName) noexcept
    {
        for (const std::string_view prefix : { "MLh_", "MRh_", "DualMagic_" })
        {
            if (EqualsIgnoringCase(a_eventName.substr(0, prefix.size()), prefix))
            {
                return true;
            }
        }
        return ContainsIgnoringCase(a_eventName, "Cast");
    }

    enum class AttackEventHand
    {
        kRight,
        kLeft,
        kDual
    };

    constexpr AttackEventHand GetAttackEventHand(std::string_view a_eventName) noexcept
    {
        // attackPowerStartH2HCombo is the power attack of both fists.
        if (ContainsIgnoringCase(a_eventName, "DualWield") || ContainsIgnoringCase(a_eventName, "H2HCombo"))
        {
            return AttackEventHand::kDual;
        }
        // attackPowerStartLeft is a right-hand power attack toward the left, so only
        // the hand suffixes count.
        if (EndsWithIgnoringCase(a_eventName, "LeftHand") || EndsWithIgnoringCase(a_eventName, "H2HLeft"))
        {
            return AttackEventHand::kLeft;
        }
        return AttackEventHand::kRight;
    }

    static_assert(GetAttackEventHand("attackStart") == AttackEventHand::kRight);
    static_assert(GetAttackEventHand("attackPowerStartLeft") == AttackEventHand::kRight);
    static_assert(GetAttackEventHand("attackStartLeftHand") == AttackEventHand::kLeft);
    static_assert(GetAttackEventHand("attackStartSprintLeftHand") == AttackEventHand::kLeft);
    static_assert(GetAttackEventHand("attackPowerStartForwardH2HLeftHand") == AttackEventHand::kLeft);
    static_assert(GetAttackEventHand("AttackStartH2HLeft") == AttackEventHand::kLeft);
    static_assert(GetAttackEventHand("AttackStartH2HRight") == AttackEventHand::kRight);
    static_assert(GetAttackEventHand("attackPowerStartDualWield") == AttackEventHand::kDual);
    static_assert(GetAttackEventHand("attackPowerStartH2HCombo") == AttackEventHand::kDual);
    static_assert(GetAttackEventHand("attackPowerStartLeftLeftHand") == AttackEventHand::kLeft);
    static_assert(GetAttackEventHand("attackPowerStart_SprintLeftHand") == AttackEventHand::kLeft);

    // The vanilla race attack data names a power attack's direction after the
    // hand prefix (attackPowerStartLeftLeftHand is a left-hand attack toward the
    // left).
    enum class PowerAttackEventDirection
    {
        kStanding,
        kForward,
        kBackward,
        kLeft,
        kRight
    };

    constexpr PowerAttackEventDirection GetPowerAttackEventDirection(std::string_view a_eventName) noexcept
    {
        std::string_view name = a_eventName;
        for (const std::string_view handSuffix : { "LeftHand", "RightHand" })
        {
            if (EndsWithIgnoringCase(name, handSuffix))
            {
                name.remove_suffix(handSuffix.size());
                break;
            }
        }
        if (ContainsIgnoringCase(name, "Forward") || ContainsIgnoringCase(name, "Sprint"))
        {
            return PowerAttackEventDirection::kForward;
        }
        if (ContainsIgnoringCase(name, "Back"))
        {
            return PowerAttackEventDirection::kBackward;
        }
        if (EndsWithIgnoringCase(name, "Left"))
        {
            return PowerAttackEventDirection::kLeft;
        }
        if (EndsWithIgnoringCase(name, "Right"))
        {
            return PowerAttackEventDirection::kRight;
        }
        return PowerAttackEventDirection::kStanding;
    }

    static_assert(GetPowerAttackEventDirection("attackPowerStartInPlace") == PowerAttackEventDirection::kStanding);
    static_assert(GetPowerAttackEventDirection("attackPowerStartForward") == PowerAttackEventDirection::kForward);
    static_assert(GetPowerAttackEventDirection("attackPowerStartBackward") == PowerAttackEventDirection::kBackward);
    static_assert(GetPowerAttackEventDirection("attackPowerStartLeft") == PowerAttackEventDirection::kLeft);
    static_assert(GetPowerAttackEventDirection("attackPowerStartRight") == PowerAttackEventDirection::kRight);
    static_assert(
        GetPowerAttackEventDirection("attackPowerStartInPlaceLeftHand") == PowerAttackEventDirection::kStanding);
    static_assert(
        GetPowerAttackEventDirection("attackPowerStartForwardLeftHand") == PowerAttackEventDirection::kForward);
    static_assert(GetPowerAttackEventDirection("attackPowerStartBackLeftHand") == PowerAttackEventDirection::kBackward);
    static_assert(GetPowerAttackEventDirection("attackPowerStartLeftLeftHand") == PowerAttackEventDirection::kLeft);
    static_assert(GetPowerAttackEventDirection("attackPowerStartRightLeftHand") == PowerAttackEventDirection::kRight);
    static_assert(
        GetPowerAttackEventDirection("attackPowerStartForwardH2HRightHand") == PowerAttackEventDirection::kForward);
    static_assert(
        GetPowerAttackEventDirection("attackPowerStartForwardH2HLeftHand") == PowerAttackEventDirection::kForward);
    static_assert(GetPowerAttackEventDirection("attackPowerStart_Sprint") == PowerAttackEventDirection::kForward);
    static_assert(GetPowerAttackEventDirection("attackPowerStart_2HWSprint") == PowerAttackEventDirection::kForward);
    static_assert(
        GetPowerAttackEventDirection("attackPowerStart_SprintLeftHand") == PowerAttackEventDirection::kForward);
    static_assert(GetPowerAttackEventDirection("attackPowerStartDualWield") == PowerAttackEventDirection::kStanding);
    static_assert(GetPowerAttackEventDirection("attackPowerStartH2HCombo") == PowerAttackEventDirection::kStanding);
    static_assert(GetPowerAttackEventDirection("bashPowerStart") == PowerAttackEventDirection::kStanding);

    static_assert(IsCastAnimationEvent("MRh_SpellAimedConcentrationStart"));
    static_assert(IsCastAnimationEvent("MLH_SpellRelease_event"));
    static_assert(IsCastAnimationEvent("InterruptCast"));
    static_assert(!IsCastAnimationEvent("attackStart"));
    static_assert(!IsCastAnimationEvent("MRh"));

    static_assert(IsWeaponTransitionEndEvent("Magic_Equip_OutMoving", true));

    static_assert(!IsWeaponTransitionEndEvent("weaponDraw", true));
    static_assert(IsWeaponTransitionEndEvent("WeapEquip_OutMoving", true));
    static_assert(!IsWeaponTransitionEndEvent("weaponSheathe", false));
    static_assert(IsWeaponTransitionEndEvent("Unequip_OutMoving", false));
    static_assert(!IsWeaponTransitionEndEvent("AttackStart", true));
}
