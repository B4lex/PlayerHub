# Player Appearance Synchronization

## Status

Implemented and working in game between two clients on Skyrim AE 1.7.104. A
remote puppet spawns from a runtime actor base built from the source's appearance
and shows that appearance. The remaining open items are listed under
[Engine assumptions](#engine-assumptions).

## Goal

A remote puppet looks like the source player's character: race, sex, body
weight, height, head parts (face, eyes and eye color, hair, beard, brows,
scars), hair color, face morph sliders and presets, face detail texture, skin
tone, and face tint layers (war paint, dirt, lip and cheek color, and so on).

Before this change every puppet used the *local* player's actor base, so every
remote player looked like the viewer.

## Non-goals

- Equipment, armor, and weapons. Equipment is presentation state with its own
  lifecycle and is specified separately.
- Character name, voice type, and actor values.
- Transformation races (werewolf, vampire lord) and their graphs. Capture only
  publishes playable races; while the source is transformed, remote peers keep
  the last playable appearance.
- RaceMenu/NiOverride data: body morphs, overlays, node transforms, and
  scaled bones. These live outside `TESNPC` and are specific to that mod.
- Custom skin armor (`TESNPC::skin`) and far skins. The resolved race's skin is
  used.
- Synchronizing appearance between runtimes other than 1.7.104, or on VR (tint
  capture is disabled on VR).

## Protocol

Appearance is reconstructable, rarely changing state. It is too large to repeat
in every 20 Hz `player:update` checkpoint, so it uses its own latest-wins event.
The client emits `player:appearance`:

```json
{
  "revision": 3,
  "race": { "plugin": "Skyrim.esm", "formId": 79686 },
  "female": false,
  "weight": 50.0,
  "height": 1.0,
  "headParts": [
    { "plugin": "Skyrim.esm", "formId": 324286, "type": 1 },
    { "plugin": "Skyrim.esm", "formId": 324287, "type": 3 }
  ],
  "hairColor": { "r": 59, "g": 42, "b": 30 },
  "faceDetails": null,
  "bodyTint": { "r": 222, "g": 188, "b": 170 },
  "faceMorphs": [0.0, 0.0, "... 19 floats total"],
  "faceParts": [0, 0, 0, 0],
  "tints": [
    { "index": 44, "type": 6, "r": 222, "g": 188, "b": 170, "alpha": 1.0 }
  ]
}
```

### Form references

Forms are referenced as `{plugin, formId}`, where `plugin` is the file that
defines the form and `formId` is the file-local ID (the low 24 bits, or the low
12 bits for a light plugin). Unlike `cellId` and `furnitureRefId`, appearance
references do **not** require matching load orders: peers with different
modpacks resolve by plugin name and fall back to vanilla data when a plugin or
form is missing. Dynamic (`FF`) forms cannot be referenced and are omitted.

### Fields

| Field | Meaning | Validation |
| --- | --- | --- |
| `revision` | Source-local monotonic counter | integer `1..2^63-1`, strictly increasing per connection |
| `race` | `TESRace` | form reference |
| `female` | Sex | boolean |
| `weight` | `TESNPC::weight`, body morph | number `0..100` |
| `height` | `TESNPC::height` multiplier | number `0.25..4` |
| `headParts` | Top-level `BGSHeadPart`s; extra parts are implied | at most 16; `type` `0..6` |
| `hairColor` | RGB of the `BGSColorForm`, or `null` | channels `0..255` |
| `faceDetails` | Face detail `BGSTextureSet`, or `null` | form reference |
| `bodyTint` | `TESNPC::bodyTintColor` RGB | channels `0..255` |
| `faceMorphs` | `FaceData::morphs` | exactly 19 finite numbers |
| `faceParts` | `FaceData::parts` presets | exactly 4 32-bit integers |
| `tints` | Face tint layers | at most 64; `index` `0..65535`, `type` `0..14`, `alpha` `0..1` |

Plugin names are 1..260 characters. Hair color is sent as RGB rather than a
form, because character editors commonly create dynamic color forms.

Unset face morphs hold `FLT_MAX` as a sentinel. The client's JSON parser is not
full precision, so that value can decode marginally above `FLT_MAX`; the client
clamps finite morphs to the float range instead of rejecting the payload. A JSON
`null` is accepted as either a null message or an absent value.

### Server

- Validates `player:appearance`; malformed payloads and non-increasing revisions
  are rejected with an acknowledgement error.
- Stores the latest appearance per connection and relays it to every other
  client as `player:appearance` with `playerId`.
- On `connect`, sends every stored appearance of the other connections to the
  new client, so late joiners do not wait for the next change.
- Forgets the appearance on disconnect.
- When recording is enabled, appends `player:appearance` to the recording.
  Replay accepts the event.

## Source capture

On the game thread, once per second, the client captures from the player:

- `race`: the actor base race. If it is not playable, `originalRace` is used if
  that is playable; otherwise capture is skipped (transformation).
- `female`, `weight`, `height`, `headParts[0..numHeadParts)`, the hair color
  form's RGB, `faceDetails`, `faceData` morphs/parts.
- `bodyTint`: `TESNPC::bodyTintColor`; if it is black, the color of the skin
  tone tint mask.
- `tints`: `PlayerCharacter` runtime `tintMasks`. Each mask is mapped to the
  race's `TintAsset` for the player's sex by texture (pointer, then
  case-insensitive path) to get its stable `index`. Masks with zero alpha or no
  matching asset are skipped.

Publishing policy:

- A capture that differs from the last published appearance must be identical
  in two consecutive captures (debounce), so dragging an editor slider does not
  stream updates.
- Publishes are at least 3 seconds apart.
- Each publish increments `revision`.
- On every transition to connected, the last appearance is re-published, because
  the server forgets a connection's state when it drops.

## Receiver

### Accepting updates

The network thread keeps only the newest pending appearance per player (by
revision) and discards it on `player:left`. The main-loop pass stores it on the
remote puppet. A rejected payload is logged with the name of the offending
field.

### Actor base

Every accepted appearance revision gets a new runtime `TESNPC`:

1. Create it with the `TESNPC` form factory, assign a dynamic form ID
   (`TESDataHandler::GetNextID`), and `Copy` the local player base. That keeps
   class, AI data, and factions identical to the puppets we had before.
2. Detach every appearance pointer the copy may share with the player base
   (`headParts`, `faceData`, `headRelatedData`, `tintLayers`, `faceNPC`) and
   allocate fresh ones, so the local player can never be changed.
3. Apply the resolved appearance (below).

Bases are never freed. A deleted puppet reference may still point at its base
for a few frames, and the leak is bounded by the source's publish rate.

### Resolution and vanilla fallback

| Data | Unresolvable case | Fallback |
| --- | --- | --- |
| Race | plugin/form missing, not a race, or not playable | `NordRace` (`Skyrim.esm` `013746`) |
| Head part | missing, wrong type, or its valid-race list rejects the race | race default head part of the same type and sex, else omitted |
| Face head part | none after resolution | race default face part |
| Hair color | color form creation fails | race default hair color |
| Face details | missing | race default face details texture set |
| Tint layer | race has no tint asset with that index and type | layer dropped |

Each fallback is logged once per revision.

### Spawn and respawn

- A puppet waits up to 3 seconds after its first checkpoint for an appearance.
  After that it spawns from the local player base, as it did before.
- A newer appearance revision than the one the puppet was spawned with retires
  the puppet at the next main-loop pass, as a coordinate-space change does.
  Respawn uses the new base and reconstructs locomotion, weapon stance, and
  furniture from the newest checkpoint. These respawns are at least 2 seconds
  apart per player.
- After the respawned actor is ready and its 3D is loaded, the receiver calls
  `Actor::UpdateHairColor` and `Actor::UpdateSkinColor` once.
- Respawns caused by coordinate-space changes reuse the current base.

## Engine assumptions

Verified in game:

1. `TESNPC::Copy` from the player base, with a `TESDataHandler::GetNextID` form
   ID, produces a spawnable actor base. `CreateReferenceAtLocation` completes 3D
   and graph initialization for it without the `PlayerBase` construction template.
2. For a dynamic form with no preprocessed FaceGen file, Skyrim builds the head
   at runtime from the base's head parts and face data.

Not yet specifically checked:

3. `TintAsset::texture.index` matches `TESNPC::Layer::tintIndex`, and
   `interpolationValue = alpha * 100` with the matching `preset` reproduces the
   source's war paint and face tint colors exactly.
4. The puppet animation firewall, combat suppression, and furniture entry behave
   the same with a copied base as with `PlayerBase`.

## Acceptance criteria

1. A remote puppet shows the source player's race, sex, weight, hair, eyes,
   hair color, face shape, and skin/face tints.
2. Late joiners see the correct appearance without the source changing anything.
3. Changing appearance (barber, RaceMenu) updates the remote puppet within a few
   seconds with a single respawn, and it then returns to its synchronized
   locomotion/furniture state.
4. A peer missing the source's race or head part plugin sees a vanilla fallback
   and no crash.
5. The local player's own appearance never changes because of a remote puppet.
6. Malformed or stale appearance payloads are rejected by the server and the
   client.
