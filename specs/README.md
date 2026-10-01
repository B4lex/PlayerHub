# Specs

A retroactive summary of the implementation, organized by system, so humans and
agents can pick up context quickly. The code is the authority when a spec
disagrees with it. `AGENTS.md` describes how to keep these files current.

## Systems

| Spec | System | Sources |
| --- | --- | --- |
| [sync-pipeline.md](sync-pipeline.md) | Main-loop pass, game thread, peer receive state, checkpoint publishing and playout, world presence, actor lifecycle | `src/runtime/`, `src/sync/` (top level, `presence/`) |
| [network.md](network.md) | Wire protocol, client transport and connection, server connections, player sessions, and validation | `proto/`, `src/protocol/`, `src/network/`, `server/internal/{session,auth,validate,wire,world}` |
| [interest-and-delivery.md](interest-and-delivery.md) | Which peers each client receives, exclusions, per-recipient outboxes, ordering, batching | `server/internal/{interest,hub,delivery}` |
| [remote-actor.md](remote-actor.md) | Spawn, update order, registry, animation firewall, graph recovery, survival, combat and reaction isolation, world isolation | `src/actor/` |
| [locomotion.md](locomotion.md) | Transform, movement state, direction, turning, jumps, swimming | `sync/movement/`, `sync/animation/`, `actor/ActorMovement`, `actor/ActorAnimation` |
| [combat-presentation.md](combat-presentation.md) | Weapon stance, attack action stream, power attacks, bashes, blocking, bow draws, spell casts | `sync/combat/`, `actor/ActorAnimation` |
| [appearance-and-equipment.md](appearance-and-equipment.md) | Appearance, actor base, player label, equipment, default outfit, item filter and catalog | `sync/appearance/`, `sync/equipment/`, `sync/outfit/`, `src/forms/` |
| [native-interactions.md](native-interactions.md) | Furniture, mounts, death: where Skyrim drives the actor | `sync/furniture/`, `sync/mount/`, `sync/death/`, `actor/ActorFurniture`, `actor/ActorMount`, `actor/ActorDeath` |
| [configuration.md](configuration.md) | `skyrim-mp.ini`, schema, snapshots, saving | `src/config/` |
| [ui-overlay.md](ui-overlay.md) | Overlay, input focus, fonts, status panel, chat, settings window, item picker and preview | `src/ui/`, `sync/chat/` |
| [recording-and-replay.md](recording-and-replay.md) | Captures, virtual players, load testing, admin panel | `server/internal/{recording,playback,loadtest,admin}`, `server/cmd/`, `server/admin-ui/` |

## Features

Where each feature is described. The first spec owns its goal and acceptance
criteria.

| Feature | Specs |
| --- | --- |
| Movement and locomotion | [locomotion](locomotion.md), [network](network.md#checkpoint-and-context), [sync-pipeline](sync-pipeline.md#local-publishing) |
| Checkpoint playout (smooth remote movement) | [sync-pipeline](sync-pipeline.md#checkpoint-playout), [locomotion](locomotion.md#coalescing), [network](network.md#checkpoint-and-context), [interest-and-delivery](interest-and-delivery.md#batching) |
| Swimming | [locomotion](locomotion.md#swimming), [remote-actor](remote-actor.md#swimming) |
| Weapon stance and attacks | [combat-presentation](combat-presentation.md), [network](network.md#actions) |
| Bow draws and spell casts | [combat-presentation](combat-presentation.md#hand-presentation), [appearance-and-equipment](appearance-and-equipment.md#equipment) |
| Blocking, bashes, power attacks | [combat-presentation](combat-presentation.md) |
| Appearance and player labels | [appearance-and-equipment](appearance-and-equipment.md#appearance), [network](network.md#appearance-and-equipment) |
| Sessions and nicknames | [network](network.md#sessions), [appearance-and-equipment](appearance-and-equipment.md#nickname), [configuration](configuration.md#general), [ui-overlay](ui-overlay.md#status-panel) |
| Equipment and default outfit | [appearance-and-equipment](appearance-and-equipment.md#equipment), [network](network.md#playerequipment), [configuration](configuration.md#defaultoutfit) |
| Furniture | [native-interactions](native-interactions.md#furniture), [remote-actor](remote-actor.md#placement-ownership) |
| Mounts | [native-interactions](native-interactions.md#mounts), [sync-pipeline](sync-pipeline.md#actor-lifecycle) |
| Death | [native-interactions](native-interactions.md#death) |
| World presence, fast travel, spawn throttling | [sync-pipeline](sync-pipeline.md) |
| Combat and reaction isolation | [remote-actor](remote-actor.md#combat-isolation) |
| World isolation and collision | [remote-actor](remote-actor.md#world-isolation), [native-interactions](native-interactions.md#collision) |
| Connection, latency, traffic | [network](network.md#connection), [ui-overlay](ui-overlay.md#status-panel) |
| Proximity, exclusions, late join, peer enter and leave, batching | [interest-and-delivery](interest-and-delivery.md), [network](network.md#server-messages), [sync-pipeline](sync-pipeline.md#receive-state) |
| Chat | [ui-overlay](ui-overlay.md#chat), [network](network.md#chat) |
| Settings | [ui-overlay](ui-overlay.md#settings-window), [configuration](configuration.md) |
| Item picker and preview | [ui-overlay](ui-overlay.md#item-picker), [appearance-and-equipment](appearance-and-equipment.md#item-filter-and-catalog) |
| Replay and load testing | [recording-and-replay](recording-and-replay.md) |
