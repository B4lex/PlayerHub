#pragma once

#include "protocol/VoiceFrame.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

struct OpusEncoder;
struct DenoiseState;

namespace Audio
{
    // Turns the microphone's 48 kHz mono samples into Opus frames while the player transmits.
    // Process runs on the capture device's thread; the setters on any thread.
    class VoiceEncoder
    {
    public:
        using Emit = void (*)(std::uint16_t a_sequence, bool a_end, std::span<const std::uint8_t> a_opus);

        VoiceEncoder();
        ~VoiceEncoder();
        VoiceEncoder(const VoiceEncoder&) = delete;
        VoiceEncoder& operator=(const VoiceEncoder&) = delete;

        void SetEmit(Emit a_emit) { emit.store(a_emit, std::memory_order_release); }
        void SetGain(float a_gain) { gain.store(a_gain, std::memory_order_relaxed); }
        void SetNoiseSuppression(bool a_enabled) { noiseSuppression.store(a_enabled, std::memory_order_relaxed); }
        void SetVoiceActivation(bool a_enabled, float a_level)
        {
            voiceActivation.store(a_enabled, std::memory_order_relaxed);
            activationLevel.store(a_level, std::memory_order_relaxed);
        }

        void Process(const float* a_samples, std::size_t a_count, bool a_allowed, bool a_pushToTalk);

        // Ends a talk spurt and forgets partial frames, as when the device stops.
        void Reset();

        // The input level on the meter's scale, 0 at -60 dBFS and 1 at full scale, after gain and
        // noise suppression.
        [[nodiscard]] float GetLevel() const { return level.load(std::memory_order_relaxed); }
        [[nodiscard]] bool IsTransmitting() const { return transmittingShown.load(std::memory_order_relaxed); }

    private:
        static constexpr std::size_t kChunkSamples = 480;

        void ProcessChunk(bool a_allowed, bool a_pushToTalk);
        void ProcessFrame(bool a_allowed, bool a_pushToTalk);
        void Encode(std::span<const float> a_frame, bool a_end);

        std::atomic<Emit> emit{ nullptr };
        std::atomic<float> gain{ 1.0F };
        std::atomic_bool noiseSuppression{ true };
        std::atomic_bool voiceActivation{ false };
        std::atomic<float> activationLevel{ 0.35F };
        std::atomic<float> level{ 0.0F };
        std::atomic_bool transmittingShown{ false };

        OpusEncoder* encoder{ nullptr };
        DenoiseState* denoiser{ nullptr };

        std::array<float, kChunkSamples> chunk{};
        std::size_t chunkFill{ 0 };
        std::array<float, Multiplayer::kVoiceFrameSamples> frame{};
        std::array<float, Multiplayer::kVoiceFrameSamples> previous{};
        bool hasPrevious{ false };
        std::size_t frameFill{ 0 };

        float meter{ 0.0F };
        // Samples left of the hangover after voice or the push-to-talk key ended.
        std::size_t activationHold{ 0 };
        std::size_t pushToTalkHold{ 0 };
        bool transmitting{ false };
        std::uint16_t sequence{ 0 };
    };
}
