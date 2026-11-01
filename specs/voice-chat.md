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

This system owns the audio devices, capture and encoding, the client's voice
route, playback with its jitter buffers and spatial mix, and the game-side
voice state: the microphone key, each speaker's distance, direction,
occlusion, and lip sync. It leaves to other systems:

- the wire messages, the datagram layout, and their validation to
  [Network](network.md#voice);
- which clients hear a speaker to
  [Interest and delivery](interest-and-delivery.md#voice-listeners), which
  derives them from the subscriptions;
- the engine calls for lip sync and occlusion rays to
  [Remote actor](remote-actor.md#voice);
- the speaking indicators, the player menu's volume slider, the HUD
  microphone, and the settings page to
  [UI overlay](ui-overlay.md#voice-chat);
- the `[Voice]` settings and the microphone key to
  [Configuration](configuration.md#voice);
- per-player volumes in `PlayerHub.players` to
  [Network](network.md#player-filters).

## Components

| Module | Responsibility |
| --- | --- |
| `audio/AudioEngine` | The public audio API; miniaudio devices opened and closed on a worker thread; the device list; the capture and playback callbacks |
| `audio/VoiceEncoder` | Input gain, RNNoise, the meter, push-to-talk and voice activation gating, Opus encoding |
| `audio/VoiceMixer` | One jitter buffer and Opus decoder per speaker, concealment, the stereo mix with gain, pan, and occlusion |
| `audio/MiniAudio.c` | The miniaudio implementation, built as C |
| `network/VoiceTransport` | The datagram socket, AES-256-GCM through Windows CNG, probes and keepalives, the route, the WebSocket fallback |
| `sync/voice/VoiceSync` | Game-thread state: devices following settings and connection, the microphone key, speakers' parameters, occlusion rays, lip sync, the speaking levels the UI reads |
| `runtime/KeyState` | Keys held and pressed as the input dispatch reports them, for the microphone key |
| `server/internal/voice` | The relay: datagram keys, replay windows, addresses, listeners, forwarding by datagram or outbox |

## Threads

| Thread | Work |
| --- | --- |
| Main loop | `VoiceSync::Maintain` every frame, also paused and in the main menu; `UpdateLocal` and `UpdatePeers` in the pass ([Sync pipeline](sync-pipeline.md#frame-sequence)) |
| Input | `KeyState::Record` from `InputCapture` |
| Capture device | `VoiceEncoder::Process`, then `VoiceTransport::Send` with each frame |
| Playback device | `VoiceMixer::Render` |
| Datagram thread | Receives datagrams, sends probes, decides the route |
| WebSocket thread | `PeerVoice` to `VoiceTransport::DeliverFromWebSocket`, the welcome's `Start`, a loss's `Stop` |
| Audio worker | Opening, closing, and listing devices, so no game or transport thread waits for a driver |

Frames cross threads as fixed-size `Multiplayer::VoiceFrame` values, so
nothing allocates per frame. The mixer holds one mutex between the network
threads' `Push`, the game thread's `SetSpeakers`, and the playback thread's
`Render`; decoders are destroyed on the game thread, outside the playback
callback.

## Activation

`[Voice] Mode` chooses push-to-talk (the default) or voice activation, and
`[Controls] Microphone` (`V`) serves both.

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
  frame before the onset is sent first, so a word's start is not cut. The key
  turns the microphone off and on (`VoiceSync`, counting `KeyState` presses
  between frames); off lasts until pressed again or the game ends.
- The player is heard only with voice enabled, a connection that carries
  voice, and the player in the world (`VoiceSync::UpdateLocal`), since the
  relay sends nothing of a player who is not placed. `Maintain` runs while the
  game is paused, so players talk with a menu open.

## Capture and encoding

- The capture device delivers 48 kHz mono `float` samples in 10 ms periods;
  miniaudio converts from the device's format.
- Per 10 ms chunk: `[Voice] InputVolume` as gain, then RNNoise (0.2, its
  built-in model) with `[Voice] NoiseSuppression`, then the meter: the chunk's
  RMS in dBFS mapped from -60..0 to 0..1, falling by 0.04 per chunk.
  `Audio::GetInputLevel` reads it for the settings window's meter.
- Two chunks make an Opus frame of 20 ms: `OPUS_APPLICATION_VOIP` at 24 kbit/s
  VBR, complexity 8, voice signal, in-band FEC for an expected 10% loss, and no
  DTX, since the gate already stops sending in pauses and DTX's gaps would read
  as loss. The encoder resets at each talk spurt's start. A frame is at most 400
  bytes (`Multiplayer::kMaximumVoiceFrameBytes`).
- Each frame takes the next 16-bit sequence, rising only with frames sent, so
  one spurt follows the last without a gap; the frame that closes a spurt
  carries `end`.

## Route

`VoiceTransport` starts with each welcome that has a `VoiceAccess`
([Network](network.md#voice)) and stops with the connection.

- The datagram thread resolves the access's host, or the WebSocket's host when
  it is empty, opens a connected UDP socket to the port, and probes every
  second. The first authentic datagram from the relay makes the route
  `kDatagrams` and sends `VoiceRoute{datagrams: true}`; after that it probes
  every 5 s, which also keeps a NAT mapping open. 12 s without an authentic
  datagram sends `VoiceRoute{datagrams: false}` and goes back to the WebSocket,
  probing every second until datagrams return.
- Until then, and when the socket cannot open, the route is `kWebSocket`:
  frames go as `VoiceFrame` messages and come as `PeerVoice`. Behind a proxy
  that carries no UDP, such as Cloudflare's, voice works this way, with TCP's
  latency under loss.
- A session's thread publishes its route only while it is current, so a
  stopping session never tells a newer connection about its datagrams.
- Datagram bytes count in the traffic rates with 28 bytes of IP and UDP headers
  ([Network](network.md#traffic)).

## Playback

`VoiceSync::UpdatePeers` gives the mixer every speaker in reach once per pass
(`Audio::SetSpeakers`): a peer whose remote actor is ready, not muted
([UI overlay](ui-overlay.md#mute-and-vanish)), and within the relay's radius.
Frames of anyone else are dropped on arrival, and their buffers are freed.

- **Gain**: full within 300 units of the player (about four metres), then
  `(300 / distance)^0.7`, gentler than the physical fall so a voice carries
  across a camp, times a fade over the last fifth of the radius to silence
  where the relay stops sending, times the player's
  [volume for that speaker](ui-overlay.md#voice-chat), 0 to 200%. The output
  device's `[Voice] OutputVolume` scales the mix.
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
- **Loss**: a missing frame is rebuilt from the next frame's FEC when that has
  arrived, otherwise concealed by Opus's packet loss concealment; after 5
  concealed frames (100 ms) the speaker is taken as silent. An `end` frame stops
  the spurt at once.
- **Cap**: 16 speakers are decoded at once. A new speaker over the cap takes the
  place of the quietest one not playing, or is left out, which bounds the
  playback thread's work in a crowd.
- The mix is soft-limited above 0.9 of full scale.

## Speaking levels and lip sync

- Each decoded frame's level, its RMS mapped from -50..-10 dBFS to 0..1, rises
  at once and falls by a quarter per frame. `VoiceSync` copies the levels once
  per pass, and `GetSpeakingLevel` gives a peer's while above 0.05, for the
  nameplates and the player list ([UI overlay](ui-overlay.md#voice-chat)).
- With `[Voice] LipSync`, a speaking peer's level opens its character's mouth
  through `ActorLipSync` each pass, and the mouth closes when it stops. The
  phonemes need no analysis of the voice, which reads as speech at a distance.

## Devices

- `Audio::Configure` takes the settings and whether voice is active: enabled,
  in a game, with a connection that carries voice. The worker opens the capture
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
   level, the first syllable included, and not otherwise; the microphone key
   turns it off and on, and the HUD shows it off.
4. A muted player is not heard; a vanished player's voice ends with its
   character; another player's volume set in the player menu lasts across games
   on that server.
5. A player whose UDP is blocked still hears and is heard, over the WebSocket.
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
  WebSocket, so an `http://` endpoint exposes them as it does the session token.
- Non-goals: voice channels without position, voice recording or replay (voice
  bypasses the hub and its recorder), and server-side moderation of voice.
