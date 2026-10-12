#include "actor/JumpSound.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "runtime/CodeDetour.h"

#include <RE/Skyrim.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace JumpSound
{
    namespace
    {
        using ActorConstants::ShouldLogOccurrence;
        using CodeDetour::kCallDetourTrampolineSize;
        using CodeDetour::WriteCallDetour;

        static_assert(2 * kCallDetourTrampolineSize == kTrampolineSize);

        // SOMMono01800, the output model of vanilla NPC landing sounds on soft
        // ground: full volume up to the minimum distance, then the curve's five
        // points spread evenly to the maximum distance.
        constexpr float kMinimumDistance = 150.0F;
        constexpr float kMaximumDistance = 1800.0F;
        constexpr std::array<float, 5> kAttenuationCurve{ 1.0F, 0.5F, 0.2F, 0.05F, 0.0F };

        // Set on the thread that dispatches a remote actor's jump footstep, for the nested
        // impact-sound calls below.
        thread_local std::uint32_t t_remoteJumpActor = 0;
        thread_local std::optional<float> t_playVolume;

        float DistanceVolume(float a_distance)
        {
            if (a_distance <= kMinimumDistance)
            {
                return 1.0F;
            }
            if (a_distance >= kMaximumDistance)
            {
                return 0.0F;
            }
            const float curvePosition = (a_distance - kMinimumDistance) / (kMaximumDistance - kMinimumDistance)
                * static_cast<float>(kAttenuationCurve.size() - 1);
            const auto index = static_cast<std::size_t>(curvePosition);
            return std::lerp(
                kAttenuationCurve[index], kAttenuationCurve[index + 1], curvePosition - static_cast<float>(index));
        }

        bool IsJumpFootstep(const RE::BSFixedString& a_tag)
        {
            // Footstep records (FSTP) carry these tags for the behavior graph's
            // takeoff and landing events.
            static const RE::BSFixedString jumpUp{ "JumpUp" };
            static const RE::BSFixedString jumpDown{ "JumpDown" };
            return a_tag == jumpUp || a_tag == jumpDown;
        }

        bool AttenuatesWithDistance(RE::BGSSoundDescriptorForm* a_sound)
        {
            auto* descriptor = a_sound ? skyrim_cast<RE::BGSStandardSoundDef*>(a_sound->soundDescriptor) : nullptr;
            const auto* outputModel = descriptor ? descriptor->outputModel : nullptr;
            // Compound descriptors and sounds without an output model keep vanilla behavior.
            return !outputModel
                || outputModel->data.flags.all(RE::BGSSoundOutput::Data::Flag::kAttenuatesWithDistance);
        }

        void LogAttenuatedSound(
            std::uint32_t a_actorHandle, const RE::BGSSoundDescriptorForm& a_sound, float a_distance, float a_volume)
        {
            static std::atomic<std::uint32_t> s_count{ 0 };
            const auto count = ++s_count;
            if (!ShouldLogOccurrence(count))
            {
                return;
            }
            logs::debug("Attenuated remote actor jump sound: actor={:08X} sound={:08X} distance={:.0f} volume={:.2f} "
                        "count={}",
                a_actorHandle, a_sound.GetFormID(), a_distance, a_volume, count);
        }

        // The comments below name 1.7.104 IDs and offsets. Each SE ID was matched
        // against the 1.5.97 disassembly by its code and the functions it calls.

        // BGSImpactManager's footstep sink (ID 36209). It picks the impact for the
        // ground material and plays it at the actor's position.
        struct FootstepHook
        {
            static RE::BSEventNotifyControl Thunk(RE::BSTEventSink<RE::BGSFootstepEvent>* a_sink,
                const RE::BGSFootstepEvent* a_event, RE::BSTEventSource<RE::BGSFootstepEvent>* a_source)
            {
                if (!a_event || !IsJumpFootstep(a_event->tag) || !ActorRegistry::IsRegistered(a_event->actor))
                {
                    return _original(a_sink, a_event, a_source);
                }

                t_remoteJumpActor = a_event->actor.native_handle();
                const auto result = _original(a_sink, a_event, a_source);
                t_remoteJumpActor = 0;
                return result;
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // The footstep sink's call to PlayImpactDataSounds (ID 36212). For a
        // non-player actor it plays the impact's first sound through a shared
        // handle pool, at the position in a_data.
        struct ImpactSoundsHook
        {
            static constexpr std::ptrdiff_t kSEOffset = 0x29E;
            static constexpr std::ptrdiff_t kAEOffset = 0x53D;

            static bool Thunk(
                RE::BGSImpactManager* a_manager, RE::BGSImpactManager::ImpactSoundData& a_data, void* a_unused)
            {
                const auto actorHandle = t_remoteJumpActor;
                if (actorHandle == 0)
                {
                    return _original(a_manager, a_data, a_unused);
                }

                auto* sound = a_data.impactData
                    ? (a_data.playSound1 ? a_data.impactData->sound1 : a_data.impactData->sound2)
                    : nullptr;
                auto* audio = RE::BSAudioManager::QPlatformInstance();
                const auto* listener = audio ? audio->audioListener : nullptr;
                if (!listener || !sound || !a_data.position || !a_data.pool || AttenuatesWithDistance(sound))
                {
                    return _original(a_manager, a_data, a_unused);
                }

                const float distance = a_data.position->GetDistance(listener->listenerPosition);
                const float volume = DistanceVolume(distance);
                LogAttenuatedSound(actorHandle, *sound, distance, volume);
                if (volume <= 0.0F)
                {
                    return false;
                }

                t_playVolume = volume;
                const bool played = _original(a_manager, a_data, a_unused);
                t_playVolume.reset();
                return played;
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // The pooled impact sound's call to BSSoundHandle::Play (ID 36208). The
        // volume message is queued before the play message, so the sound never
        // starts at full volume.
        struct PooledSoundPlayHook
        {
            static constexpr std::ptrdiff_t kSEOffset = 0x17E;
            static constexpr std::ptrdiff_t kAEOffset = 0x182;

            static bool Thunk(RE::BSSoundHandle* a_handle)
            {
                if (t_playVolume && a_handle)
                {
                    (void)a_handle->SetVolume(*t_playVolume);
                }
                return _original(a_handle);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };
    }

    void InstallHooks()
    {
        // Without the volume hook a nearby jump would still play unattenuated, so only route
        // remote jumps once it is in place.
        PooledSoundPlayHook::_original = WriteCallDetour(REL::ID(RELOCATION_ID(35313, 36208)),
            REL::Relocate(PooledSoundPlayHook::kSEOffset, PooledSoundPlayHook::kAEOffset),
            reinterpret_cast<std::uintptr_t>(&PooledSoundPlayHook::Thunk), "pooled impact sound play");
        if (PooledSoundPlayHook::_original.address() != 0)
        {
            ImpactSoundsHook::_original = WriteCallDetour(REL::ID(RELOCATION_ID(35314, 36209)),
                REL::Relocate(ImpactSoundsHook::kSEOffset, ImpactSoundsHook::kAEOffset),
                reinterpret_cast<std::uintptr_t>(&ImpactSoundsHook::Thunk), "footstep impact sounds");
        }
        if (ImpactSoundsHook::_original.address() != 0)
        {
            REL::Relocation<std::uintptr_t> footstepSinkVTable{ RE::VTABLE_BGSImpactManager[0] };
            FootstepHook::_original = footstepSinkVTable.write_vfunc(0x1, FootstepHook::Thunk);
        }
        logs::debug("Remote actor jump sound hooks: pooledPlay={} impactSounds={} footstep={}",
            PooledSoundPlayHook::_original.address() != 0, ImpactSoundsHook::_original.address() != 0,
            FootstepHook::_original.address() != 0);
    }
}
