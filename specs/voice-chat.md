# Voice Chat

## Status

The plugin side has not been built on Windows or run in game yet, so nothing
of it is confirmed beyond the server; everything under
[Engine assumptions](#engine-assumptions) is unconfirmed.

## Scope

Goal: players hear the players near them, from their characters' positions,
and are heard with push-to-talk or voice activation, at a bandwidth that
stays small beside the checkpoint stream. Non-goals: voice without a
character to place it at (radio, party, or whisper channels), recording voice,
echo cancellation, and server-side mixing.

This system owns the audio devices, capture and encoding, the client's
[relay link](#relay-link), playback with its jitter buffers and spatial mix, and the game-side
voice state: the microphone key, each speaker's distance, direction,
occlusion, and lip sync. It leaves to other systems:

- the wire messages, the datagram layout, and their validation to
  [Network](network.md#voice);
- which clients hear a speaker to
  [Interest and delivery](interest-and-delivery.md#voice-listeners), which
  derives them from the subscriptions;
- the engine calls for lip sync and occlusion rays to
  [Remote actor](remote-actor.md#voice);
- the speaking indicators, the HUD microphone, and the settings page to
  [UI overlay](ui-overlay.md#voice-chat);
- the `[Voice]` settings and the microphone key to
  [Configuration](configuration.md#voice).

## Components

| Module | Responsibility |
| --- | --- |
| `audio/AudioEngine` | The public audio API; miniaudio devices opened and closed on a worker thread; the device list; the capture and playback callbacks |
| `audio/VoiceEncoder` | RNNoise, the meter, push-to-talk and voice activation gating, input gain and its limiter, Opus encoding |
| `audio/VoiceMixer` | One jitter buffer and Opus decoder per speaker, concealment, the stereo mix with gain, pan, and occlusion |
| `audio/MiniAudio.c` | The miniaudio implementation, built as C |
| `network/VoiceTransport` | The datagram socket, AES-256-GCM through Windows CNG, probes and keepalives, the link's state |
| `sync/voicechat/VoiceChatSync` | Game-thread state: devices following settings and connection, the microphone key, speakers' parameters, occlusion rays, lip sync, the speaking levels the UI reads |
| `runtime/KeyState` | Keys held as the input dispatch reports them, for the microphone key |
| `server/internal/voice` | The relay: datagram keys, replay windows, addresses, listeners, forwarding by datagram or outbox |

## Threads

| Thread | Work |
| --- | --- |
| Main loop | `VoiceChatSync::Maintain` every frame, also paused and in the main menu; `UpdateLocal` and `UpdatePeers` in the pass ([Sync pipeline](sync-pipeline.md#frame-sequence)) |
| Input | `KeyState::Record` from `InputCapture` |
| Capture device | `VoiceEncoder::Process`, then `VoiceTransport::Send` with each frame |
| Playback device | `VoiceMixer::Render` |
| Datagram thread | Receives datagrams, sends probes, sets the link's state |
| WebSocket thread | The welcome's `VoiceTransport::Start`, a loss's `Stop` |
| Audio worker | Opening, closing, and listing devices, so no game or transport thread waits for a driver |

Frames cross threads as fixed-size `Multiplayer::VoiceFrame` values, so
nothing allocates per frame. The mixer holds one mutex between the network
threads' `Push`, the game thread's `SetSpeakers` and `CopyLevels`, and the
playback thread's `Render`. `Render` holds it per block only to take each
speaker's parameters and up to two frames off its jitter buffer, and again to
publish the speaking levels; it decodes and mixes without it, so no other
thread waits on a decoder. A decoder reset at a talk spurt's start is passed
with the next frame for the same reason. A channel removed while `Render` may
be mixing it waits in `retired` until no mix runs, and is destroyed by the next
`SetSpeakers` or `Clear` after the lock, never on the playback thread.

## Activation

`[Voice] Mode` chooses push-to-talk (the default) or voice activation.
`[Controls] Microphone` (`V`) is push-to-talk's key and does nothing under
voice activation.

- `InputCapture` records each keyboard press and release into `KeyState`
  without taking the key from the game, so a key the game also uses keeps its
  action. A press typed into a text field is not recorded; every release is.
- Push-to-talk sends while the key is held, and for 150 ms after it is released,
  so a word's end is not cut. The capture thread checks every 100 ms whether the
  game's window is in front (`GetForegroundWindow`) and ignores the key while
  another window has focus, since a release there never reaches the game.
- Voice activation sends while a 10 ms chunk's level reaches
  `[Voice] ActivationLevel` and, with noise suppression, RNNoise's voice
  probability is at least 0.5, and for 400 ms after the last such chunk. The
  level is the meter's, taken before `[Voice] InputVolume`
  ([Capture and encoding](#capture-and-encoding)), so the volume does not move
  what opens the gate. The frame before the onset is sent first, so a word's
  start is not cut.
- The player is heard only with voice enabled, a connected
  [relay link](#relay-link), and the player in the world (`VoiceChatSync::UpdateLocal`), since the
  relay sends nothing of a player who is not placed. `Maintain` runs while the
  game is paused, so players talk with a menu open.

## Capture and encoding

- The capture device delivers 48 kHz mono `float` samples in 10 ms periods;
  miniaudio converts from the device's format.
- Per 10 ms chunk: RNNoise (0.2, its built-in model) with
  `[Voice] NoiseSuppression`, then the meter: the chunk's RMS in dBFS mapped
  from -60..0 to 0..1, falling by 0.04 per chunk. `Audio::GetInputLevel` reads
  it for the settings window's meter.
- `[Voice] InputVolume` follows the meter and the gate, as a gain of 1.4
  (`kBaseInputGain`) times the percentage squared: 100% is +3 dB, since
  microphones came through quiet at unity gain, 50% is -9 dB, and 150% is
  +10 dB. Loudness as heard doubles with about 10 dB, so a linear gain would
  make 150% sound barely louder.
- A limiter follows the gain, since the encoder hard-clips beyond full scale:
  a sample above 0.8 turns the limiter's gain down at once to hold it there,
  and the gain returns over about 100 ms, so a boosted voice gets louder
  rather than distorted. The ceiling leaves room for the codec: speech limited
  to 0.9 decoded with peaks at full scale, and limited to 0.8 stayed below it.
- Two chunks make an Opus frame of 20 ms: `OPUS_APPLICATION_VOIP` at 24 kbit/s
  VBR, complexity 8, voice signal, in-band FEC enabled for an expected 5% loss,
  and no DTX, since the gate already stops sending in pauses and DTX's gaps
  would read as loss. The encoder resets at each talk spurt's start. A frame is
  at most 400 bytes (`Multiplayer::kMaximumVoiceFrameBytes`).
- The expected loss chooses between band and FEC: above 5%, libopus narrows the
  band until FEC fits, which at 24 kbit/s is wideband (8 kHz); at 5% or less it
  keeps fullband and adds FEC only once the bitrate affords it, which 24 kbit/s
  does not. Fullband without FEC was chosen, since the decoder's deep PLC
  ([Playback](#playback)) covers an isolated lost frame. On a speech sample, 10%
  gave wideband at about 52 bytes a frame with FEC in most frames, and 5% gave
  fullband at about 55 bytes without FEC. Complexity 10 produced the same
  stream as 8.
- Each frame takes the next 16-bit sequence, rising only with frames sent, so
  one spurt follows the last without a gap; the frame that closes a spurt
  carries `end`.

## Relay link

`VoiceTransport` starts with each welcome that has a `VoiceAccess`
([Network](network.md#voice)) and stops with the connection. Voice travels only
as datagrams; `VoiceTransport::GetState` gives the link's state.

- The datagram thread resolves the access's host, or the server address's
  host when it is empty
  ([Configuration](configuration.md#server-address)), opens a connected UDP socket to the port, and probes every
  second (`kConnecting`). The first authentic datagram from the relay makes the
  link `kConnected`; after that it probes every 5 s, which also keeps a NAT
  mapping open.
- 5 s of probes without an answer, 12 s without an authentic datagram once
  connected, or a host, key, or socket that cannot be set up make the link
  `kUnreachable`, and the settings window says so
  ([UI overlay](ui-overlay.md#voice-chat)). The thread keeps probing every
  second, so the link connects once datagrams get through.
- Frames are sent only while `kConnected`, and the audio devices are open only
  then ([Devices](#devices)). Behind a proxy that carries no UDP, such as
  Cloudflare's, the server's `voice.public_host` must name a host that reaches
  the relay directly.
- Datagram bytes count in the traffic rates with 28 bytes of IP and UDP headers
  ([Network](network.md#traffic)).

## Playback

`VoiceChatSync::UpdatePeers` gives the mixer every speaker in reach once per pass
(`Audio::SetSpeakers`): a peer whose remote actor is ready, not muted
([UI overlay](ui-overlay.md#mute-and-vanish)), and within the relay's radius.
Frames of anyone else are dropped on arrival, and their buffers are freed.

- **Gain**: full within 300 units of the player (about four metres), then
  `(300 / distance)^0.7`, gentler than the physical fall so a voice carries
  across a camp, times a fade over the last fifth of the radius to silence
  where the relay stops sending. The output device's `[Voice] OutputVolume` scales the mix.
- **Pan**: the speaker's direction from the camera against the camera's right
  vector, mixed with an equal-power law. Distance is from the player's
  character, so the third-person camera does not quiet everyone.
- **Occlusion**: a ray from the player's head to the speaker's
  ([Remote actor](remote-actor.md#voice)) every 0.25 s while the speaker is
  heard, else every second, at most 4 rays per pass. A blocked speaker goes
  through a one-pole low-pass at 900 Hz (20 kHz in the open) at 0.55 of its gain.
  Pan and occlusion move once per render block, gain per sample, each over about
  20 ms, so a step never clicks.
- **Jitter buffer**: 16 frames per speaker. A spurt starts playing once it holds
  2 frames, or up to 6 (120 ms) as the measured arrival jitter grows, or at once
  with an `end` frame. A late frame is dropped; one 16 or more ahead restarts the
  buffer.
- **Decoder**: libopus is built with OSCE (`libopus-dnn` in `xmake.lua`), which
  also builds deep PLC, from the xiph.org release archive that carries the
  models' weights (about 3 MB in the plugin). Decoder complexity 7
  (`kDecoderComplexity`) enables deep PLC and NoLACE, which enhances the SILK
  speech layer. Measured on one core of a desktop CPU over a fullband stream,
  a frame decoded in about 24 µs at complexity 0, 35 µs with LACE (6), and
  70 µs with NoLACE, or 200 µs with NoLACE without AVX2; 16 speakers with
  NoLACE take about 1.1 ms of the playback thread per 20 ms.
- **Loss**: a missing frame is rebuilt from the next frame's FEC when that has
  arrived, otherwise concealed by deep PLC; a next frame without FEC conceals
  the same way. After 5 concealed frames (100 ms) the speaker is taken as
  silent. An `end` frame stops the spurt at once.
- **Cap**: 16 speakers are decoded at once. A new speaker over the cap takes the
  place of the quietest one not playing, or is left out, which bounds the
  playback thread's work in a crowd.
- The mix is soft-limited above 0.9 of full scale.

## Speaking levels and lip sync

- Each decoded frame's level, its RMS mapped from -50..-10 dBFS to 0..1, rises
  at once and falls by a quarter per frame. `VoiceChatSync` copies the levels once
  per pass, and `IsSpeaking` tells whether a peer's is above 0.05, for the
  nameplates and the player list ([UI overlay](ui-overlay.md#voice-chat)).
- A speaking peer's level opens its character's mouth through `ActorLipSync`
  each pass, and the mouth closes when it stops. The phonemes need no analysis
  of the voice, which reads as speech at a distance.

## Devices

- `Audio::Configure` takes the settings and whether voice is active: enabled,
  in a game, with a connected [relay link](#relay-link). The worker opens the capture
  and playback devices only while active, so the system's microphone indicator
  shows only then, and closes both when not; a changed device name reopens that
  device. Gains and gating change without reopening.
- A device is chosen by its system name, as `[Voice] InputDevice` and
  `OutputDevice` hold it; empty is the system default, which miniaudio's WASAPI
  backend follows when it changes. A name no device has logs a warning and uses
  the default.
- A device that stops by itself (unplugged) is reopened with the next voice
  setting change or connection.
- The backends are WASAPI, then DirectSound and WinMM.
- The device list is read when the audio context starts and whenever the
  settings window opens a device combo (`Audio::RefreshDevices`).

## Bandwidth

Computed from the layout, not measured: a 24 kbit/s frame is about 60 bytes, so
an uplink datagram is 9 + 3 + 60 + 16 = 88 bytes plus 28 of IP and UDP, about
5.8 KB/s while talking, and nothing while silent except a 57-byte probe every
5 s. A listener receives the same per speaker heard (5 + 7 + 60 + 16 bytes). The
relay sends a speaker only to listeners within the radius
([Interest and delivery](interest-and-delivery.md#voice-listeners)), so a
crowd's voice costs each listener what the speakers near it say.

## Engine assumptions

1. `BSFaceGenAnimationData::phenomeKeyFrame` drives the face's phonemes as the
   console's `mfg phoneme` does, under the data's `lock`, for a remote actor as
   for an NPC, and the face update blends it into the final keyframe each frame.
2. `NiCamera`'s world rotation has the camera's right vector in its third
   column, as Gamebryo's `NiCamera::GetWorldRightVector` reads it, for
   `RE::Main::WorldRootCamera()` in first and third person.
3. `bhkWorld::PickObject` between two heads, with the character controller
   layer and a collector that accepts only static, animated static, terrain,
   ground, and prop layers, finds walls and terrain between players and passes
   characters, water, and clutter, as `GroundProbe`'s does.
4. miniaudio's WASAPI backend works under Wine and Proton.

## Acceptance criteria

1. Two players near each other hear each other from their characters'
   directions, louder when closer and silent beyond the radius; a wall between
   them muffles the voice.
2. With push-to-talk, a player is heard only while holding the microphone key,
   also with a menu open, and never after alt-tabbing with the key held.
3. With voice activation, a player is heard when speaking above the activation
   level, the first syllable included, and not otherwise.
4. A muted player is not heard; a vanished player's voice ends with its
   character.
5. A player whose UDP is blocked is told in the settings window that the
   voice relay cannot be reached, and holds no audio device open; once
   datagrams get through, voice starts without reconnecting.
6. A speaker shows the speaking glyph on its nameplate and in the player list,
   and its character's mouth moves while it speaks.
7. Changing the input or output device applies without a restart, and a
   disconnected game holds no audio device open.

## Known limitations

- No echo cancellation: a player using speakers rather than headphones may be
  heard twice.
- Lip sync follows loudness only, not phonemes.
- A speaker's voice is placed at its character, so a speaker without a ready
  remote actor (spawning, or beyond the visible cap) is not heard.
- The relay's radius is horizontal like interest, so a player on another floor
  of a tower is heard if near enough horizontally; occlusion muffles it.
- The datagram route's encryption keys last one connection and are sent over the
  WebSocket, so a server address without TLS exposes them as it does the session token.
- A player whose network blocks UDP to the relay has no voice.
- Non-goals: voice channels without position, voice recording or replay (voice
  bypasses the hub and its recorder), server-side moderation of voice, and
  voice over the WebSocket, where TCP holds every frame behind a lost segment
  and voice would share the outbox and its budget with presentation.
