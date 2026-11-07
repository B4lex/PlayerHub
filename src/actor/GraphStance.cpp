#include "actor/GraphStance.h"

#include "actor/BehaviorGraphLayout.h"
#include "runtime/Profiling.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <string_view>

namespace GraphStance
{
    namespace
    {
        using namespace BehaviorGraphLayout;

        // The vanilla sneak branches tag iState 2 (BSiStateTaggingGenerator) and set
        // IsSneaking (BSIsActiveModifier, which clears it on deactivation) while
        // active. Neither covers every branch: MagicBehavior's sneak branches only
        // tag iState, and the unarmed, weapon, and bow sneak idles only set IsSneaking.
        constexpr std::int32_t kSneakStateTag = 2;

        enum class StanceAxis
        {
            kSneak,
            kMoving,
            kSprint,
            kLevitation,
            kIdle
        };

        struct StanceSelector
        {
            StanceAxis axis;
            std::string_view machine;
            std::string_view offState;
            // Empty: every state but offState.
            std::string_view onState;
            // An idle machine of a weapon-drawn behavior.
            bool drawn{ false };
        };

        // Every SneakStart, SneakStop, moveStart, moveStop, SprintStart, and SprintStop
        // transition of the vanilla third-person behaviors (Skyrim - Animations.bsa)
        // belongs to a two-state machine like these, so its current state switches when the event
        // is taken, while the branch it leaves still blends out. Turn, shout,
        // first-person, horse, flinch, stagger, bleedout, drunk, and crossbow aim
        // and reload selectors are left out, and so are the magic locomotion speed
        // samplers, which re-enter idle inside a moving branch, and the jump and
        // landing sprint variants, which linger while the landing blends out. Machines that pick a
        // variant only on entry, without these transitions, are left out too.
        constexpr std::array kStanceSelectors{
            StanceSelector{ StanceAxis::kSneak, "MTIdleTurnTypeBehavior", "MTIdleTurnState", "SneakIdleTurnState" },
            StanceSelector{
                StanceAxis::kSneak, "MT_LocomotionType_Behavior", "MT_Locomotion_State", "MT_Sneak_Locomotion_State" },
            StanceSelector{ StanceAxis::kSneak, "Weap_IdleType_Behavior", "Weap_IdleState", "Sneak1HM_IdleState" },
            StanceSelector{ StanceAxis::kSneak, "H2H_Standing_ArmPoseBehavior", "DefaultH2H_Idle", "SneakH2HIdle" },
            StanceSelector{ StanceAxis::kSneak, "Melee_DirectionType_Behavior", "Melee_Direction_Behavior",
                "Sneak_Direction_Behavior" },
            StanceSelector{ StanceAxis::kSneak, "BowLoweredType_Behavior", "BowLoweredState", "SneakBowLoweredState" },
            StanceSelector{
                StanceAxis::kSneak, "Bow_IdleDrawType_Behavior", "Bow_IdleDrawnState", "SneakBow_IdleDrawnState" },
            StanceSelector{
                StanceAxis::kSneak, "Bow_ReleaseType_Behavior", "Bow_ReleaseState", "SneakBow_ReleaseState" },
            StanceSelector{ StanceAxis::kSneak, "Bow_DirectionType_Behavior", "Bow_Direction_Behavior",
                "SneakBow_Locomotion_State" },
            StanceSelector{ StanceAxis::kSneak, "CrossBowLoweredType_Behavior", "CrossBowLoweredState",
                "SneakCrossBowLoweredState" },
            StanceSelector{ StanceAxis::kSneak, "CrossBow_DirectionType_Behavior", "CrossBow_Direction_Behavior",
                "SneakBow_Locomotion_State" },
            StanceSelector{ StanceAxis::kSneak, "Shd_IdleType_Behavior", "DefaultBlockIdle", "SneakBlockIdle" },
            StanceSelector{
                StanceAxis::kSneak, "BlockLocomotionBehavior", "DefaultBlockLocomotion", "Sneak_Direction_Behavior" },
            StanceSelector{ StanceAxis::kSneak, "Bash_Behavior", "DefaultBlockBash", "SneakBlockBash" },
            StanceSelector{ StanceAxis::kSneak, "BashPowerBehavior", "DefaultBlockBashPower", "SneakBlockBashPower" },
            StanceSelector{ StanceAxis::kSneak, "MagicReadyRootBehavior", "DefaultMagicReady", "SneakMagicReady" },
            StanceSelector{
                StanceAxis::kSneak, "MagicCastingRootBehavior", "DefaultMagicCasting", "SneakMagicCasting" },
            StanceSelector{ StanceAxis::kSneak, "Magic_Locomotion_Root", "Magic_Locomotion", "Magic_Sneak_Locomotion" },
            StanceSelector{ StanceAxis::kSneak, "MRh_Unequip_StandingBehavior", "MRh_Unequip_DefaultStanding_Behavior",
                "MRh_Unequip_SneakStanding_Behavior" },
            StanceSelector{
                StanceAxis::kSneak, "Weap_EquipBehavior", "Weap_EquipDefaultState", "Weap_EquipSneakState" },
            StanceSelector{ StanceAxis::kSneak, "WeapRightReplace_ForceEquipStandingBehavior",
                "WeapRightReplaceDefault_ForceEquipState", "WeapRightReplaceSneak_ForceEquipState" },
            StanceSelector{ StanceAxis::kSneak, "WeapLeftReplace_ForceEquipStandingBehavior",
                "1HMForceEquipDefaultState", "1HMForceEquipsSneakState" },
            StanceSelector{ StanceAxis::kSneak, "MagicWeap_ForceEquipStandingBehavior",
                "DefaultMagicWeap_ForceEquipStandingState", "SneakMagicWeap_ForceEquipStandingState" },
            StanceSelector{
                StanceAxis::kMoving, "MT_Default_Behavior", "MT_Standing_State", "MT_LocomotionType_State" },
            StanceSelector{
                StanceAxis::kMoving, "1HM_Standing_Locomotion_Behavior", "1HM_Standing_State", "1HM_Locomotion_State" },
            StanceSelector{
                StanceAxis::kMoving, "Bow_Standing_Locomotion_Behavior", "Bow_Standing_State", "Bow_Locomotion_State" },
            StanceSelector{ StanceAxis::kMoving, "CrossBow_Standing_Locomotion_Behavior", "CrossBow_Standing_State",
                "CrossBow_Locomotion_State" },
            StanceSelector{ StanceAxis::kMoving, "DefaultMagicReadyBehavior", "MagicReadyStandingState",
                "MagicReadyLocomotionState" },
            StanceSelector{ StanceAxis::kMoving, "SneakMagicReadyBehavior", "SneakMagicReadyStanding",
                "SneakMagicReadyLocomotion" },
            StanceSelector{ StanceAxis::kMoving, "DefaultMagicCastingBehavior", "MagicCastingStandingState",
                "MagicCastingLocomotionState" },
            StanceSelector{
                StanceAxis::kMoving, "Blocking_LowerBody_Behavior", "Block_Standing_State", "Shd_Locomotion" },
            StanceSelector{ StanceAxis::kMoving, "BlockIdle_MovingStanding_Behavior", "BlockIdle_StandingState",
                "BlockIdle_MovingState" },
            StanceSelector{
                StanceAxis::kMoving, "Bow_Attack_LowerBodyBehavior", "Bow_AttackStanding", "Bow_AttackMoving" },
            StanceSelector{ StanceAxis::kMoving, "Torch_Ready_Behavior", "Torch_Standing", "Tor_Locomotion" },
            StanceSelector{
                StanceAxis::kMoving, "H2H_Readied_BlendPose_Behavior", "H2H_Standing_ArmPose", "H2H_Moving_ArmPose" },
            StanceSelector{ StanceAxis::kMoving, "Swim_Behavior", "SwimIdle", "SwimLocomotion" },
            StanceSelector{
                StanceAxis::kMoving, "WeapEquipBehavior", "WeapEquip_StandingState", "WeapEquip_LocomotionState" },
            StanceSelector{ StanceAxis::kMoving, "WeapRightReplace_ForceEquipBehavior",
                "WeapRightReplace_ForceEquipStanding", "WeapRightReplace_ForceEquipLocomotion" },
            StanceSelector{ StanceAxis::kMoving, "WeapLeftReplace_ForceEquipBehavior",
                "WeapLeftReplace_ForceEquipStanding", "WeapLeftReplace_ForceEquipLocomotion" },
            StanceSelector{ StanceAxis::kMoving, "MagicWeap_ForceEquipBehavior", "MagicWeap_ForceEquipStanding",
                "MagicWeap_ForceEquipLocomotion" },
            StanceSelector{
                StanceAxis::kMoving, "MagicForceEquipLowerBody", "MagicForceEquip_Standing", "MagicForceEquip_Moving" },
            StanceSelector{ StanceAxis::kMoving, "MLh_EquipLowerBody", "MLh_Equip_Standing", "MLh_Equip_Moving" },
            StanceSelector{ StanceAxis::kMoving, "MRh_EquipLowerBody", "MRh_Equip_Standing", "MRh_Equip_Moving" },
            StanceSelector{ StanceAxis::kMoving, "MLh_UnequipLowerBody", "MLh_Unequip_Standing", "MLh_Unequip_Moving" },
            StanceSelector{ StanceAxis::kMoving, "MRh_UnequipLowerBody", "MRh_Unequip_Standing", "MRh_Unequip_Moving" },
            StanceSelector{
                StanceAxis::kMoving, "Weap_UnequipLowerBody", "Weap_Unequip_Standing", "Weap_Unequip_Moving" },
            StanceSelector{ StanceAxis::kSprint, "MT_Locomotion_Behavior", "Default_Locomotion_State", "Sprint_State" },
            StanceSelector{
                StanceAxis::kSprint, "Sneak_Locomotion_Behavior", "Sneak_Locomotion_State", "Sneak_Sprint_State" },
            StanceSelector{ StanceAxis::kSprint, "Melee_Locomotion_Behavior", "Default_Melee_LocomotionState",
                "Melee_Sprint_State" },
            StanceSelector{ StanceAxis::kSprint, "Magic_Locomotion_Behavior", "Default_Magic_LocomotionState",
                "Magic_Sprint_State" },
            StanceSelector{
                StanceAxis::kSprint, "MagicReadyLocomotion_Behavior", "MagicReady_Locomotion", "MagicReady_Sprint" },
            StanceSelector{
                StanceAxis::kSprint, "MagicCastLocomotion_Behavior", "MagicCastLocomotion", "MagicCast_Sprint" },
            StanceSelector{ StanceAxis::kSprint, "SneakMagicReadyLocomotionBehavior", "SneakMagicReadyLocomotionState",
                "SneakMagicSprint" },
            StanceSelector{
                StanceAxis::kSprint, "H2H_SprintPoseBehavior", "H2H_LocomotionPoseState", "H2H_SprintPoseState" },
            StanceSelector{ StanceAxis::kSprint, "Tor_LocomotionBehavior", "Tor_LocomotionState", "Tor_SprintState" },
            StanceSelector{
                StanceAxis::kSprint, "MT_Torch_IdleLocomotion_Behavior", "MT_Torch_IdleLocomotion", "MT_Torch_Sprint" },
            StanceSelector{ StanceAxis::kSprint, "1HM_Torch_IdleLocomotion_Behavior", "MT_Torch_IdleLocomotion",
                "MT_Torch_Sprint" },
            // WerewolfBehavior.hkx, which the werebear shares, and VampireLord.hkx.
            StanceSelector{ StanceAxis::kMoving, "MovementBehavior", "Standing_State", "Locomotion_State" },
            StanceSelector{ StanceAxis::kMoving, "Behavior03", "IdleStanding_State", "LocomotionCombat" },
            StanceSelector{ StanceAxis::kMoving, "Behavior06", "WW SwimTread.hkx", "SwimDirectionalBlend" },
            StanceSelector{ StanceAxis::kMoving, "CombatReadyBehavior", "IdleStanding_State", "LocomotionCombat" },
            StanceSelector{ StanceAxis::kMoving, "EquipBehavior", "StandingEquip", "MovingEquip" },
            StanceSelector{ StanceAxis::kMoving, "UnequipBehavior", "UnequipStanding", "UnequipMoving" },
            StanceSelector{ StanceAxis::kMoving, "LevitationMovementBehavior", "Levitate_NonCombatIdle",
                "LevitationNonCombatLocomotion_State" },
            StanceSelector{
                StanceAxis::kMoving, "LevitateCombatBehavior", "LevitateCombatIdle", "Levitate_CombatLocomotion" },
            StanceSelector{ StanceAxis::kMoving, "CastState_Ready_Behavior", "CastState_CombatIdle",
                "CastState_CombatLocomotion" },
            StanceSelector{ StanceAxis::kSprint, "H2HBehavior", "H2H_Idle_State", "Sprint" },
            StanceSelector{ StanceAxis::kSprint, "Ground_StateBehavior", "GroundcombatNonCombat_State", "Sprint" },
            StanceSelector{
                StanceAxis::kSprint, "Levitation_Behavior", "LevitateCombatNonCombatState", "LevitationSprint" },
            // LevitationToggle leaves either state through its start or end clip, which
            // already belongs to the new stance.
            StanceSelector{ StanceAxis::kLevitation, "H2HBehavior", "Ground_State_Cam", "LevitationBase_State" },
            StanceSelector{ StanceAxis::kLevitation, "H2HBehavior", "Levitate_End", "Levitate_Start" },
            // The machines that the vanilla Idle* events enter (0_master.hkx, mt_behavior.hkx,
            // idlebehavior.hkx, 1hm_behavior.hkx), first and third person alike. Each idle
            // returns to the default state when it ends: a gesture through its clip's
            // IdleStop annotation, a held idle through its exit clip's IdleFurnitureExit.
            // Every other state of the unarmed root and the dialogue idle machine is an
            // idle, including furniture; the carry-arm machines hold the overlays some
            // idle clips start until OffsetStop.
            StanceSelector{ StanceAxis::kIdle, "MT_RootBehavior", "MT_State", {} },
            StanceSelector{ StanceAxis::kIdle, "MT_BehaviorGraph", "MT_Default_State", "InteractionObjectState" },
            StanceSelector{ StanceAxis::kIdle, "MT_BehaviorGraph", "MT_Default_State", "MT_BoyRitualState" },
            StanceSelector{ StanceAxis::kIdle, "MT_Idle_3rdPersonBehavior", "InitialIdleState", {} },
            StanceSelector{ StanceAxis::kIdle, "CarryLeftArmBehavior", "MTLeftArmState", {} },
            StanceSelector{ StanceAxis::kIdle, "CarryRightArmBehavior", "MTRightArmState", {} },
            StanceSelector{ StanceAxis::kIdle, "1HM_Behavior", "1HM_Ready_State", "CombatIdles", true },
            StanceSelector{ StanceAxis::kIdle, "RootBehaviorGraph", "Default_State", "FullBodyPlayerIdles" },
            StanceSelector{ StanceAxis::kIdle, "RootBehaviorGraph", "Default_State", "StandingVampireFeed" },
            StanceSelector{ StanceAxis::kIdle, "Master_Behavior", "Root", "IdleDLC2TentacleWarpBook" },
        };


        struct SelectorVotes
        {
            bool off{ false };
            bool on{ false };

            [[nodiscard]] SelectorState GetState() const noexcept
            {
                if (off && on)
                {
                    return SelectorState::kMixed;
                }
                return on ? SelectorState::kOn : off ? SelectorState::kOff : SelectorState::kNone;
            }
        };

        template <class Visitor>
        void VisitStanceSelectors(RE::Actor& a_actor, Visitor&& a_visitor)
        {
            RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
            if (!a_actor.GetAnimationGraphManager(manager) || !manager)
            {
                return;
            }

            RE::BSSpinLockGuard lock{ manager->GetRuntimeData().updateLock };
            const auto* behavior = GetActiveBehavior(*manager);
            if (!behavior || !behavior->activeNodes)
            {
                return;
            }

            // An exact vtable match instead of an RTTI cast per active node: this read
            // runs several times per second for every reconciled actor.
            static const REL::Relocation<std::uintptr_t> stateMachineVTable{ RE::VTABLE_hkbStateMachine[0] };
            for (const auto& nodeInfo : *behavior->activeNodes)
            {
                const auto* node = nodeInfo.nodeClone ? nodeInfo.nodeClone : nodeInfo.nodeTemplate;
                if (!node || ReadAt<std::uintptr_t>(node, 0) != stateMachineVTable.address())
                {
                    continue;
                }
                const auto& machine = *static_cast<const RE::hkbStateMachine*>(node);
                const std::string_view machineName{ GetName(machine.name) };
                const char* state = nullptr;
                // A machine may select on more than one pair of states.
                for (const auto& selector : kStanceSelectors)
                {
                    if (selector.machine == machineName)
                    {
                        state = state ? state : GetCurrentStateName(machine);
                        a_visitor(selector, state);
                    }
                }
            }
        }
    }

    StanceSelectors ReadStanceSelectors(RE::Actor& a_actor)
    {
        PROFILE_FUNCTION();
        std::array<SelectorVotes, 5> votes{};
        bool drawnIdle = false;
        VisitStanceSelectors(a_actor, [&](const StanceSelector& a_selector, std::string_view a_state) {
            auto& axis = votes[static_cast<std::size_t>(a_selector.axis)];
            axis.off = axis.off || a_state == a_selector.offState;
            // GetCurrentStateName reads "?" for a state it cannot find.
            const bool on = a_selector.onState.empty() ? a_state != a_selector.offState && a_state != "?"
                                                       : a_state == a_selector.onState;
            axis.on = axis.on || on;
            drawnIdle = drawnIdle || (on && a_selector.drawn);
        });
        const auto& idle = votes[static_cast<std::size_t>(StanceAxis::kIdle)];
        return { .sneak = votes[static_cast<std::size_t>(StanceAxis::kSneak)].GetState(),
            .moving = votes[static_cast<std::size_t>(StanceAxis::kMoving)].GetState(),
            .sprinting = votes[static_cast<std::size_t>(StanceAxis::kSprint)].GetState(),
            .levitating = votes[static_cast<std::size_t>(StanceAxis::kLevitation)].GetState(),
            .idle = idle.on ? SelectorState::kOn : idle.off ? SelectorState::kOff : SelectorState::kNone,
            .drawnIdle = drawnIdle };
    }

    std::string DescribeStanceSelectors(RE::Actor& a_actor)
    {
        std::string description;
        VisitStanceSelectors(a_actor, [&](const StanceSelector& a_selector, std::string_view a_state) {
            std::format_to(std::back_inserter(description), "{}{}>{}", description.empty() ? "" : " ",
                a_selector.machine, a_state);
        });
        return description;
    }

    const char* GetMovingName(SelectorState a_selectors) noexcept
    {
        switch (a_selectors)
        {
            case SelectorState::kOff:
                return "standing";
            case SelectorState::kOn:
                return "moving";
            case SelectorState::kMixed:
                return "mixed";
            default:
                return "unknown";
        }
    }

    SneakReading ReadSneak(const RE::Actor& a_actor, SelectorState a_selectors)
    {
        switch (a_selectors)
        {
            case SelectorState::kOff:
                return { .sneak = Sneak::kNotSneaking, .selectors = a_selectors };
            case SelectorState::kOn:
                return { .sneak = Sneak::kSneaking, .selectors = a_selectors };
            case SelectorState::kMixed:
                // Selectors disagree while one stance's branch blends into another's.
                return { .sneak = Sneak::kUnknown, .selectors = a_selectors };
            case SelectorState::kNone:
                break;
        }

        // Fallback for graphs without the vanilla selectors. A sneak branch keeps
        // these set while it blends out after SneakStop.
        static const RE::BSFixedString isSneakingVariable{ "IsSneaking" };
        bool isSneaking = false;
        const bool isSneakingAvailable = a_actor.GetGraphVariableBool(isSneakingVariable, isSneaking);
        const auto stateTag = ReadStateTag(a_actor);
        if ((isSneakingAvailable && isSneaking) || stateTag == kSneakStateTag)
        {
            return { .sneak = Sneak::kSneaking };
        }
        return { .sneak = isSneakingAvailable && stateTag >= 0 ? Sneak::kNotSneaking : Sneak::kUnknown };
    }

    const char* GetSneakName(const SneakReading& a_reading) noexcept
    {
        switch (a_reading.selectors)
        {
            case SelectorState::kOff:
                return "notSneaking/states";
            case SelectorState::kOn:
                return "sneaking/states";
            case SelectorState::kMixed:
                return "mixed/states";
            default:
                break;
        }
        switch (a_reading.sneak)
        {
            case Sneak::kNotSneaking:
                return "notSneaking/variables";
            case Sneak::kSneaking:
                return "sneaking/variables";
            default:
                return "unknown";
        }
    }

    std::int32_t ReadStateTag(const RE::Actor& a_actor)
    {
        static const RE::BSFixedString stateVariable{ "iState" };
        std::int32_t value = -1;
        return a_actor.GetGraphVariableInt(stateVariable, value) ? value : -1;
    }
}
