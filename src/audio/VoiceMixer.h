#pragma once

#include "audio/VoiceReverb.h"
#include "protocol/PeerSlot.h"
#include "protocol/VoiceFrame.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace Audio
{
    // Where and how loud a speaker is heard, from the game thread.
    struct Speaker
    {
        Multiplayer::PeerSlot slot{ 0 };
        // Linear, from distance; 0 drops its frames.
        float gain{ 0.0F };
        // -1 is fully left, 1 fully right.
        float pan{ 0.0F };
        // 0 is in the open, 1 behind a wall.
        float occlusion{ 0.0F };
        // Linear level sent to the room reverb.
        float reverbSend{ 0.0F };
        // Air absorption: 0 is beside the listener, 1 at the edge of hearing.
        float air{ 0.0F };
        // 0 is in front of or beside the camera, 1 straight behind it.
        float rear{ 0.0F };
    };

    struct SpeakerLevel
    {
        Multiplayer::PeerSlot slot{ 0 };
        // 0 to 1, the envelope of what is played.
        float level{ 0.0F };
    };

    // Plays the voices of the speakers the game thread names: one jitter buffer and Opus decoder
    // per speaker, mixed to stereo. Push runs on the network threads, Render on the playback device's.
    // Render decodes and mixes outside the lock, so the other threads wait only for its bookkeeping.
    class VoiceMixer
    {
    public:
        VoiceMixer();
        ~VoiceMixer();
        VoiceMixer(const VoiceMixer&) = delete;
        VoiceMixer& operator=(const VoiceMixer&) = delete;

        void Push(const Multiplayer::VoiceFrame& a_frame);

        // Speakers not named lose their buffers.
        void SetSpeakers(std::span<const Speaker> a_speakers);

        // Any thread. The reverb moves to a_reverb over a_fadeSeconds.
        void SetReverb(const RoomReverb& a_reverb, float a_fadeSeconds);

        void Render(float* a_stereo, std::size_t a_frames, float a_masterGain);

        void CopyLevels(std::vector<SpeakerLevel>& a_levels);

        void Clear();

    private:
        struct Channel;

        Channel* FindChannelLocked(Multiplayer::PeerSlot a_slot);
        const Speaker* FindSpeakerLocked(Multiplayer::PeerSlot a_slot) const;
        void MixChannel(Channel& a_channel, std::size_t a_frames);
        void CollectRetiredLocked(std::vector<std::unique_ptr<Channel>>& a_removed);

        std::mutex lock;
        std::vector<Speaker> speakers;
        std::vector<std::unique_ptr<Channel>> channels;
        // Removed channels wait here while Render may still be mixing them without the lock.
        std::vector<std::unique_ptr<Channel>> retired;
        bool mixing{ false };
        RoomReverb pendingReverb;
        float pendingFadeSeconds{ 0.0F };
        bool reverbChanged{ false };
        bool resetReverb{ false };

        // The playback thread's own.
        std::vector<Channel*> active;
        std::vector<float> mix;
        std::vector<float> send;
        VoiceReverb reverb;
    };
}
