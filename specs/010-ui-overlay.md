# UI Overlay and Ping Indicator

## Status

Implemented, pending a Windows build and in-game check. The renderer hook site
was checked against the 1.7.104 executable and address library. SE 1.5.97 uses
the same ID and offset as other public ImGui overlays but has not been checked
here. Skyrim VR is not supported: the hook is not installed and a warning is
logged.

## Goal

Give the plugin its own UI layer. The first use is a small indicator in a screen
corner showing whether the client is connected and its round-trip latency to the
server. Later, the same layer has to host settings that players can change.

## Technology choice

| Option | Result |
| --- | --- |
| Dear ImGui on Skyrim's D3D11 device | **Chosen.** One static library, with no assets or extra runtime files. Immediate-mode code keeps a settings screen close to the values it edits, and many SKSE plugins draw this way, so it gets along with ENB, ReShade, and Community Shaders. |
| Scaleform (Flash) menus | Looks native, but needs SWF authoring tools, ActionScript, and a Papyrus/ESP or SkyUI MCM for settings. That adds a lot of tooling and cross-language glue for a few widgets. |
| Chromium Embedded Framework | Can render any web UI, but it is a runtime of more than 100 MB with separate processes, offscreen texture upload, and fragile behavior under Wine/Proton. That is too much for a HUD and a settings panel. |

If a native look matters later, a Scaleform HUD can still be added beside the
ImGui settings screen without changing the network side.

## Source layout

```text
src/ui/
  Overlay.h/.cpp            # renderer hook, ImGui context/backends, frame order
  Layout.h                  # shared placement helpers (screen corners)
  hud/PingIndicator.h/.cpp  # always-on connection/ping widget
src/network/
  ConnectionMonitor.h/.cpp  # probe cadence, timeout, latest sample, reconnect schedule
```

`ui/` reads state and never changes the game or the network. `Overlay::DrawFrame`
calls each widget in a fixed order. Like `SyncCoordinator`, it uses no widget
registry or virtual interface. Add later screens as siblings, for example
`ui/menu/SettingsMenu`.

## Rendering

- `UI::Overlay::Install` runs at plugin load and uses 14 trampoline bytes. It
  replaces the `call` at `+0x9` in `BSGraphics::Renderer::End` (IDs 75461/77246).
  On 1.7.104 that is `mov ecx, 1; call <timer>`, and the same function later
  calls `IDXGISwapChain::Present` (`call [r9+0x40]` at `+0xA2`). The thunk
  calls the original function first, then draws.
- The ImGui context is created on the first hooked frame that has a device,
  context, window, and swap chain. If a backend fails to initialize, the
  overlay turns itself off for the session and logs an error.
- `io.DisplaySize` comes from the swap chain's back buffer, not the window's
  client rectangle. Sizes and fonts are written for 1080p and scaled by
  `backBufferHeight / 1080` (at least 0.5) with the scalable default vector font.
- The DX11 backend does not save or restore the output merger. The overlay binds
  the swap chain's render target view, draws, and then restores the previous
  targets.
- `ImGuiConfigFlags_NoMouse`, `NoMouseCursorChange`, and `NoKeyboard` are set, and
  `imgui.ini` is disabled.

## Latency measurement

- The client emits `latency:probe` with no arguments and an ack callback. The
  server returns `{"ok": true}` right away and does not relay or store anything.
- The main-loop hook calls `Network::UpdateConnectionMonitor(delta)` after
  `SyncCoordinator::OnFrame`, so it runs only while the game frame time
  advances. The main menu does not, so there the ping stays unknown until a save
  loads. The client sends a probe every 1 second after the
  previous one was sent. Only one probe is outstanding at a time. The interval
  and timeouts follow the frame-delta rules in `AGENTS.md`.
- The round trip itself is measured with `std::chrono::steady_clock` from emit to
  ack, so frame rate does not affect the number.
- Each probe has an ID, and a compare-exchange on the outstanding ID accepts only
  the current probe's ack. Acks from abandoned probes or older connections are
  ignored. A disconnect or a new connection generation clears the sample.

## Connection loss and recovery

The client is disconnected exactly when the default namespace is not open, and
the overlay shows that state whichever side ended it. Every loss is recoverable.

- Detection, server side: a server restart, an Engine.IO ping timeout, or a
  namespace `DISCONNECT` reaches the client as a namespace close, a transport
  close, or a failed connect. All three mark the client disconnected and forget
  every remote player.
- Detection, silent server: the library does not notice a server that stops
  answering. A probe with no ack after 5 seconds of accumulated, clamped frame
  time makes `ConnectionMonitor` call `AbandonConnection`, which marks the client
  disconnected exactly as a reported close would.
- The library's own retry is disabled (`set_reconnect_attempts(0)`). It skips any
  close with a normal status, which is how a server drops a client, and it leaves
  the namespace closed on a live transport. Instead each loss is reported once
  and `ConnectionMonitor` reopens the connection.
- Recovery: after a loss the monitor waits 1 second, then calls
  `BeginReconnect`, doubling the wait after each failed attempt up to 15 seconds.
  Each wait is scaled by a random factor in 0.5–1.5, so clients that lost the
  same server do not all retry together.
  Attempts are unlimited. An attempt that is reported failed, or that has not
  opened the namespace after 20 seconds, is followed by the next one.
- `BeginReconnect` runs on a worker thread: `sync_close` joins the old transport
  thread, so nothing from the dead connection is reported afterwards. It then
  registers the event handlers again, because the library drops the socket object
  and its handlers when the server closes the namespace, and calls `connect`.
- A reopened namespace advances the connection generation, so latest-wins
  publishers republish their state. Remote players are recreated from the
  server's replay.

## Ping indicator

The indicator sits in the top-right corner, 12 px from both edges before scaling,
on a translucent background. It takes no input.

| State | Text | Color |
| --- | --- | --- |
| Not connected | `Disconnected` | red |
| Connected, no sample yet or timed out | `Ping -- ms` | grey |
| ≤ 80 ms | `Ping N ms` | green |
| ≤ 150 ms | `Ping N ms` | yellow |
| > 150 ms | `Ping N ms` | red |

## Future work: settings screen

- Input: Skyrim reads keyboard and mouse through DirectInput, so window messages
  alone do not reach ImGui reliably. An interactive menu must feed ImGui from the
  `BSInputDeviceManager` input event stream, block those events from the game
  while the menu is open, and show a cursor. When that input is scaled, it must
  use the same back-buffer size as `io.DisplaySize`.
- Settings storage: add one settings module that loads and saves a file under
  `Data/SKSE/Plugins/` and is read by the game thread. The server endpoint file
  and `PingIndicator::kCorner` are the first candidates to move there.
- Pausing: while the menu is open, decide whether to open a native menu, so
  that the game pauses and controls are released, or to only capture input.

## Verification

- Server: `test_app_latency.py` checks that the probe is acknowledged with
  `{"ok": true}` and not relayed, and that a client-supplied `sentAtMs` is
  neither stored nor relayed.
- In game (pending): the indicator shows `Disconnected` before the connection,
  then a stable ping. Stopping the server shows `Disconnected`, and a restart
  recovers. So do a server-side kick, and a blocked route to the server, which
  is noticed after the 5-second probe timeout. The overlay scales at 1080p and
  4K, and nothing new appears in the log apart from the state-change lines.
