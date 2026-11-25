#include "actor/ActorFace.h"
#include "actor/RemoteActor.h"
#include "runtime/CodeDetour.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ActorFace
{
    namespace
    {
        using CodeDetour::kCallDetourTrampolineSize;
        using CodeDetour::WriteCallDetour;
        using Keyframe = RE::BSFaceGenKeyframeMultiple;
        using Network::FaceWeights;

        static_assert(kCallDetourTrampolineSize == kTrampolineSize);

        // The head's morphs are built from the final keyframes (expression3, modifier3,
        // phoneme3). The face update (ID 26598, SE 25983) copies each input keyframe into its
        // final one only while one of the input's values is in (0, 1], so a final otherwise keeps
        // its last values. Its only caller, BSFaceGenNiNode's update (ID 26998, SE 26417), queues
        // the head's morph update when it returns true.
        struct WeightRange
        {
            Keyframe RE::BSFaceGenAnimationData::* keyframe;
            std::size_t first;
            std::size_t count;
        };

        constexpr std::array kWeightRanges{
            WeightRange{ &RE::BSFaceGenAnimationData::expression3, 0, Network::kFaceExpressionCount },
            WeightRange{ &RE::BSFaceGenAnimationData::modifier3, Network::kFaceFirstModifierWeight,
                Network::kFaceModifierCount },
            WeightRange{ &RE::BSFaceGenAnimationData::phoneme3, Network::kFaceFirstPhonemeWeight,
                Network::kFacePhonemeCount },
        };

        // Callers hold the face's lock.
        FaceWeights ReadFinals(const RE::BSFaceGenAnimationData& a_face)
        {
            FaceWeights result;
            for (const auto& range : kWeightRanges)
            {
                const auto& keyframe = a_face.*range.keyframe;
                for (std::size_t i = 0; i < range.count && keyframe.values && i < keyframe.count; ++i)
                {
                    result.values[range.first + i] = keyframe.values[i];
                }
            }
            return result;
        }

        void WriteFinals(RE::BSFaceGenAnimationData& a_face, const FaceWeights& a_weights)
        {
            for (const auto& range : kWeightRanges)
            {
                auto& keyframe = a_face.*range.keyframe;
                for (std::size_t i = 0; i < range.count && keyframe.values && i < keyframe.count; ++i)
                {
                    if (keyframe.values[i] != a_weights.values[range.first + i])
                    {
                        keyframe.values[i] = a_weights.values[range.first + i];
                        keyframe.isUpdated = false;
                    }
                }
            }
        }

        // The engine marks a value it has not set with FLT_MAX; it counts as 0 here.
        float ReadSetValue(float a_value)
        {
            return a_value > 0.0F && a_value <= 1.0F ? a_value : 0.0F;
        }

        FaceWeights Blend(const FaceWeights& a_own, const FaceWeights& a_target, float a_weight)
        {
            FaceWeights result;
            for (std::size_t i = 0; i < Network::kFaceWeightCount; ++i)
            {
                const float own = ReadSetValue(a_own.values[i]);
                const float target = i >= Network::kFaceFirstPhonemeWeight ? (std::max)(own, a_target.values[i])
                                                                           : a_target.values[i];
                result.values[i] = own + (target - own) * a_weight;
            }
            return result;
        }

        struct Slot
        {
            // Compared only: the actor's key in ActorRegistry, and its head's face data.
            const RE::IAnimationGraphManagerHolder* owner{ nullptr };
            const RE::BSFaceGenAnimationData* face{ nullptr };
            FaceWeights target;
            float weight{ 0.0F };
            bool releasing{ false };
            // Written by the hook: the head's own finals after its last update, which the next
            // one starts from, and the finals shown over them.
            FaceWeights own;
            FaceWeights shown;
            bool written{ false };
        };

        std::mutex s_mutex;
        std::vector<Slot> s_slots;
        // Lets the hook, which runs for every head, skip the lock while no face is set.
        std::atomic<std::size_t> s_slotCount{ 0 };

        std::vector<Slot>::iterator FindFaceLocked(const RE::BSFaceGenAnimationData* a_face)
        {
            return std::ranges::find(s_slots, a_face, &Slot::face);
        }

        std::vector<Slot>::iterator FindOwnerLocked(const RE::IAnimationGraphManagerHolder* a_owner)
        {
            return std::ranges::find(s_slots, a_owner, &Slot::owner);
        }

        void EraseLocked(std::vector<Slot>::iterator a_slot)
        {
            s_slots.erase(a_slot);
            s_slotCount.store(s_slots.size(), std::memory_order_release);
        }

        PROFILE_HOOK_COUNTER(s_faceUpdateProfile, "face update");

        struct FaceUpdateHook
        {
            static bool Thunk(RE::BSFaceGenAnimationData* a_face, float a_delta, bool a_blink)
            {
                if (!a_face || s_slotCount.load(std::memory_order_acquire) == 0)
                {
                    return _original(a_face, a_delta, a_blink);
                }
                PROFILE_HOOK(s_faceUpdateProfile);
                Slot slot;
                bool hasSlot = false;
                {
                    const std::scoped_lock lock{ s_mutex };
                    if (const auto found = FindFaceLocked(a_face); found != s_slots.end())
                    {
                        slot = *found;
                        hasSlot = true;
                    }
                }
                if (!hasSlot)
                {
                    return PROFILE_HOOK_ORIGINAL(_original(a_face, a_delta, a_blink));
                }

                // Recursive, so the update's own locking nests inside it.
                RE::BSSpinLockGuard faceLock{ a_face->lock };
                if (slot.written)
                {
                    WriteFinals(*a_face, slot.own);
                }
                bool changed = PROFILE_HOOK_ORIGINAL(_original(a_face, a_delta, a_blink));
                bool written = false;
                if (!slot.releasing)
                {
                    slot.own = ReadFinals(*a_face);
                    const auto blended = Blend(slot.own, slot.target, slot.weight);
                    WriteFinals(*a_face, blended);
                    changed = changed || !slot.written || blended.values != slot.shown.values;
                    slot.shown = blended;
                    written = true;
                }
                else
                {
                    changed = changed || slot.written;
                }

                const std::scoped_lock lock{ s_mutex };
                const auto found = FindFaceLocked(a_face);
                if (found != s_slots.end())
                {
                    found->own = slot.own;
                    found->shown = slot.shown;
                    found->written = written;
                    // A slot released after the copy above keeps its written finals for one more
                    // update to restore.
                    if (found->releasing && !written)
                    {
                        EraseLocked(found);
                    }
                }
                return changed;
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };
    }

    void InstallHooks()
    {
        FaceUpdateHook::_original = WriteCallDetour(REL::ID(RELOCATION_ID(26417, 26998)), 0xAD,
            reinterpret_cast<std::uintptr_t>(&FaceUpdateHook::Thunk), "face update");
        logs::debug("Remote face hook: update={}", FaceUpdateHook::_original.address() != 0);
    }

    std::optional<FaceWeights> ReadFace(RE::Actor& a_actor)
    {
        if (!GameThread::Require("ActorFace::ReadFace"))
        {
            return std::nullopt;
        }
        auto* face = a_actor.GetFaceGenAnimationData();
        if (!face)
        {
            return std::nullopt;
        }
        RE::BSSpinLockGuard lock{ face->lock };
        return ReadFinals(*face);
    }

    void SetFace(const RemoteActor& a_remote, RE::Actor& a_actor, const FaceWeights& a_weights, float a_weight)
    {
        if (!GameThread::Require("ActorFace::SetFace") || !a_remote.animationGraphHolder)
        {
            return;
        }
        const auto* face = a_actor.GetFaceGenAnimationData();
        if (!face)
        {
            ClearFace(a_remote);
            return;
        }
        const std::scoped_lock lock{ s_mutex };
        auto slot = FindOwnerLocked(a_remote.animationGraphHolder);
        if (slot == s_slots.end())
        {
            s_slots.push_back(Slot{ .owner = a_remote.animationGraphHolder, .face = face });
            slot = s_slots.end() - 1;
            s_slotCount.store(s_slots.size(), std::memory_order_release);
            logs::debug("Remote face weights shown: actor={:08X}", a_remote.handle.native_handle());
        }
        else if (slot->face != face)
        {
            // A rebuilt head: the old one and its finals are gone.
            slot->face = face;
            slot->written = false;
        }
        slot->target = a_weights;
        slot->weight = std::clamp(a_weight, 0.0F, 1.0F);
        slot->releasing = false;
    }

    void ClearFace(const RemoteActor& a_remote)
    {
        if (s_slotCount.load(std::memory_order_acquire) == 0 || !a_remote.animationGraphHolder)
        {
            return;
        }
        const std::scoped_lock lock{ s_mutex };
        const auto slot = FindOwnerLocked(a_remote.animationGraphHolder);
        if (slot == s_slots.end() || slot->releasing)
        {
            return;
        }
        logs::debug("Remote face weights released: actor={:08X} restore={}", a_remote.handle.native_handle(),
            slot->written);
        if (slot->written)
        {
            slot->releasing = true;
        }
        else
        {
            EraseLocked(slot);
        }
    }

    void ForgetFace(const RemoteActor& a_remote)
    {
        if (s_slotCount.load(std::memory_order_acquire) == 0 || !a_remote.animationGraphHolder)
        {
            return;
        }
        const std::scoped_lock lock{ s_mutex };
        if (const auto slot = FindOwnerLocked(a_remote.animationGraphHolder); slot != s_slots.end())
        {
            EraseLocked(slot);
        }
    }
}
