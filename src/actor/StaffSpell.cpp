#include "actor/StaffSpell.h"
#include "runtime/GameThread.h"

namespace StaffSpell
{
    namespace
    {
        constexpr int GetSlot(bool a_leftHand)
        {
            return a_leftHand ? RE::Actor::SlotTypes::kLeftHand : RE::Actor::SlotTypes::kRightHand;
        }

        bool IsHoldingStaff(const RE::Actor& a_actor, bool a_leftHand)
        {
            const auto* hand = a_actor.GetEquippedObject(a_leftHand);
            const auto* weapon = hand ? hand->As<RE::TESObjectWEAP>() : nullptr;
            return weapon && weapon->IsStaff();
        }
    }

    bool IsCastless(const RE::MagicItem* a_selected)
    {
        return !a_selected || a_selected->GetSpellType() == RE::MagicSystem::SpellType::kEnchantment;
    }

    RE::MagicItem* GetSelected(RE::Actor& a_actor, bool a_leftHand)
    {
        return a_actor.GetActorRuntimeData().selectedSpells[GetSlot(a_leftHand)];
    }

    // Equipping an enchanted weapon selects its enchantment for that hand (ID 38752,
    // through Actor::SelectSpell), and the hand's caster casts the selection. The
    // HasEquippedSpell condition (ID 21554 via ID 38767), which roots the hand's
    // cast idles, accepts any selection but a weapon enchantment, so a staff's
    // enchantment casts from the staff while a sword's does not.
    RE::MagicItem* GetSpell(RE::Actor& a_actor, bool a_leftHand)
    {
        if (!IsHoldingStaff(a_actor, a_leftHand))
        {
            return nullptr;
        }
        auto* selected = GetSelected(a_actor, a_leftHand);
        return IsCastless(selected) ? nullptr : selected;
    }

    void Select(RE::Actor& a_actor, bool a_leftHand, RE::MagicItem* a_spell)
    {
        if (!GameThread::Require("StaffSpell::Select"))
        {
            return;
        }
        // Actor::SelectSpell (ID 38768, SE 37819, matched by its code and its call to
        // the selection setter ID 38826, SE 37871): sets the hand's selection, and
        // both hands' for a spell whose equip slot needs both, and prepares the hand
        // caster's sounds for it (ID 34436). Null clears the selection.
        using func_t = void(RE::Actor*, int, RE::MagicItem*);
        static REL::Relocation<func_t> func{ RELOCATION_ID(37819, 38768) };
        func(std::addressof(a_actor), GetSlot(a_leftHand), a_spell);
    }
}
