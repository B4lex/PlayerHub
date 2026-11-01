#include "audio/VoiceMixer.h"

#include <opus/opus.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numbers>

namespace Audio
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr std::size_t kRingFrames = 16;
        constexpr float kFrameSeconds = static_cast<float>(Multiplayer::kVoiceFrameSamples) / Multiplayer::kVoiceSampleRate;
        // Frames held before a talk spurt starts playing: two absorb ordinary jitter, and a jittery
        // link earns up to six (120 ms).
        constexpr int kMinimumDepth = 2;
        constexpr int kMaximumDepth = 6;
        // Concealed frames in a row before a speaker is taken as silent, 100 ms.
        constexpr int kMaximumConcealed = 5;
        // Speakers decoded at once; the farthest beyond it are left out, which bounds the playback
        // thread's work in a crowd.
        constexpr std::size_t kMaximumChannels = 16;
        constexpr std::size_t kBlockFrames = 1024;

        constexpr float kOpenCutoffHertz = 20000.0F;
        constexpr float kOccludedCutoffHertz = 900.0F;
        constexpr float kOccludedGain = 0.55F;
        // Smoothing towards the game thread's parameters per sample, about 20 ms, so a step
        // does not click.
        constexpr float kParameterSmoothing = 1.0F / 960.0F;
        constexpr float kLevelFloorDecibels = -50.0F;
        constexpr float kLevelRangeDecibels = 40.0F;
        constexpr float kLevelRelease = 0.75F;
        constexpr float kSilentLevel = 0.01F;
        constexpr float kLimiterKnee = 0.9F;

        float LowpassCoefficient(float a_occlusion)
        {
            const auto cutoff = std::lerp(kOpenCutoffHertz, kOccludedCutoffHertz, std::clamp(a_occlusion, 0.0F, 1.0F));
            return 1.0F - std::exp(-2.0F * std::numbers::pi_v<float> * cutoff / Multiplayer::kVoiceSampleRate);
        }

        float Limit(float a_sample)
        {
            const auto magnitude = std::abs(a_sample);
            if (magnitude <= kLimiterKnee)
            {
                return a_sample;
            }
            const auto limited
                = kLimiterKnee + (1.0F - kLimiterKnee) * std::tanh((magnitude - kLimiterKnee) / (1.0F - kLimiterKnee));
            return std::copysign(limited, a_sample);
        }

        std::int16_t Distance(std::uint16_t a_from, std::uint16_t a_to)
        {
            return static_cast<std::int16_t>(static_cast<std::uint16_t>(a_to - a_from));
        }
    }

    struct VoiceMixer::Channel
    {
        struct Packet
        {
            bool present{ false };
            bool end{ false };
            std::uint16_t sequence{ 0 };
            std::uint16_t size{ 0 };
            std::array<std::uint8_t, Multiplayer::kMaximumVoiceFrameBytes> data{};
        };

        explicit Channel(Multiplayer::PeerSlot a_slot) :
            slot(a_slot)
        {
            int error = OPUS_OK;
            decoder = opus_decoder_create(static_cast<opus_int32>(Multiplayer::kVoiceSampleRate), 1, &error);
            if (error != OPUS_OK)
            {
                decoder = nullptr;
            }
        }

        ~Channel()
        {
            if (decoder)
            {
                opus_decoder_destroy(decoder);
            }
        }

        Channel(const Channel&) = delete;
        Channel& operator=(const Channel&) = delete;

        [[nodiscard]] int CountBuffered() const
        {
            int count = 0;
            for (const auto& packet : ring)
            {
                count += packet.present && Distance(next, packet.sequence) >= 0 ? 1 : 0;
            }
            return count;
        }

        void ClearRing()
        {
            for (auto& packet : ring)
            {
                packet.present = false;
            }
        }

        void Insert(const Multiplayer::VoiceFrame& a_frame, Clock::time_point a_now)
        {
            if (received)
            {
                const auto interval = std::chrono::duration<float>(a_now - lastArrival).count();
                // Only within a talk spurt; a pause between spurts is no jitter.
                if (interval < 0.5F)
                {
                    jitter += (std::abs(interval - kFrameSeconds) - jitter) / 16.0F;
                }
            }
            received = true;
            lastArrival = a_now;

            if (playing)
            {
                const auto ahead = Distance(next, a_frame.sequence);
                if (ahead < 0)
                {
                    return;
                }
                if (ahead >= static_cast<int>(kRingFrames))
                {
                    // Far ahead of the playout: the speaker restarted or the link stalled.
                    playing = false;
                    ClearRing();
                    next = a_frame.sequence;
                }
            }
            else
            {
                const auto ahead = Distance(next, a_frame.sequence);
                if (CountBuffered() == 0)
                {
                    // A new talk spurt. Spurts number their frames on from the last, so only
                    // the end flag or a pause tells them apart.
                    ClearRing();
                    if (decoder)
                    {
                        opus_decoder_ctl(decoder, OPUS_RESET_STATE);
                    }
                    next = a_frame.sequence;
                }
                else if (ahead <= -static_cast<int>(kRingFrames))
                {
                    return;
                }
                else if (ahead < 0)
                {
                    // An earlier frame of a spurt that has not started playing.
                    next = a_frame.sequence;
                }
                else if (ahead >= static_cast<int>(kRingFrames))
                {
                    ClearRing();
                    next = a_frame.sequence;
                }
            }

            auto& packet = ring[a_frame.sequence % kRingFrames];
            packet.present = true;
            packet.end = a_frame.end;
            packet.sequence = a_frame.sequence;
            packet.size = a_frame.size;
            std::copy_n(a_frame.opus.begin(), a_frame.size, packet.data.begin());

            const auto depth
                = std::clamp(kMinimumDepth + static_cast<int>(jitter / kFrameSeconds), kMinimumDepth, kMaximumDepth);
            if (!playing && (CountBuffered() >= depth || a_frame.end))
            {
                playing = true;
                concealed = 0;
            }
        }

        // Fills pcm with the next frame, or returns false while there is none to play.
        bool DecodeNext()
        {
            if (!playing || !decoder)
            {
                return false;
            }
            auto& packet = ring[next % kRingFrames];
            int decoded = 0;
            bool end = false;
            if (packet.present && packet.sequence == next)
            {
                decoded = opus_decode_float(decoder, packet.data.data(), packet.size, pcm.data(),
                    static_cast<int>(pcm.size()), 0);
                end = packet.end;
                packet.present = false;
                concealed = 0;
            }
            else
            {
                if (++concealed > kMaximumConcealed)
                {
                    playing = false;
                    return false;
                }
                const auto& following = ring[static_cast<std::uint16_t>(next + 1) % kRingFrames];
                if (following.present && following.sequence == static_cast<std::uint16_t>(next + 1))
                {
                    // The next frame's in-band FEC carries a coarse copy of this one.
                    decoded = opus_decode_float(decoder, following.data.data(), following.size, pcm.data(),
                        static_cast<int>(pcm.size()), 1);
                }
                else
                {
                    decoded = opus_decode_float(decoder, nullptr, 0, pcm.data(), static_cast<int>(pcm.size()), 0);
                }
            }
            if (decoded <= 0)
            {
                pcm.fill(0.0F);
            }
            ++next;
            read = 0;
            if (end)
            {
                playing = false;
            }

            float sum = 0.0F;
            for (const auto sample : pcm)
            {
                sum += sample * sample;
            }
            const auto decibels
                = 10.0F * std::log10((std::max)(sum / static_cast<float>(pcm.size()), 1e-10F));
            const auto frameLevel = std::clamp((decibels - kLevelFloorDecibels) / kLevelRangeDecibels, 0.0F, 1.0F);
            level = (std::max)(frameLevel, level * kLevelRelease);
            return true;
        }

        void Release()
        {
            level *= kLevelRelease;
            if (level < kSilentLevel)
            {
                level = 0.0F;
            }
        }

        Multiplayer::PeerSlot slot;
        OpusDecoder* decoder{ nullptr };
        std::array<Packet, kRingFrames> ring{};
        std::uint16_t next{ 0 };
        bool playing{ false };
        int concealed{ 0 };
        bool received{ false };
        Clock::time_point lastArrival{};
        float jitter{ 0.0F };

        std::array<float, Multiplayer::kVoiceFrameSamples> pcm{};
        std::size_t read{ Multiplayer::kVoiceFrameSamples };
        float level{ 0.0F };

        float gain{ 0.0F };
        float pan{ 0.0F };
        float occlusion{ 0.0F };
        float lowpass{ 0.0F };
    };

    VoiceMixer::VoiceMixer()
    {
        mix.resize(kBlockFrames * 2);
    }

    VoiceMixer::~VoiceMixer() = default;

    VoiceMixer::Channel* VoiceMixer::FindChannelLocked(Multiplayer::PeerSlot a_slot)
    {
        const auto channel = std::ranges::find(channels, a_slot, [](const auto& a_channel) { return a_channel->slot; });
        return channel == channels.end() ? nullptr : channel->get();
    }

    const Speaker* VoiceMixer::FindSpeakerLocked(Multiplayer::PeerSlot a_slot) const
    {
        const auto speaker = std::ranges::lower_bound(speakers, a_slot, {}, &Speaker::slot);
        return speaker != speakers.end() && speaker->slot == a_slot ? &*speaker : nullptr;
    }

    void VoiceMixer::Push(const Multiplayer::VoiceFrame& a_frame)
    {
        const auto now = Clock::now();
        const std::scoped_lock guard{ lock };
        const auto* speaker = FindSpeakerLocked(a_frame.speaker);
        if (!speaker || speaker->gain <= 0.0F)
        {
            return;
        }
        auto* channel = FindChannelLocked(a_frame.speaker);
        if (!channel)
        {
            if (channels.size() >= kMaximumChannels)
            {
                // The quietest silent channel makes room, so a nearer speaker is heard over a farther one.
                const auto quietest = std::ranges::min_element(channels, {}, [this](const auto& a_channel) {
                    const auto* other = FindSpeakerLocked(a_channel->slot);
                    return a_channel->playing ? 2.0F : (other ? other->gain : 0.0F);
                });
                const auto* other = FindSpeakerLocked((*quietest)->slot);
                if ((*quietest)->playing || (other && other->gain >= speaker->gain))
                {
                    return;
                }
                channels.erase(quietest);
            }
            channel = channels.emplace_back(std::make_unique<Channel>(a_frame.speaker)).get();
            channel->gain = speaker->gain;
            channel->pan = speaker->pan;
            channel->occlusion = speaker->occlusion;
        }
        channel->Insert(a_frame, now);
    }

    void VoiceMixer::SetSpeakers(std::span<const Speaker> a_speakers)
    {
        // Decoders are destroyed here rather than on the playback thread.
        std::vector<std::unique_ptr<Channel>> removed;
        const std::scoped_lock guard{ lock };
        speakers.assign(a_speakers.begin(), a_speakers.end());
        std::ranges::sort(speakers, {}, &Speaker::slot);
        for (auto& channel : channels)
        {
            const auto* speaker = FindSpeakerLocked(channel->slot);
            if (!speaker || speaker->gain <= 0.0F)
            {
                removed.push_back(std::move(channel));
            }
        }
        std::erase(channels, nullptr);
    }

    void VoiceMixer::Render(float* a_stereo, std::size_t a_frames, float a_masterGain)
    {
        const std::scoped_lock guard{ lock };
        while (a_frames > 0)
        {
            const auto block = (std::min)(a_frames, kBlockFrames);
            std::fill_n(mix.begin(), block * 2, 0.0F);
            for (auto& channel : channels)
            {
                const auto* speaker = FindSpeakerLocked(channel->slot);
                const auto targetGain = speaker ? speaker->gain : 0.0F;
                // Pan and occlusion move once per block, a few milliseconds, which is smooth enough
                // for them and spares an exponential and two cosines per sample.
                const auto step = (std::min)(1.0F, static_cast<float>(block) * kParameterSmoothing);
                if (speaker)
                {
                    channel->pan += (speaker->pan - channel->pan) * step;
                    channel->occlusion += (speaker->occlusion - channel->occlusion) * step;
                }
                const auto coefficient = LowpassCoefficient(channel->occlusion);
                const auto occlusionGain = std::lerp(1.0F, kOccludedGain, channel->occlusion);
                // Equal power across the stereo field.
                const auto angle = (std::clamp(channel->pan, -1.0F, 1.0F) + 1.0F) * std::numbers::pi_v<float> / 4.0F;
                const auto left = std::cos(angle) * occlusionGain;
                const auto right = std::sin(angle) * occlusionGain;
                for (std::size_t i = 0; i < block; ++i)
                {
                    if (channel->read >= channel->pcm.size() && !channel->DecodeNext())
                    {
                        channel->Release();
                        break;
                    }
                    channel->gain += (targetGain - channel->gain) * kParameterSmoothing;
                    channel->lowpass += (channel->pcm[channel->read++] - channel->lowpass) * coefficient;
                    const auto sample = channel->lowpass * channel->gain;
                    mix[i * 2] += sample * left;
                    mix[i * 2 + 1] += sample * right;
                }
            }
            for (std::size_t i = 0; i < block * 2; ++i)
            {
                a_stereo[i] = Limit(mix[i] * a_masterGain);
            }
            a_stereo += block * 2;
            a_frames -= block;
        }
    }

    void VoiceMixer::CopyLevels(std::vector<SpeakerLevel>& a_levels)
    {
        a_levels.clear();
        const std::scoped_lock guard{ lock };
        for (const auto& channel : channels)
        {
            if (channel->playing || channel->level > 0.0F)
            {
                a_levels.push_back(SpeakerLevel{ .slot = channel->slot, .level = channel->level });
            }
        }
    }

    void VoiceMixer::Clear()
    {
        std::vector<std::unique_ptr<Channel>> removed;
        const std::scoped_lock guard{ lock };
        removed.swap(channels);
        speakers.clear();
    }
}
