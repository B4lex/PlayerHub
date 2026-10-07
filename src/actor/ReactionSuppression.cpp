#include "actor/ReactionSuppression.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "runtime/CodeDetour.h"

#include <array>
#include <atomic>
#include <utility>

namespace ReactionSuppression
{
    namespace
    {
        using ActorConstants::ShouldLogOccurrence;
        using CodeDetour::EntryDetourTrampolineSize;
        using CodeDetour::WriteEntryDetour;

        std::uint32_t CountSuppression(std::atomic_uint32_t& a_count)
        {
            const auto count = ++a_count;
            return ShouldLogOccurrence(count) ? count : 0;
        }

        constexpr std::array<std::uint8_t, 7> kProcessResponsePrologue{
            0x48, 0x8B, 0xC4, // mov rax, rsp
            0x48, 0x89, 0x50, 0x10 // mov [rax+10h], rdx
        };
        constexpr std::array<std::uint8_t, 6> kIsFleeingPrologue{
            0x40, 0x53, // push rbx
            0x48, 0x83, 0xEC, 0x20 // sub rsp, 20h
        };
        constexpr std::array<std::uint8_t, 6> kSetDetectionStatePrologue{
            0x40, 0x53, // push rbx
            0x48, 0x83, 0xEC, 0x20 // sub rsp, 20h
        };

        static_assert(EntryDetourTrampolineSize(kProcessResponsePrologue.size())
                + EntryDetourTrampolineSize(kIsFleeingPrologue.size())
                + EntryDetourTrampolineSize(kSetDetectionStatePrologue.size())
            == kTrampolineSize);

        // Drops the reference the caller passed by value, as ID 39643 does before it
        // returns: the engine's DialogueItem destructor (ID 35221) frees the
        // responses that CommonLib's defaulted destructor would leak.
        void ReleaseDialogueItem(RE::DialogueItem* a_item)
        {
            if (!a_item || a_item->DecRef() != 0)
            {
                return;
            }

            using Destructor = void(RE::DialogueItem*);
            using SizedDelete = void(void*, std::size_t);
            static REL::Relocation<Destructor> destructor{ REL::ID(35221) };
            static REL::Relocation<SizedDelete> sizedDelete{ REL::ID(109588) };
            destructor(a_item);
            sizedDelete(a_item, sizeof(RE::DialogueItem));
        }

        // The AI process starting a dialogue item (ID 39643 on 1.7.104), reached
        // only from ID 39642 once it has chosen a topic response for the speaker.
        // Skyrim's own barks, such as hit and combat lines, start here. a_item
        // points at a by-value smart pointer that the callee releases.
        struct ProcessResponseHook
        {
            static void Thunk(RE::AIProcess* a_process, RE::DialogueItem** a_item, RE::Actor* a_speaker,
                RE::TESObjectREFR* a_listener, bool a_flag)
            {
                if (ActorRegistry::IsRegistered(a_speaker))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    if (const auto count = CountSuppression(suppressedCount))
                    {
                        const auto* item = a_item ? *a_item : nullptr;
                        const auto* info = item ? item->info : nullptr;
                        const auto* topic = item ? item->topic : nullptr;
                        logs::debug(
                            "Suppressed remote actor dialogue response: speaker={:08X} listener={:08X} "
                            "topic={:08X} info={:08X} count={}",
                            a_speaker->GetFormID(), a_listener ? a_listener->GetFormID() : 0,
                            topic ? topic->GetFormID() : 0, info ? info->GetFormID() : 0, count);
                    }
                    ReleaseDialogueItem(a_item ? std::exchange(*a_item, nullptr) : nullptr);
                    return;
                }
                _original(a_process, a_item, a_speaker, a_listener, a_flag);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // Actor::IsFleeing (ID 37577), queried from 22 call sites across combat and AI evaluation.
        struct IsFleeingHook
        {
            static bool Thunk(RE::Actor* a_actor)
            {
                const bool fleeing = _original(a_actor);
                if (fleeing && ActorRegistry::IsRegistered(a_actor))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    if (const auto count = CountSuppression(suppressedCount))
                    {
                        logs::debug(
                            "Suppressed remote actor fleeing: actor={:08X} count={}", a_actor->GetFormID(), count);
                    }
                    return false;
                }
                return fleeing;
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // ActorKnowledge::SetDetectionState (ID 42704): stores the owner's new
        // detection of its target and notifies the knowledge's listeners, which
        // drive detection dialogue, alarms, and combat searches. It takes its own
        // reference, so skipping it leaves the caller's reference intact.
        struct SetDetectionStateHook
        {
            static void Thunk(RE::ActorKnowledge* a_knowledge, RE::DetectionState* a_state)
            {
                if (a_knowledge && ActorRegistry::IsRegistered(a_knowledge->owner))
                {
                    static std::atomic_uint32_t suppressedCount{ 0 };
                    if (const auto count = CountSuppression(suppressedCount))
                    {
                        logs::debug("Suppressed remote actor detection: owner={:08X} target={:08X} count={}",
                            a_knowledge->owner.native_handle(), a_knowledge->target.native_handle(), count);
                    }
                    return;
                }
                _original(a_knowledge, a_state);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };
    }

    void InstallHooks()
    {
        // The entry bytes are verified against 1.7.104 only.
        if (!REL::Module::IsAE())
        {
            logs::warn("Remote actor reaction suppression hooks are verified only on Skyrim AE; remote actors may "
                       "speak barks and react to detection");
            return;
        }

        ProcessResponseHook::_original = WriteEntryDetour(REL::ID(39643), kProcessResponsePrologue,
            reinterpret_cast<std::uintptr_t>(&ProcessResponseHook::Thunk), "dialogue response");
        IsFleeingHook::_original = WriteEntryDetour(
            REL::ID(37577), kIsFleeingPrologue, reinterpret_cast<std::uintptr_t>(&IsFleeingHook::Thunk), "IsFleeing");
        SetDetectionStateHook::_original = WriteEntryDetour(REL::ID(42704), kSetDetectionStatePrologue,
            reinterpret_cast<std::uintptr_t>(&SetDetectionStateHook::Thunk), "detection state");
        logs::debug("Remote actor reaction suppression hooks: dialogueResponse={} isFleeing={} detectionState={}",
            ProcessResponseHook::_original.address() != 0, IsFleeingHook::_original.address() != 0,
            SetDetectionStateHook::_original.address() != 0);
    }
}
