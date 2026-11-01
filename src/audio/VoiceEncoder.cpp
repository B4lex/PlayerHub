#include "audio/VoiceEncoder.h"

#include <opus/opus.h>
#include <rnnoise.h>

#include <algorithm>
#include <cmath>

namespace Audio
{
    namespace
    {
        // Wideband speech at a fraction of a checkpoint stream; in-band FEC lets a receiver rebuild
        // a lost frame from the next one.
        constexpr opus_int32 kBitrate = 24000;
        constexpr int kComplexity = 8;
        constexpr int kExpectedLossPercent = 10;

        constexpr float kMeterFloorDecibels = -60.0F;
        constexpr float kMeterFallPerChunk = 0.04F;
        // RNNoise's voice probability above which a loud enough chunk counts as speech.
        constexpr float kVoiceProbability = 0.5F;
        // RNNoise works on 16-bit sample values.
        constexpr float kRNNoiseScale = 32768.0F;

        constexpr std::size_t kActivationHoldSamples = Multiplayer::kVoiceSampleRate * 2 / 5;
        constexpr std::size_t kPushToTalkHoldSamples = Multiplayer::kVoiceSampleRate * 3 / 20;

        float ToMeter(float a_rms)
        {
            const auto decibels = 20.0F * std::log10((std::max)(a_rms, 1e-6F));
            return std::clamp((decibels - kMeterFloorDecibels) / -kMeterFloorDecibels, 0.0F, 1.0F);
        }
    }

    VoiceEncoder::VoiceEncoder()
    {
        int error = OPUS_OK;
        encoder = opus_encoder_create(static_cast<opus_int32>(Multiplayer::kVoiceSampleRate), 1, OPUS_APPLICATION_VOIP,
            &error);
        if (error != OPUS_OK || !encoder)
        {
            logs::error("Voice could not create the Opus encoder: {}", opus_strerror(error));
            encoder = nullptr;
        }
        else
        {
            opus_encoder_ctl(encoder, OPUS_SET_BITRATE(kBitrate));
            opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
            opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(kComplexity));
            opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
            opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
            opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(kExpectedLossPercent));
            // The gate below stops sending in pauses; DTX would leave gaps a receiver takes for loss.
            opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
        }
        denoiser = rnnoise_create(nullptr);
        if (!denoiser)
        {
            logs::error("Voice could not create the noise suppressor; the microphone is sent as it is");
        }
    }

    VoiceEncoder::~VoiceEncoder()
    {
        if (encoder)
        {
            opus_encoder_destroy(encoder);
        }
        if (denoiser)
        {
            rnnoise_destroy(denoiser);
        }
    }

    void VoiceEncoder::Process(const float* a_samples, std::size_t a_count, bool a_allowed, bool a_pushToTalk)
    {
        while (a_count > 0)
        {
            const auto taken = (std::min)(a_count, kChunkSamples - chunkFill);
            std::copy_n(a_samples, taken, chunk.begin() + static_cast<std::ptrdiff_t>(chunkFill));
            chunkFill += taken;
            a_samples += taken;
            a_count -= taken;
            if (chunkFill == kChunkSamples)
            {
                ProcessChunk(a_allowed, a_pushToTalk);
                chunkFill = 0;
            }
        }
    }

    void VoiceEncoder::Reset()
    {
        if (transmitting && frameFill > 0)
        {
            std::fill(frame.begin() + static_cast<std::ptrdiff_t>(frameFill), frame.end(), 0.0F);
            Encode(frame, true);
        }
        transmitting = false;
        transmittingShown.store(false, std::memory_order_relaxed);
        chunkFill = 0;
        frameFill = 0;
        hasPrevious = false;
        activationHold = 0;
        pushToTalkHold = 0;
        meter = 0.0F;
        level.store(0.0F, std::memory_order_relaxed);
    }

    void VoiceEncoder::ProcessChunk(bool a_allowed, bool a_pushToTalk)
    {
        const auto chunkGain = gain.load(std::memory_order_relaxed);
        for (auto& sample : chunk)
        {
            sample *= chunkGain;
        }
        float voiceProbability = 1.0F;
        const bool suppress = denoiser && noiseSuppression.load(std::memory_order_relaxed);
        if (suppress)
        {
            std::array<float, kChunkSamples> scaled;
            for (std::size_t i = 0; i < kChunkSamples; ++i)
            {
                scaled[i] = chunk[i] * kRNNoiseScale;
            }
            voiceProbability = rnnoise_process_frame(denoiser, scaled.data(), scaled.data());
            for (std::size_t i = 0; i < kChunkSamples; ++i)
            {
                chunk[i] = scaled[i] / kRNNoiseScale;
            }
        }

        float sum = 0.0F;
        for (const auto sample : chunk)
        {
            sum += sample * sample;
        }
        const auto chunkMeter = ToMeter(std::sqrt(sum / static_cast<float>(kChunkSamples)));
        meter = (std::max)(chunkMeter, meter - kMeterFallPerChunk);
        level.store(meter, std::memory_order_relaxed);

        if (chunkMeter >= activationLevel.load(std::memory_order_relaxed)
            && (!suppress || voiceProbability >= kVoiceProbability))
        {
            activationHold = kActivationHoldSamples;
        }
        else
        {
            activationHold -= (std::min)(activationHold, kChunkSamples);
        }
        if (a_pushToTalk)
        {
            pushToTalkHold = kPushToTalkHoldSamples;
        }
        else
        {
            pushToTalkHold -= (std::min)(pushToTalkHold, kChunkSamples);
        }

        std::copy(chunk.begin(), chunk.end(), frame.begin() + static_cast<std::ptrdiff_t>(frameFill));
        frameFill += kChunkSamples;
        if (frameFill == frame.size())
        {
            ProcessFrame(a_allowed, a_pushToTalk);
            frameFill = 0;
        }
    }

    void VoiceEncoder::ProcessFrame(bool a_allowed, bool a_pushToTalk)
    {
        const bool activation = voiceActivation.load(std::memory_order_relaxed);
        const bool wanted = a_allowed
                         && (activation ? activationHold > 0 : (a_pushToTalk || pushToTalkHold > 0));
        if (wanted)
        {
            if (!transmitting)
            {
                transmitting = true;
                if (encoder)
                {
                    opus_encoder_ctl(encoder, OPUS_RESET_STATE);
                }
                // Activation opens on the first loud chunk, so the frame before it carries the onset.
                if (activation && hasPrevious)
                {
                    Encode(previous, false);
                }
            }
            Encode(frame, false);
        }
        else if (transmitting)
        {
            transmitting = false;
            Encode(frame, true);
        }
        transmittingShown.store(transmitting, std::memory_order_relaxed);
        previous = frame;
        hasPrevious = true;
    }

    void VoiceEncoder::Encode(std::span<const float> a_frame, bool a_end)
    {
        const auto target = emit.load(std::memory_order_acquire);
        if (!encoder || !target)
        {
            return;
        }
        std::array<std::uint8_t, Multiplayer::kMaximumVoiceFrameBytes> packet;
        const auto size = opus_encode_float(encoder, a_frame.data(), static_cast<int>(a_frame.size()), packet.data(),
            static_cast<opus_int32>(packet.size()));
        if (size <= 0)
        {
            return;
        }
        target(sequence++, a_end, std::span{ packet.data(), static_cast<std::size_t>(size) });
    }
}
