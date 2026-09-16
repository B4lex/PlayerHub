#include "actor/CombatIsolation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"

#include <array>
#include <atomic>
#include <cstring>
#include <string_view>

namespace CombatIsolation
{
	namespace
	{
		using ActorConstants::kBlockedEventLogInterval;
		using ActorConstants::kInitialBlockedEventLogs;

		// Returns true when either side of a hostility or combat query is a
		// remote actor. A remote actor belongs to another player's world, so local actors never
		// treat it as an enemy and it never treats them as one.
		bool IsRemoteActorCombatQuery(
			const RE::Actor* a_actor,
			const RE::Actor* a_target,
			std::string_view a_path)
		{
			if (!ActorRegistry::IsRegistered(a_target) && !ActorRegistry::IsRegistered(a_actor)) {
				return false;
			}

			static std::atomic_uint32_t suppressedCount{ 0 };
			const auto count = ++suppressedCount;
			if (count <= kInitialBlockedEventLogs || count % kBlockedEventLogInterval == 0) {
				logs::info(
					"Suppressed local hostility involving remote puppet: path={} actor={:08X} target={:08X} count={}",
					a_path,
					a_actor ? a_actor->GetFormID() : 0,
					a_target ? a_target->GetFormID() : 0,
					count);
			}
			return true;
		}

		// Redirects a function's first instructions to a_thunk and returns the address
		// of a gateway that runs them and resumes the original function. The moved
		// instructions must be exactly a_prologue and position independent. Returns 0
		// without patching if the bytes differ, for example on another runtime or
		// when another plugin already patched the function.
		constexpr std::size_t kEntryDetourPrologueSize = 5;
		// jmp [rip+0] followed by the 64-bit resume address.
		constexpr std::size_t kAbsoluteJumpSize = 14;
		// The gateway plus the absolute jump write_branch<5> allocates.
		constexpr std::size_t kEntryDetourTrampolineSize =
			kEntryDetourPrologueSize + 2 * kAbsoluteJumpSize;
		constexpr std::size_t kCombatIsolationDetourCount = 3;
		static_assert(
			kCombatIsolationDetourCount * kEntryDetourTrampolineSize ==
			kTrampolineSize);

		std::uintptr_t WriteEntryDetour(
			REL::ID a_id,
			const std::array<std::uint8_t, kEntryDetourPrologueSize>& a_prologue,
			std::uintptr_t a_thunk,
			std::string_view a_name)
		{
			const auto target = REL::Relocation<std::uintptr_t>{ a_id }.address();
			if (std::memcmp(
					reinterpret_cast<const void*>(target),
					a_prologue.data(),
					a_prologue.size()) != 0) {
				logs::error(
					"Did not install {} hook: unexpected entry bytes at ID {}",
					a_name,
					a_id.id());
				return 0;
			}

			auto& trampoline = SKSE::GetTrampoline();
			auto* gateway = static_cast<std::uint8_t*>(
				trampoline.allocate(kEntryDetourPrologueSize + kAbsoluteJumpSize));
			std::memcpy(gateway, a_prologue.data(), a_prologue.size());
			const std::array<std::uint8_t, 6> jump{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
			const std::uintptr_t resume = target + kEntryDetourPrologueSize;
			std::memcpy(gateway + kEntryDetourPrologueSize, jump.data(), jump.size());
			std::memcpy(
				gateway + kEntryDetourPrologueSize + jump.size(),
				&resume,
				sizeof(resume));

			(void) trampoline.write_branch<5>(target, a_thunk);
			return reinterpret_cast<std::uintptr_t>(gateway);
		}

		// The engine's hostility decision (ID 37534 on 1.7.104): aggression and
		// faction reaction toward a target. Its wrappers (IDs 37532 and 37533) serve
		// combat group target handling, StartCombat, and many AI callers.
		struct HostilityHook
		{
			static bool Thunk(
				RE::Actor* a_actor,
				RE::Actor* a_target,
				std::int32_t a_aggression,
				std::uint8_t a_mode)
			{
				if (IsRemoteActorCombatQuery(a_actor, a_target, "Hostility")) {
					return false;
				}
				return _original(a_actor, a_target, a_aggression, a_mode);
			}

			static inline REL::Relocation<decltype(Thunk)> _original;
		};

		// Actor::IsHostileToActor (ID 37537). Besides the hostility decision it
		// also returns true from a faction-reaction path of its own.
		struct IsHostileToActorHook
		{
			static bool Thunk(RE::Actor* a_actor, RE::Actor* a_target)
			{
				if (IsRemoteActorCombatQuery(a_actor, a_target, "IsHostileToActor")) {
					return false;
				}
				return _original(a_actor, a_target);
			}

			static inline REL::Relocation<decltype(Thunk)> _original;
		};

		// Actor::StartCombat (ID 38561). Also reached without a hostility check, for
		// example when a remote actor's replicated swing hits a local actor.
		struct StartCombatHook
		{
			static bool Thunk(
				RE::Actor* a_actor,
				RE::Actor* a_target,
				RE::CombatGroup* a_combatGroup)
			{
				if (IsRemoteActorCombatQuery(a_actor, a_target, "StartCombat")) {
					return false;
				}
				return _original(a_actor, a_target, a_combatGroup);
			}

			static inline REL::Relocation<decltype(Thunk)> _original;
		};

	}

	void InstallHooks()
	{
		// The entry bytes are verified against 1.7.104 only.
		if (!REL::Module::IsAE()) {
			logs::warn(
				"Remote-puppet combat isolation hooks are verified only on Skyrim AE; local enemies may target puppets");
			return;
		}

		// mov [rsp+20h], r9b
		HostilityHook::_original = WriteEntryDetour(
			REL::ID(37534),
			{ 0x44, 0x88, 0x4C, 0x24, 0x20 },
			reinterpret_cast<std::uintptr_t>(&HostilityHook::Thunk),
			"hostility decision");
		// mov [rsp+10h], rbx
		IsHostileToActorHook::_original = WriteEntryDetour(
			REL::ID(37537),
			{ 0x48, 0x89, 0x5C, 0x24, 0x10 },
			reinterpret_cast<std::uintptr_t>(&IsHostileToActorHook::Thunk),
			"IsHostileToActor");
		// mov r11, rsp; push rbp; push rsi
		StartCombatHook::_original = WriteEntryDetour(
			REL::ID(38561),
			{ 0x4C, 0x8B, 0xDC, 0x55, 0x56 },
			reinterpret_cast<std::uintptr_t>(&StartCombatHook::Thunk),
			"StartCombat");
		logs::info(
			"Remote-puppet combat isolation hooks: hostility={} isHostileToActor={} startCombat={}",
			HostilityHook::_original.address() != 0,
			IsHostileToActorHook::_original.address() != 0,
			StartCombatHook::_original.address() != 0);
	}
}
