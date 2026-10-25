# Specs

A retroactive summary of the implementation, organized by system, so humans and
agents can pick up context quickly. The code is the authority when a spec
disagrees with it. `AGENTS.md` describes how to keep these files current.

## Systems

| Spec | System | Sources |
| --- | --- | --- |
| [sync-pipeline.md](sync-pipeline.md) | Main-loop pass, game thread, peer receive state, checkpoint publishing and playout, world presence, actor lifecycle | `src/runtime/`, `src/sync/` (top level, `presence/`) |
| [network.md](network.md) | Wire protocol, client transport and connection, server connections, player sessions, and validation | `proto/`, `src/protocol/`, `src/network/`, `server/internal/{session,auth,admission,validate,wire,world}` |
| [interest-and-delivery.md](interest-and-delivery.md) | Which peers each client receives, exclusions, per-recipient outboxes, ordering, batching | `server/internal/{interest,hub,delivery}` |
| [remote-actor.md](remote-actor.md) | Spawn, update order, registry, animation firewall, graph recovery, survival, combat and reaction isolation, world isolation, light budget | `src/actor/` |
| [locomotion.md](locomotion.md) | Transform, movement state, direction, turning, ground offset, jumps, swimming, beast graphs | `sync/movement/`, `sync/animation/`, `actor/ActorMovement`, `actor/GroundProbe`, `actor/ActorAnimation`, `actor/JumpSound` |
| [combat-presentation.md](combat-presentation.md) | Weapon stance, attack action stream, power attacks, bashes, blocking, bow draws, spell casts, spell look, beast actions | `sync/combat/`, `sync/shapeshift/`, `actor/ActorAnimation`, `actor/ActorShapeshift`, `actor/BeastActions`, `sync/equipment/SpellStandIn` |
| [appearance-and-equipment.md](appearance-and-equipment.md) | Appearance, actor base, shapeshifts, player label, equipment, fake lights, default outfit, item filter and catalog | `sync/appearance/`, `sync/equipment/`, `sync/outfit/`, `src/forms/` |
| [native-interactions.md](native-interactions.md) | Furniture, mounts, death: where Skyrim drives the actor | `sync/furniture/`, `sync/mount/`, `sync/death/`, `actor/ActorFurniture`, `actor/ActorMount`, `actor/ActorDeath` |
| [trade.md](trade.md) | Item exchange between two players: invitation, plugin check, offers, reports, acceptance, commit, cancelling, the trade bot; the only change a peer makes to the local world | `sync/trade/`, `forms/ItemInstance`, `protocol/TradeState.h`, `server/internal/hub/trade.go`, `server/cmd/tradebot` |
| [configuration.md](configuration.md) | `PlayerHub.ini`, schema, snapshots, saving | `src/config/` |
| [ui-overlay.md](ui-overlay.md) | Overlay, input focus, backdrop, fonts, localization, theme, background opacity, menu frame, logo, nameplates, status panel, display name hint, notices, chat, friends, mute and vanish, player menu, player menu in the world, settings window, Discord link, item picker and preview | `src/ui/`, `src/text/`, `locale/`, `sync/chat/`, `sync/filters/`, `sync/nameplates/`, `sync/notices/`, `assets/` |
| [recording-and-replay.md](recording-and-replay.md) | Captures, virtual players, load testing, admin panel | `server/internal/{recording,playback,loadtest,admin}`, `server/cmd/`, `admin-ui/` |

## Features

Where each feature is described. The first spec owns its goal and acceptance
criteria.

| Feature | Specs |
| --- | --- |
| Movement and locomotion | [locomotion](locomotion.md), [network](network.md#checkpoint-and-context), [sync-pipeline](sync-pipeline.md#local-publishing) |
| Checkpoint playout (smooth remote movement) | [sync-pipeline](sync-pipeline.md#checkpoint-playout), [locomotion](locomotion.md#coalescing), [network](network.md#checkpoint-and-context), [interest-and-delivery](interest-and-delivery.md#batching) |
| Swimming | [locomotion](locomotion.md#swimming), [remote-actor](remote-actor.md#swimming) |
| Ground offset (different landscape mods) | [locomotion](locomotion.md#ground-offset), [network](network.md#checkpoint-and-context) |
| Weapon stance and attacks | [combat-presentation](combat-presentation.md), [network](network.md#actions) |
| Bow draws and spell casts | [combat-presentation](combat-presentation.md#hand-presentation), [appearance-and-equipment](appearance-and-equipment.md#equipment) |
| Spell look in hand | [combat-presentation](combat-presentation.md#spell-look), [appearance-and-equipment](appearance-and-equipment.md#equipment), [network](network.md#equipment) |
| Blocking, bashes, power attacks | [combat-presentation](combat-presentation.md) |
| Appearance and player labels | [appearance-and-equipment](appearance-and-equipment.md#appearance), [network](network.md#appearance-and-equipment) |
| Sessions and display names | [network](network.md#sessions), [appearance-and-equipment](appearance-and-equipment.md#display-name), [configuration](configuration.md#connection), [ui-overlay](ui-overlay.md#connection-controls) |
| Equipment and default outfit | [appearance-and-equipment](appearance-and-equipment.md#equipment), [network](network.md#equipment), [configuration](configuration.md#defaultoutfit) |
| Furniture | [native-interactions](native-interactions.md#furniture), [remote-actor](remote-actor.md#placement-ownership) |
| Mounts | [native-interactions](native-interactions.md#mounts), [sync-pipeline](sync-pipeline.md#actor-lifecycle) |
| Death | [native-interactions](native-interactions.md#death) |
| Shapeshifts (werewolf, vampire lord, werebear) and beast actions | [appearance-and-equipment](appearance-and-equipment.md#shapeshifts), [combat-presentation](combat-presentation.md#beast-actions), [locomotion](locomotion.md#beast-graphs), [network](network.md#actions), [sync-pipeline](sync-pipeline.md#actor-lifecycle), [trade](trade.md#invitation) |
| World presence, fast travel, spawn throttling | [sync-pipeline](sync-pipeline.md) |
| Combat and reaction isolation, Precision compatibility | [remote-actor](remote-actor.md#combat-isolation), [remote-actor](remote-actor.md#precision), [sync-pipeline](sync-pipeline.md#frame-sequence) |
| World isolation and collision | [remote-actor](remote-actor.md#world-isolation), [native-interactions](native-interactions.md#collision) |
| Light budget for remote players' torches and spells | [remote-actor](remote-actor.md#light-budget), [configuration](configuration.md#advanced), [sync-pipeline](sync-pipeline.md#frame-sequence) |
| Fake lights for remote torches and spell hand lights | [appearance-and-equipment](appearance-and-equipment.md#fake-lights), [combat-presentation](combat-presentation.md#spell-look), [remote-actor](remote-actor.md#light-budget) |
| Connection, latency, traffic | [network](network.md#connection), [ui-overlay](ui-overlay.md#status-panel) |
| No connection during the vanilla Helgen intro | [network](network.md#vanilla-intro-hold), [sync-pipeline](sync-pipeline.md#vanilla-intro), [ui-overlay](ui-overlay.md#connection-controls), [ui-overlay](ui-overlay.md#display-name-hint) |
| Display name hint | [ui-overlay](ui-overlay.md#display-name-hint), [configuration](configuration.md#controls) |
| Official and custom server | [configuration](configuration.md#official-and-custom-server), [network](network.md#connection), [ui-overlay](ui-overlay.md#settings-window) |
| Player cap and join limits | [network](network.md#admission), [ui-overlay](ui-overlay.md#connection-controls) |
| Session management in the admin panel (list, rename, delete) | [network](network.md#sessions), [recording-and-replay](recording-and-replay.md#admin-api) |
| Kicks and bans | [network](network.md#kicks-and-bans), [recording-and-replay](recording-and-replay.md#admin-api), [ui-overlay](ui-overlay.md#notices), [ui-overlay](ui-overlay.md#connection-controls) |
| Proximity, exclusions, late join, peer enter and leave, batching | [interest-and-delivery](interest-and-delivery.md), [network](network.md#server-messages), [sync-pipeline](sync-pipeline.md#receive-state) |
| Chat | [ui-overlay](ui-overlay.md#chat), [network](network.md#chat) |
| Nameplates above players' heads | [ui-overlay](ui-overlay.md#nameplates), [sync-pipeline](sync-pipeline.md#frame-sequence), [configuration](configuration.md#ui), [appearance-and-equipment](appearance-and-equipment.md#player-label) |
| Notices (toasts on the right) and chat notes | [ui-overlay](ui-overlay.md#notices), [ui-overlay](ui-overlay.md#notes), [trade](trade.md#notices) |
| Players online list | [ui-overlay](ui-overlay.md#player-list), [network](network.md#roster), [network](network.md#nearby-players) |
| Friends | [ui-overlay](ui-overlay.md#friends), [network](network.md#player-filters) |
| Mute and vanish | [ui-overlay](ui-overlay.md#mute-and-vanish), [network](network.md#vanish), [interest-and-delivery](interest-and-delivery.md#subscriptions) |
| Player menu in the world (Activate on a player) | [ui-overlay](ui-overlay.md#player-menu-in-the-world), [remote-actor](remote-actor.md#spawn), [sync-pipeline](sync-pipeline.md#frame-sequence) |
| Player menu heading (name, race, level; inspection) | [ui-overlay](ui-overlay.md#player-menu), [network](network.md#inspection), [sync-pipeline](sync-pipeline.md#frame-sequence), [appearance-and-equipment](appearance-and-equipment.md#resolution-and-fallback) |
| Settings | [ui-overlay](ui-overlay.md#settings-window), [configuration](configuration.md) |
| Discord invitation on the Connection page | [ui-overlay](ui-overlay.md#discord-link) |
| Accent color | [ui-overlay](ui-overlay.md#accent-color), [configuration](configuration.md#ui) |
| Scale | [ui-overlay](ui-overlay.md#scale), [configuration](configuration.md#ui) |
| Background opacity and blur | [ui-overlay](ui-overlay.md#background-opacity), [configuration](configuration.md#ui) |
| Key bindings | [ui-overlay](ui-overlay.md#key-bindings), [ui-overlay](ui-overlay.md#input-focus), [configuration](configuration.md#controls) |
| Input language switching while typing | [ui-overlay](ui-overlay.md#input-language) |
| Localization and the language setting (the plugin's text in the game's or a picked language, from translation files anyone can add) | [ui-overlay](ui-overlay.md#localization), [ui-overlay](ui-overlay.md#translation-files), [configuration](configuration.md#ui), [configuration](configuration.md#schema), [trade](trade.md#item-details) |
| Item trade | [trade](trade.md), [network](network.md#trade), [ui-overlay](ui-overlay.md#trade-window), [sync-pipeline](sync-pipeline.md#frame-sequence), [interest-and-delivery](interest-and-delivery.md#ordering), [configuration](configuration.md#controls) |
| Item picker and preview | [ui-overlay](ui-overlay.md#item-picker), [appearance-and-equipment](appearance-and-equipment.md#item-filter-and-catalog) |
| Replay and load testing | [recording-and-replay](recording-and-replay.md) |
