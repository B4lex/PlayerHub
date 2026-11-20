#include "audio/VoiceReverb.h"
#include "protocol/VoiceFrame.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Audio
{
    namespace
    {
        constexpr float kSampleRate = static_cast<float>(Multiplayer::kVoiceSampleRate);

        // Holds the longest I3DL2 reflections delay (0.3 s) plus the longest reverb delay (0.1 s) and
        // the early taps' spread.
        constexpr std::size_t kDelaySamples = 32768;
        constexpr std::size_t kDelayMask = kDelaySamples - 1;
        constexpr float kMaximumReflectionsDelaySeconds = 0.3F;
        constexpr float kMaximumLateDelaySeconds = 0.1F;

        struct EarlyTap
        {
            float offsetSeconds;
            float left;
            float right;
        };

        // Reflections alternate sides and fall off, so the first ones widen the voice before the tail.
        constexpr std::array<EarlyTap, 6> kEarlyTaps{ {
            { 0.0F, 1.0F, 0.4F },
            { 0.0043F, 0.34F, 0.86F },
            { 0.0091F, 0.74F, 0.3F },
            { 0.0137F, 0.26F, 0.64F },
            { 0.0199F, 0.55F, 0.22F },
            { 0.0263F, 0.19F, 0.47F },
        } };

        // Mutually prime lengths of 25 to 39 ms, so the lines' echoes do not pile up on one period.
        constexpr std::array<std::size_t, 8> kLineSamples{ 1201, 1291, 1373, 1471, 1559, 1663, 1777, 1867 };
        constexpr std::array<std::size_t, 2> kDiffuserSamples{ 541, 337 };
        constexpr std::array<float, 8> kInputSigns{ 1.0F, 1.0F, 1.0F, 1.0F, -1.0F, -1.0F, -1.0F, -1.0F };
        constexpr std::array<float, 8> kLeftSigns{ 1.0F, -1.0F, 1.0F, -1.0F, 1.0F, -1.0F, 1.0F, -1.0F };
        constexpr std::array<float, 8> kRightSigns{ 1.0F, 1.0F, -1.0F, -1.0F, 1.0F, 1.0F, -1.0F, -1.0F };
        // The input spreads over the lines and the first pass reaches each ear at half the input's energy.
        const float kLateInputScale = 1.0F / std::sqrt(8.0F);
        const float kLateOutputScale = std::sqrt(0.5F);
        const float kHadamardScale = 1.0F / std::sqrt(8.0F);
        constexpr float kMaximumDiffusion = 0.6F;
        constexpr float kMinimumDecaySeconds = 0.1F;
        constexpr float kMaximumDecaySeconds = 20.0F;
        constexpr float kSilentGain = 1e-5F;
        constexpr float kSilentSend = 1e-6F;

        float EarlyScale()
        {
            float sum = 0.0F;
            for (const auto& tap : kEarlyTaps)
            {
                sum += tap.left * tap.left;
            }
            return 1.0F / std::sqrt(sum);
        }
        // Each ear's reflections sum to unit energy.
        const float kEarlyScale = EarlyScale();

        // The coefficient b of the one-pole low-pass y = (1 - b) x + b y' whose power gain at the
        // frequency with cosine a_cosine is a_powerGain; unity at DC.
        float LowpassCoefficient(float a_powerGain, float a_cosine)
        {
            if (a_powerGain >= 0.9999F)
            {
                return 0.0F;
            }
            const auto gain = (std::max)(a_powerGain, 0.001F);
            const auto root = std::sqrt(
                (std::max)(0.0F, 2.0F * gain * (1.0F - a_cosine) - gain * gain * (1.0F - a_cosine * a_cosine)));
            return (1.0F - gain * a_cosine - root) / (1.0F - gain);
        }

        float DecayGain(std::size_t a_samples, float a_decaySeconds)
        {
            return std::pow(10.0F, -3.0F * static_cast<float>(a_samples) / (kSampleRate * a_decaySeconds));
        }

        RoomReverb Lerp(const RoomReverb& a_from, const RoomReverb& a_to, float a_t)
        {
            return RoomReverb{ .room = std::lerp(a_from.room, a_to.room, a_t),
                .roomHF = std::lerp(a_from.roomHF, a_to.roomHF, a_t),
                .decaySeconds = std::lerp(a_from.decaySeconds, a_to.decaySeconds, a_t),
                .decayHFRatio = std::lerp(a_from.decayHFRatio, a_to.decayHFRatio, a_t),
                .reflections = std::lerp(a_from.reflections, a_to.reflections, a_t),
                .reflectionsDelaySeconds = std::lerp(a_from.reflectionsDelaySeconds, a_to.reflectionsDelaySeconds, a_t),
                .late = std::lerp(a_from.late, a_to.late, a_t),
                .lateDelaySeconds = std::lerp(a_from.lateDelaySeconds, a_to.lateDelaySeconds, a_t),
                .diffusion = std::lerp(a_from.diffusion, a_to.diffusion, a_t),
                .hfReferenceHertz = std::lerp(a_from.hfReferenceHertz, a_to.hfReferenceHertz, a_t) };
        }

        // Unnormalized; the line gains carry the 1/sqrt(8) that makes it orthonormal.
        void Hadamard(std::array<float, 8>& a_values)
        {
            for (std::size_t width = 1; width < a_values.size(); width *= 2)
            {
                for (std::size_t start = 0; start < a_values.size(); start += width * 2)
                {
                    for (std::size_t i = start; i < start + width; ++i)
                    {
                        const auto a = a_values[i];
                        const auto b = a_values[i + width];
                        a_values[i] = a + b;
                        a_values[i + width] = a - b;
                    }
                }
            }
        }
    }

    VoiceReverb::VoiceReverb()
    {
        delay.assign(kDelaySamples, 0.0F);
        for (std::size_t i = 0; i < kLines; ++i)
        {
            lines[i].buffer.assign(kLineSamples[i], 0.0F);
        }
        for (std::size_t i = 0; i < kDiffusers; ++i)
        {
            diffusers[i].buffer.assign(kDiffuserSamples[i], 0.0F);
        }
        UpdateCoefficients();
    }

    void VoiceReverb::SetTarget(const RoomReverb& a_target, float a_fadeSeconds)
    {
        from = current;
        to = a_target;
        fadeSeconds = (std::max)(a_fadeSeconds, 0.0F);
        fadeElapsed = 0.0F;
        if (fadeSeconds <= 0.0F)
        {
            current = to;
        }
        dirty = true;
    }

    void VoiceReverb::Reset()
    {
        std::ranges::fill(delay, 0.0F);
        for (auto& line : lines)
        {
            std::ranges::fill(line.buffer, 0.0F);
            line.damping = 0.0F;
        }
        for (auto& diffuser : diffusers)
        {
            std::ranges::fill(diffuser.buffer, 0.0F);
        }
        inputLowpass = 0.0F;
        idleSamples = 0;
        idle = true;
    }

    void VoiceReverb::Advance(std::size_t a_frames)
    {
        if (fadeElapsed < fadeSeconds)
        {
            fadeElapsed += static_cast<float>(a_frames) / kSampleRate;
            current = Lerp(from, to, (std::min)(1.0F, fadeElapsed / fadeSeconds));
            dirty = true;
        }
        if (dirty)
        {
            dirty = false;
            UpdateCoefficients();
        }
    }

    void VoiceReverb::UpdateCoefficients()
    {
        const auto hfReference = std::clamp(current.hfReferenceHertz, 20.0F, 20000.0F);
        const auto cosine = std::cos(2.0F * std::numbers::pi_v<float> * hfReference / kSampleRate);
        const auto roomHF = std::clamp(current.roomHF, 0.0F, 1.0F);
        inputCoefficient = LowpassCoefficient(roomHF * roomHF, cosine);

        earlyGain = (std::max)(0.0F, current.room * current.reflections) * kEarlyScale;
        lateGain = (std::max)(0.0F, current.room * current.late) * kLateOutputScale;
        earlyDelay = std::clamp(current.reflectionsDelaySeconds, 0.0F, kMaximumReflectionsDelaySeconds) * kSampleRate;
        lateDelay
            = earlyDelay + std::clamp(current.lateDelaySeconds, 0.0F, kMaximumLateDelaySeconds) * kSampleRate;
        diffuserCoefficient = kMaximumDiffusion * std::clamp(current.diffusion, 0.0F, 1.0F);

        const auto decay = std::clamp(current.decaySeconds, kMinimumDecaySeconds, kMaximumDecaySeconds);
        // A ratio above 1 would brighten the tail as it decays; the lines leave highs undamped instead.
        const auto ratio = std::clamp(current.decayHFRatio, 0.1F, 1.0F);
        for (std::size_t i = 0; i < kLines; ++i)
        {
            auto& line = lines[i];
            const auto gain = DecayGain(kLineSamples[i], decay);
            const auto highGain = DecayGain(kLineSamples[i], decay * ratio) / gain;
            line.gain = gain * kHadamardScale;
            line.dampingCoefficient = LowpassCoefficient(highGain * highGain, cosine);
        }
        tailSamples = static_cast<std::size_t>(lateDelay + (kEarlyTaps.back().offsetSeconds + decay) * kSampleRate)
                    + kLineSamples.back();
    }

    float VoiceReverb::ReadDelay(float a_samples) const
    {
        const auto whole = static_cast<std::size_t>(a_samples);
        const auto fraction = a_samples - static_cast<float>(whole);
        const auto newer = (delayPosition + kDelaySamples - whole) & kDelayMask;
        const auto older = (newer + kDelayMask) & kDelayMask;
        return std::lerp(delay[newer], delay[older], fraction);
    }

    bool VoiceReverb::IsSilent() const
    {
        return earlyGain < kSilentGain && lateGain < kSilentGain && fadeElapsed >= fadeSeconds;
    }

    void VoiceReverb::Process(const float* a_send, float* a_stereo, std::size_t a_frames)
    {
        Advance(a_frames);
        if (IsSilent())
        {
            if (!idle)
            {
                Reset();
            }
            return;
        }
        const bool sounding
            = std::any_of(a_send, a_send + a_frames, [](float a_sample) { return std::abs(a_sample) > kSilentSend; });
        if (sounding)
        {
            idleSamples = 0;
            idle = false;
        }
        else if (!idle)
        {
            idleSamples += a_frames;
            if (idleSamples > tailSamples)
            {
                // The tail has decayed past 60 dB.
                Reset();
            }
        }
        if (idle)
        {
            return;
        }

        std::array<float, kEarlyTaps.size()> earlyDelays{};
        for (std::size_t t = 0; t < kEarlyTaps.size(); ++t)
        {
            earlyDelays[t] = earlyDelay + kEarlyTaps[t].offsetSeconds * kSampleRate;
        }
        std::array<float, kLines> outputs{};
        std::array<float, kLines> feedback{};
        for (std::size_t i = 0; i < a_frames; ++i)
        {
            const auto input = a_send[i];
            inputLowpass = input + inputCoefficient * (inputLowpass - input);
            delay[delayPosition] = inputLowpass;

            float earlyLeft = 0.0F;
            float earlyRight = 0.0F;
            for (std::size_t t = 0; t < kEarlyTaps.size(); ++t)
            {
                const auto sample = ReadDelay(earlyDelays[t]);
                earlyLeft += sample * kEarlyTaps[t].left;
                earlyRight += sample * kEarlyTaps[t].right;
            }

            auto late = ReadDelay(lateDelay);
            for (auto& diffuser : diffusers)
            {
                const auto buffered = diffuser.buffer[diffuser.position];
                const auto output = buffered - diffuserCoefficient * late;
                diffuser.buffer[diffuser.position] = late + diffuserCoefficient * output;
                diffuser.position = diffuser.position + 1 == diffuser.buffer.size() ? 0 : diffuser.position + 1;
                late = output;
            }

            for (std::size_t l = 0; l < kLines; ++l)
            {
                auto& line = lines[l];
                outputs[l] = line.buffer[line.position];
                line.damping = outputs[l] + line.dampingCoefficient * (line.damping - outputs[l]);
                feedback[l] = line.damping * line.gain;
            }
            Hadamard(feedback);
            float lateLeft = 0.0F;
            float lateRight = 0.0F;
            for (std::size_t l = 0; l < kLines; ++l)
            {
                auto& line = lines[l];
                line.buffer[line.position] = feedback[l] + late * kInputSigns[l] * kLateInputScale;
                line.position = line.position + 1 == line.buffer.size() ? 0 : line.position + 1;
                lateLeft += outputs[l] * kLeftSigns[l];
                lateRight += outputs[l] * kRightSigns[l];
            }

            a_stereo[i * 2] += earlyGain * earlyLeft + lateGain * lateLeft;
            a_stereo[i * 2 + 1] += earlyGain * earlyRight + lateGain * lateRight;
            delayPosition = (delayPosition + 1) & kDelayMask;
        }
    }
}
