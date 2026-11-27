# Plugin API

## Scope

The interface other SKSE plugins call into PlayerHub with: how they get it,
how it is versioned, and the checks on what they pass. The behavior behind
each call belongs to the system that owns it; the pose window holds this
version offers are part of the [skeleton pose source window](remote-actor.md#source-window).
Nothing here reaches the network: a call changes only what the local client
decides to publish.

## Components

- `include/PlayerHubAPI.h`: the public header other plugins include.
  `IVPlayerHub1`, `kInterfaceVersion1`, `PoseWindowHold`, the export's
  `RequestPluginAPIFunction` type, and `PlayerHubAPI::RequestPluginAPI()`,
  which finds `PlayerHub.dll` with `GetModuleHandleW` and `GetProcAddress` and
  so needs `<windows.h>` in the caller's translation unit. It depends on
  nothing else, CommonLib included.
- `src/api/PluginAPI.cpp`: the `RequestPluginAPI` export and the interface
  object, which checks what callers pass and delegates to `PoseWindow`.

## Retrieval and versioning

- PlayerHub exports `RequestPluginAPI(std::uint32_t version)` with C linkage,
  following the `RequestPluginAPI` convention of Precision and TrueHUD. For
  `kInterfaceVersion1` (1) it returns the `IVPlayerHub1` object as `void*` and
  logs `Plugin API interface requested`; any other version returns `nullptr`
  (`Plugin API refused an unknown interface version`). Callers request it at
  or after SKSE's `kPostLoad`, when every plugin DLL is loaded.
- The object is static and lives as long as the process.
- The interface is pure virtual with only C types across the boundary, since
  callers use other compilers, runtime libraries, and CommonLib forks. Its
  destructor is protected and non-virtual, so callers cannot delete it and the
  vtable holds only the calls. Later versions add an interface that extends
  the previous one, appending its calls, so a caller written for version 1
  keeps working.
- Every call is `noexcept`: an exception inside PlayerHub is logged
  (`Plugin API could not ...`) and the call returns as refused.
- Calls are safe from any thread. They record a request under a lock; the
  main loop acts on it in its next pass.

## Pose window holds

A plugin that plays an animation PlayerHub does not detect as modded (a
modded idle form sent through a vanilla event, a behavior under modded
conditions, a scripted sequence) holds a pose window open while it plays, so
other players see the source's pose instead of the vanilla reproduction.

- `OpenPoseWindow(owner)` returns a nonzero `PoseWindowHold`, or 0 when 64
  holds are held. The owner only names the caller in the log: up to 64 bytes
  are read, never past its terminator, characters outside printable ASCII
  become `?`, and an empty or null owner becomes `unnamed`.
- `ClosePoseWindow(hold)` releases one hold. 0, an unknown hold, and one
  already released (by a game load) are ignored.
- What a hold does to the window, its blocks, and its release on a game load:
  [Remote actor](remote-actor.md#source-window). Holds are independent: one
  caller's close never ends a window another hold, a modded state, or a
  reaction keeps open, and no call closes or suppresses PlayerHub's own
  windows.
- A hold belongs to the call that opened it, not to a plugin, so one plugin
  may keep several. PlayerHub cannot tell callers apart (they share the
  interface object, and the owner is whatever they pass), and a plugin with
  overlapping animations then releases each without counting them itself.

## Acceptance criteria

- A plugin that requests version 1 after `kPostLoad` gets the interface;
  without PlayerHub loaded, the header's helper returns `nullptr`.
- While a plugin holds a window on foot in human form, other players see the
  local player's pose; after its close they see the vanilla reproduction
  again, unless a modded state or reaction still holds the window.
- Two holds at once: the window stays open until both are closed.
- Mounting, sitting, transforming, dying, or disconnecting during a hold stops
  the poses; when that ends with the hold still held, the poses resume.
- Loading a save or starting a new game releases every hold.

## Known limitations

- A hold has no timeout: a plugin that never closes one streams the player's
  pose until a game loads. The server's `interest.max_posed` still bounds what
  each recipient receives.
- A plugin is not told when a game load released its holds.

Non-goals:

- Papyrus functions; script-only mods cannot hold a window.
- Holding a window where receivers keep the graph (beast forms, furniture,
  mounts, death), which would need receiver changes.
- Suppressing PlayerHub's own windows.
