#include "runtime/GameReverbHook.h"
#include "audio/AudioEngine.h"
#include "runtime/Profiling.h"

#include <cmath>
#include <cstdint>

namespace GameReverbHook
{
    namespace
    {
        PROFILE_HOOK_COUNTER(s_reverbProfile, "game reverb");

        float Level(std::int32_t a_millibels)
        {
            return std::pow(10.0F, static_cast<float>(a_millibels) / 2000.0F);
        }

        // BSAudio::ApplyReverbType, vfunc 7 of BSXAudio2Audio. BSAudioManager::SetReverb (ID 67711)
        // calls it on the caller's thread when an acoustic space activates or deactivates (deactivation
        // applies the default object kReverbType, vanilla's silent DefaultReverb) and for the underwater
        // reverb. The engine fades its reverb towards the type over a_fadeMilliseconds of GetTickCount.
        struct ApplyReverbTypeHook
        {
            static void Thunk(RE::BSAudio* a_audio, const RE::BSIReverbType* a_reverbType, std::uint32_t a_fadeMilliseconds)
            {
                PROFILE_HOOK(s_reverbProfile);
                if (a_reverbType)
                {
                    const Audio::RoomReverb reverb{ .room = Level(a_reverbType->DoGetRoomLevel()),
                        .roomHF = Level(a_reverbType->DoGetRoomHFLevel()),
                        .decaySeconds = a_reverbType->DoGetDecayTime(),
                        .decayHFRatio = a_reverbType->DoGetDecayHFRatio(),
                        .reflections = Level(a_reverbType->DoGetReflectionLevel()),
                        .reflectionsDelaySeconds = a_reverbType->DoGetReflectionDelay(),
                        .late = Level(a_reverbType->DoGetReverbLevel()),
                        .lateDelaySeconds = a_reverbType->DoGetReverbDelay(),
                        .diffusion = a_reverbType->DoGetDiffusion() / 100.0F,
                        .hfReferenceHertz = a_reverbType->DoGetHFReference() };
                    const auto fadeSeconds = static_cast<float>(a_fadeMilliseconds) / 1000.0F;
                    const bool finite = std::isfinite(reverb.decaySeconds) && std::isfinite(reverb.decayHFRatio)
                                     && std::isfinite(reverb.reflectionsDelaySeconds)
                                     && std::isfinite(reverb.lateDelaySeconds) && std::isfinite(reverb.diffusion)
                                     && std::isfinite(reverb.hfReferenceHertz);
                    if (finite)
                    {
                        logs::debug("Voice reverb follows the game: room={:.3f} decay={:.2f}s reflections={:.3f} "
                                    "late={:.3f} fade={}ms",
                            reverb.room, reverb.decaySeconds, reverb.reflections, reverb.late, a_fadeMilliseconds);
                        Audio::SetReverb(reverb, fadeSeconds);
                    }
                }
                PROFILE_HOOK_ORIGINAL(_original(a_audio, a_reverbType, a_fadeMilliseconds));
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };
    }

    void Install()
    {
        REL::Relocation<std::uintptr_t> audioVTable{ RE::VTABLE_BSXAudio2Audio[0] };
        ApplyReverbTypeHook::_original = audioVTable.write_vfunc(0x7, ApplyReverbTypeHook::Thunk);
        logs::debug("Installed game reverb hook");
    }
}
