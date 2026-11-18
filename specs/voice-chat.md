# Voice Chat

## Scope

Goal: players hear the players near them, from their characters' positions,
and are heard with push-to-talk or voice activation, at a bandwidth that
stays small beside the checkpoint stream. Non-goals: voice without a
character to place it at (radio, party, or whisper channels), recording voice,
echo cancellation, and server-side mixing.

This system owns the audio devices, capture and encoding, the client's
[relay link](#relay-link), playback with its jitter buffers and spatial mix,
and the game-side voice state: the microphone key, each speaker's distance,
direction, occlusion, and lip sync. Other systems own:

- the wire messages, datagram layout, and validation:
  [Network](network.md#voice);
- which clients hear a speaker:
  [Interest and delivery](interest-and-delivery.md#voice-listeners);
- the engine calls for lip sync and occlusion rays:
  [Remote actor](remote-actor.md#voice);
- the speaking indicators, HUD microphone, and settings page:
  [UI overlay](ui-overlay.md#voice-chat);
- the `[Voice]` settings and the microphone key:
  [Configuration](configuration.md#voice).

## Components

| Module | Responsibility |
| --- | --- |
| `audio/AudioEngine` | Public audio API; miniaudio devices opened and closed on a worker thread; device list; capture and playback callbacks |
| `audio/VoiceEncoder` | RNNoise, meter, push-to-talk and voice activation gating, input gain and limiter, Opus encoding |
| `audio/VoiceMixer` | Per-speaker jitter buffer and Opus decoder, concealment, stereo mix with gain, pan, and occlusion |
| `audio/MiniAudio.c` | The miniaudio implementation, built as C |
| `network/VoiceTransport` | Datagram socket, AES-256-GCM through Windows CNG, probes and keepalives, link state |
| `sync/voicechat/VoiceChatSync` | Game-thread state: devices following settings and connection, microphone key, speaker parameters, occlusion rays, lip sync, speaking levels for the UI |
| `runtime/KeyState` | Keys held as the input dispatch reports them |
| `server/internal/voice` | The relay: datagram keys, replay windows, addresses, listeners, forwarding |

## Threads

| Thread | Work |
| --- | --- |
| Main loop | `VoiceChatSync::Maintain` every frame, also paused and in the main menu; `UpdateLocal` and `UpdatePeers` in the pass ([Sync pipeline](sync-pipeline.md#frame-sequence)) |
| Input | `KeyState::Record` from `InputCapture` |
| Capture device | `VoiceEncoder::Process`, then `VoiceTransport::Send` per frame |
| Playback device | `VoiceMixer::Render` |
| Datagram thread | Receives datagrams, sends probes, sets the link state |
| WebSocket thread | `VoiceTransport::Start` on the welcome, `Stop` on a loss |
| Audio worker | Opening, closing, and listing devices, so no game or transport thread waits for a driver |

Frames cross threads as fixed-size `Multiplayer::VoiceFrame` values, so nothing
allocates per frame. One mixer mutex is shared by the network threads' `Push`,
the game thread's `SetSpeakers` and `CopyLevels`, and `Render`. `Render` holds
it only to take each speaker's parameters and up to two frames, and to publish
levels; it decodes and mixes unlocked, and a decoder reset at a spurt's start
travels with the next frame for the same reason. A channel removed while a mix
may use it waits in `retired` and is destroyed by the next `SetSpeakers` or
`Clear`, never on the playback thread.

## Activation

`[Voice] Mode` chooses push-to-talk (default) or voice activation.
`[Controls] Microphone` (`V`) is push-to-talk's key.

- `InputCapture` records presses and releases into `KeyState` without taking
  the key from the game. A press typed into a text field is not recorded; every
  release is.
- Push-to-talk sends while the key is held and 150 ms after release. The
  capture thread checks `GetForegroundWindow` every 100 ms and ignores the key
  while another window has focus, since a release there never reaches the game.
- Voice activation sends while a 10 ms chunk's level reaches
  `[Voice] ActivationLevel` and, with noise suppression, RNNoise's voice
  probability is at least 0.5, and for 400 ms after. The level is taken before
  `[Voice] InputVolume`, so the volume does not move the gate. The frame before
  the onset is sent first, so a word's start is not cut.
- The player is heard only with a connected
  [relay link](#relay-link), and the player placed in the world
  (`VoiceChatSync::UpdateLocal`). `Maintain` runs while paused, so players talk
  with a menu open.

## Capture and encoding

- Capture is 48 kHz mono `float` in 10 ms periods; miniaudio converts the
  device format.
- Per chunk: RNNoise (0.2, built-in model) with `[Voice] NoiseSuppression`,
  then the meter: RMS in dBFS mapped from -60..0 to 0..1, falling 0.04 per
  chunk, read by `Audio::GetInputLevel`.
- `[Voice] InputVolume` is applied after the meter and gate as
  `kBaseInputGain` (1.4) times the percentage squared: 100% is +3 dB, since
  microphones came through quiet at unity, and 150% is +10 dB, since perceived
  loudness doubles with about 10 dB.
- A limiter follows, since the encoder hard-clips beyond full scale: a sample
  above 0.8 turns its gain down at once, recovering over about 100 ms. Speech
  limited to 0.9 decoded with peaks at full scale; 0.8 stayed below.
- Two chunks make a 20 ms Opus frame: `OPUS_APPLICATION_VOIP`, 24 kbit/s VBR,
  complexity 8, voice signal, expected loss 5%, no DTX (the gate already stops
  in pauses, and DTX gaps would read as loss). The encoder resets at each
  spurt. A frame is at most `Multiplayer::kMaximumVoiceFrameBytes` (400).
- Expected loss trades band for FEC: above 5% libopus narrows to wideband at
  24 kbit/s so FEC fits; at 5% it stays fullband without FEC. Fullband was
  chosen, since deep PLC ([Playback](#playback)) covers an isolated loss. Both
  give about 55 bytes a frame; complexity 10 produced the same stream as 8.
- Frames take consecutive 16-bit sequences, rising only with frames sent; the
  frame that closes a spurt carries `end`.

## Relay link

`VoiceTransport` starts with each welcome that has a `VoiceAccess`
([Network](network.md#voice)) and stops with the connection. Voice travels only
as datagrams; `VoiceTransport::GetState` gives the link state.

- The datagram thread resolves the access's host, or the server address's host
  when it is empty ([Configuration](configuration.md#server-address)), opens a
  connected UDP socket, and probes every second (`kConnecting`). The first
  authentic datagram makes it `kConnected`; then it probes every 5 s, which
  also keeps a NAT mapping open.
- 5 s of unanswered probes, 12 s without an authentic datagram once connected,
  or a host, key, or socket that cannot be set up make it `kUnreachable`
  ([UI overlay](ui-overlay.md#voice-chat)). Probing continues every second, so
  the link connects once datagrams get through.
- Frames are sent, and the [devices](#devices) open, only while `kConnected`.
  Behind a proxy without UDP, such as Cloudflare's, `voice.public_host` must
  name a host that reaches the relay directly.
- Datagram bytes count in the traffic rates with 28 bytes of IP and UDP headers
  ([Network](network.md#traffic)).

## Playback

`VoiceChatSync::UpdatePeers` passes every speaker in reach to the mixer once
per pass (`Audio::SetSpeakers`): a peer within the relay's radius, neither
muted nor vanished ([UI overlay](ui-overlay.md#mute-and-vanish)), placed at its
remote actor once ready. A peer in the local coordinate space without a ready
actor, such as one waiting to spawn, being respawned, or hidden in
[shared furniture](native-interactions.md#shared-furniture), is placed at its
played pose instead, without lip sync. Vanish is applied by the server
([Network](network.md#vanish)); the client also checks it, so a vanished
peer is silent before it leaves. Frames of anyone else are dropped on arrival
and their buffers freed.

- **Gain**: full within 300 units, then `(300 / distance)^0.7`, gentler than
  physical falloff so a voice carries across a camp, times a fade over the last
  fifth of the radius. `[Voice] OutputVolume` scales the mix.
- **Pan**: the speaker's direction against the camera's right vector, equal
  power. Distance is from the player's character, so the third-person camera
  does not quiet everyone.
- **Occlusion**: a head-to-head ray ([Remote actor](remote-actor.md#voice))
  every 0.25 s while the speaker is heard, else every second, at most 4 per
  pass. A blocked speaker goes through a 900 Hz one-pole low-pass at 0.55 gain.
  Pan and occlusion move per render block, gain per sample, each over about
  20 ms, so steps never click.
- **Jitter buffer**: 16 frames per speaker. A spurt starts once it holds 2
  frames, up to 6 (120 ms) as measured jitter grows, or at once with an `end`
  frame. A late frame is dropped; one 16 or more ahead restarts the buffer.
- **Decoder**: libopus built with OSCE (`libopus-dnn` in `xmake.lua`, from the
  xiph.org release archive that carries the model weights, about 3 MB), which
  also builds deep PLC. `kDecoderComplexity` 7 enables deep PLC and NoLACE.
  A fullband frame decodes in about 24 µs at complexity 0, 35 µs with LACE, and
  70 µs with NoLACE (200 µs without AVX2); 16 speakers take about 1.1 ms per
  20 ms.
- **Loss**: a missing frame is rebuilt from the next frame's FEC when present,
  otherwise concealed by deep PLC. After 5 concealed frames the speaker is
  silent; an `end` frame stops the spurt at once.
- **Cap**: 16 speakers decode at once. A new speaker over the cap replaces the
  quietest one not playing, or is left out.
- The mix is soft-limited above 0.9 of full scale.

## Speaking levels and lip sync

- Each decoded frame's RMS, mapped from -50..-10 dBFS to 0..1, rises at once
  and falls a quarter per frame. `VoiceChatSync` copies levels once per pass;
  `IsSpeaking` is a level above 0.05, for nameplates and the player list.
- A speaking peer's level opens its character's mouth through `ActorLipSync`;
  it closes when the peer stops. No phoneme analysis.

## Devices

- `Audio::Configure` takes the settings and whether voice is active (in a
  game, relay link connected). The worker opens both devices only while
  active, so the system's microphone indicator shows only then. A changed
  device name reopens that device; gains and gating change without reopening.
- `[Voice] InputDevice` and `OutputDevice` hold system device names; empty is
  the system default, which miniaudio's WASAPI backend follows. An unknown name
  logs a warning and uses the default.
- A device that stops by itself (unplugged) reopens with the next voice setting
  change or connection.
- Backends: WASAPI, then DirectSound and WinMM.
- The device list is read when the audio context starts and when the settings
  window opens a device combo (`Audio::RefreshDevices`).

## Bandwidth

Computed from the layout: an uplink datagram is 9 + 3 + 60 + 16 = 88 bytes plus
28 of IP and UDP, about 5.8 KB/s while talking, and a 57-byte probe every 5 s
otherwise. A listener receives about the same per speaker heard
(5 + 7 + 60 + 16 bytes), only from speakers within the radius
([Interest and delivery](interest-and-delivery.md#voice-listeners)).

## Engine assumptions

1. `BSFaceGenAnimationData::phenomeKeyFrame`, written under the data's `lock`,
   drives a remote actor's phonemes as the console's `mfg phoneme` does, and
   the face update blends it each frame.
2. `NiCamera`'s world rotation has the camera's right vector in its third
   column (`NiCamera::GetWorldRightVector`) for `RE::Main::WorldRootCamera()` in
   first and third person.
3. `bhkWorld::PickObject` between two heads, on the character controller layer
   with a collector accepting only static, animated static, terrain, ground,
   and prop layers, finds walls and terrain and passes characters, water, and
   clutter, as `GroundProbe`'s does.
4. miniaudio's WASAPI backend works under Wine and Proton.

## Acceptance criteria

1. Two nearby players hear each other from their characters' directions,
   louder when closer, silent beyond the radius, muffled through a wall.
2. With push-to-talk, a player is heard only while holding the microphone key,
   also with a menu open, and never after alt-tabbing with the key held.
3. With voice activation, a player is heard when speaking above the activation
   level, first syllable included, and not otherwise.
4. A muted or vanished player is not heard.
5. A player in the same space is heard while its character is not shown:
   before it spawns, while it respawns, and while it is hidden in furniture.
6. A player whose UDP is blocked is told in the settings window, holds no
   audio device open, and gets voice without reconnecting once datagrams pass.
7. A speaker shows the speaking glyph on its nameplate and in the player list,
   and its character's mouth moves.
8. Changing a device applies without a restart; a disconnected game holds no
   audio device open.

## Known limitations

- No echo cancellation: a player on speakers may be heard twice.
- Lip sync follows loudness, not phonemes.
- A speaker whose cell or worldspace does not resolve locally (its plugin is
  not loaded) is not heard, since it has no actor or local space to place it.
- The radius is horizontal like interest, so a player on another floor of a
  tower is heard when near enough horizontally; occlusion muffles it.
- Datagram keys last one connection and are sent over the WebSocket, so a
  server address without TLS exposes them as it does the session token.
- A network that blocks UDP to the relay has no voice.
- Non-goals: voice channels without position, voice recording or replay (voice
  bypasses the hub and its recorder), voice moderation on the server, and voice
  over the WebSocket, where TCP holds every frame behind a lost segment and
  voice would share the outbox budget with presentation.
