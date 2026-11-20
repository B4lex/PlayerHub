#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace Audio
{
    // The game's current reverb in I3DL2 terms, with levels as linear amplitudes.
    struct RoomReverb
    {
        // The wet signal's level; 0 is no reverb.
        float room{ 0.0F };
        // The wet signal's level at hfReferenceHertz, relative to room.
        float roomHF{ 1.0F };
        float decaySeconds{ 1.0F };
        float decayHFRatio{ 1.0F };
        // Relative to room.
        float reflections{ 0.0F };
        float reflectionsDelaySeconds{ 0.0F };
        // Relative to room.
        float late{ 0.0F };
        // After the reflections.
        float lateDelaySeconds{ 0.0F };
        // 0 to 1.
        float diffusion{ 1.0F };
        float hfReferenceHertz{ 5000.0F };

        bool operator==(const RoomReverb&) const = default;
    };

    // One reverb shared by every voice: early reflections from a tapped delay, then a diffused
    // eight-line feedback delay network for the tail. Playback thread only.
    class VoiceReverb
    {
    public:
        VoiceReverb();

        void SetTarget(const RoomReverb& a_target, float a_fadeSeconds);

        // Drops the tail.
        void Reset();

        // Adds the wet signal of the mono send to the interleaved stereo mix.
        void Process(const float* a_send, float* a_stereo, std::size_t a_frames);

    private:
        static constexpr std::size_t kLines = 8;
        static constexpr std::size_t kDiffusers = 2;

        struct Line
        {
            std::vector<float> buffer;
            std::size_t position{ 0 };
            float gain{ 0.0F };
            float dampingCoefficient{ 0.0F };
            float damping{ 0.0F };
        };

        struct Diffuser
        {
            std::vector<float> buffer;
            std::size_t position{ 0 };
        };

        void Advance(std::size_t a_frames);
        void UpdateCoefficients();
        [[nodiscard]] float ReadDelay(float a_samples) const;
        [[nodiscard]] bool IsSilent() const;

        RoomReverb from;
        RoomReverb to;
        RoomReverb current;
        float fadeSeconds{ 0.0F };
        float fadeElapsed{ 0.0F };
        bool dirty{ true };

        std::vector<float> delay;
        std::size_t delayPosition{ 0 };
        std::array<Line, kLines> lines;
        std::array<Diffuser, kDiffusers> diffusers;
        float inputLowpass{ 0.0F };
        // Samples since the send last carried sound; past the tail's length there is nothing to render.
        std::size_t idleSamples{ 0 };
        bool idle{ true };

        float inputCoefficient{ 0.0F };
        float earlyGain{ 0.0F };
        float earlyDelay{ 0.0F };
        float lateGain{ 0.0F };
        float lateDelay{ 0.0F };
        float diffuserCoefficient{ 0.0F };
        std::size_t tailSamples{ 0 };
    };
}
