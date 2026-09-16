#pragma once

#include <string_view>

namespace AnimationEvents
{
	constexpr bool IsWeaponTransitionTraceEvent(
		std::string_view a_eventName) noexcept
	{
		return a_eventName.find("Equip") != std::string_view::npos ||
			a_eventName.find("equip") != std::string_view::npos ||
			a_eventName.find("Weapon") != std::string_view::npos ||
			a_eventName.find("weapon") != std::string_view::npos;
	}

	static_assert(IsWeaponTransitionTraceEvent("BeginWeaponSheathe"));
	static_assert(IsWeaponTransitionTraceEvent("Unequip_OutMoving"));
	static_assert(IsWeaponTransitionTraceEvent("weaponDraw"));
	static_assert(!IsWeaponTransitionTraceEvent("FootLeft"));
	constexpr bool IsWeaponTransitionEndEvent(
		std::string_view a_eventName,
		bool a_weaponDrawn) noexcept
	{
		return a_weaponDrawn ?
			(a_eventName == "WeapEquip_Out" ||
				a_eventName == "WeapEquip_OutMoving") :
			(a_eventName == "Unequip_Out" ||
				a_eventName == "Unequip_OutMoving");
	}

	static_assert(!IsWeaponTransitionEndEvent("weaponDraw", true));
	static_assert(IsWeaponTransitionEndEvent("WeapEquip_OutMoving", true));
	static_assert(!IsWeaponTransitionEndEvent("weaponSheathe", false));
	static_assert(IsWeaponTransitionEndEvent("Unequip_OutMoving", false));
	static_assert(!IsWeaponTransitionEndEvent("AttackStart", true));
}
