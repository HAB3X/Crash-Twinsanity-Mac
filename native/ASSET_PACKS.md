# Asset packs (version 1.0)

An asset pack carries one game's assets into another. Its contents:

- scenery and level geometry;
- textures;
- collision;
- placeable objects;
- spawn points;
- moving platforms;
- animations;
- sounds and music;
- enemy behaviour descriptors.

Every engine writes the same format: Wrath of Cortex's NU2, the Radical games' Pure3D, and later ones. Twinsanity's native port
and Twinsanity Studio both read it. A pack is made on the player's machine from their own disc by a converter
(`native/tools/<game>_assets.py`). Nothing of a game goes into the repository.

This extends the character pack (`TWCP`, `PACK_VERSION` 1, `native/tools/woc_character.py`; native/CHARACTERS.md on
woc-character). A character stays a character pack, and an asset pack refers to character packs by folder.

## A pack

A pack is a folder, `<settings folder>/packs/<pack id>/`:

- `pack.json`: the manifest (JSON, UTF-8);
- `geometry.bin`: textures, meshes, collision and animations, in sections ("Binary blobs" below);
- `sounds/*.wav`: the sounds and music, as WAV files;
- anything else a later version adds, named by the manifest.

The settings folder is `$HOME/Library/Application Support/Crash Twinsanity` on macOS, and `$XDG_CONFIG_HOME/crash-twinsanity`
or `~/.config/crash-twinsanity` elsewhere. The pack id is lowercase letters, digits and `_`, the source game's prefix first
(`cttr_dino_wood`, `woc_jungle_bungle`).

## Versioning

- The manifest names its version as `"version": [major, minor]`. Each binary section has its own version (16 bit).
- **A reader:**
  - refuses a pack whose major version it doesn't know;
  - takes any minor version, ignoring the keys, section kinds and record fields it doesn't know.
- **A minor version only adds:**
  - keys;
  - section kinds;
  - fields at the end of a record, which the record's size says to skip.
- **A major version is anything else:** a field moved or reinterpreted, a unit changed, a meaning changed.
- **A writer:**
  - writes the lowest version that holds what it writes;
  - names itself in `"source"`.
- **The document:** this file is the specification. A change to it states the version it brings in, in "Changes" at the end.

## Space and units

Positions are in the pack's space:

- **Y is up.** A character or an object faces **-Z**, and its left is **+X**. This is the character pack's space, and the
  space Pure3D and NU2 already use, so neither needs to mirror anything.
- **Units are the source game's own.** The manifest's `"space"` gives two references, so a reader can scale to its own world:
  - `"unitsPerMetre"`: the converter's best estimate;
  - `"playerHeight"`: the source game's player character's standing height in these units. The usual scale is the reader's
    own player height over this.
- **Front faces:** a triangle's front face is the one where its corners, as listed, go counterclockwise. That is, the cross
  product `(b - a) x (c - a)` points out of the front. For collision, the front is the side a character can stand or push on.
- **Rotations** are quaternions `[x, y, z, w]`.
- **Times** are in seconds. Animation frames are at the clip's own rate.
- **Colours:**
  - RGBA, 8 bits a channel in the binary, 0-1 floats in the manifest;
  - 255 (1.0) is full;
  - vertex colours are prelit light: 255 means unlit-as-is (the reader multiplies the texture by them).

## The manifest

`pack.json` is a JSON object:

```json
{
  "format": "twinsanity-asset-pack",
  "version": [1, 0],
  "id": "cttr_dino_wood",
  "name": "Dino Wood (on foot)",
  "source": {"game": "Crash Tag Team Racing", "release": "SLUS_211.91", "file": "ONFOOT/ONFOOT1.RCF",
             "converter": "native/tools/cttr_assets.py"},
  "space": {"unitsPerMetre": 1.0, "playerHeight": 2.58},
  "geometry": "geometry.bin",
  "scenery": [...],
  "objects": [...],
  "placements": [...],
  "spawns": [...],
  "movers": [...],
  "sounds": [...],
  "music": [...],
  "behaviours": [...],
  "characters": [...]
}
```

Any list may be missing or empty. Meshes, collision, textures and animations are referred to by their index in their
`geometry.bin` section, counted from 0.

### Scenery

The level's static geometry, drawn and collided as it is, at the pack's origin:

```json
{"name": "track", "meshes": [0, 1, 2], "collision": [0]}
```

### Objects

A kind of placeable thing:

```json
{"id": "padjump", "meshes": [12], "collision": [3], "bounds": {"min": [-1, 0, -1], "max": [1, 0.5, 1]},
 "behaviour": "jump_pad", "parameters": {"height": 12.0}, "animations": [4], "sounds": {"use": "pad_boing"}}
```

- `meshes` and `collision`: the object's own, in its own space (placed by a placement).
- `bounds`: a box in its own space. A reader without collision for it can use the box.
- `behaviour`: what it does, as one of the names below, or the source game's own name when no common one fits (a reader that
  doesn't know it places the object as scenery). Its `parameters` are the behaviour's.
- `animations`: clips that play on it (looped unless the behaviour says otherwise).
- `sounds`: event name to sound id.

**Common behaviour names** (version 1.0):

| Name | Meaning |
|---|---|
| `scenery` | static |
| `pickup` | collected when touched (`parameters.kind`: "fruit", "gem", "crystal", "token", "life" or the game's own) |
| `crate` | broken by an attack or a bounce (`parameters.contents`, `parameters.bounce`) |
| `jump_pad` | throws the player up (`parameters.height`) |
| `door` | opens when its trigger or switch is met (`parameters.trigger`) |
| `switch` | a button (`parameters.target`: the placements it acts on) |
| `hazard` | hurts on touch (`parameters.damage`: "hit" or "death") |
| `water` | its volume is water (`parameters.deadly`) |
| `checkpoint` | saves the spawn |
| `npc` | a character that stands and talks (`parameters.character`) |
| `enemy` | runs an enemy behaviour descriptor (`parameters.behaviour`, "Behaviours" below) |

### Placements

Objects put in the level:

```json
{"object": "padjump", "position": [-25, 101, 18], "rotation": [0, 0, 0, 1], "scale": 1.0, "name": "padjump_3",
 "parameters": {}}
```

A placement's `parameters` override the object's.

### Spawns

```json
{"name": "start", "kind": "player", "position": [41, 83, 0], "rotation": [0, 0.7071, 0, 0.7071]}
```

`kind` is "player", "checkpoint", "enemy" or "camera".

### Movers

Moving platforms and other things on paths:

```json
{"placement": 7, "path": [[0, 0, 0], [0, 10, 0]], "times": [0, 2.5], "mode": "pingpong", "start": "always"}
```

- `placement`: the index into `placements` whose object moves.
- `path`: offsets from its placement.
- `times`: the time each point is reached.
- `mode`: "loop", "pingpong" or "once".
- `start`: "always", "touch" or a trigger name.

### Sounds and music

```json
{"id": "pad_boing", "file": "sounds/pad_boing.wav", "kind": "effect", "loop": false, "volume": 1.0}
```

- `kind`: "effect", "voice", "ambience" or "music".
- **Files:** WAV, 16 bit PCM, any rate, mono or stereo (more channels: the first two are used).
- **Music:** `music` lists the music's sound ids in the order the level plays them.
- **Positional sounds:** a placement or spawn can name one in `parameters.sound`.

### Behaviours

An enemy's behaviour descriptor: the source game's own state machine and numbers, written as data.

```json
{"id": "penguin_ambient", "model": "patrol_chase_attack",
 "source": "NPCBehaviourControllerAmbient (CTTR, SLUS_211.91 0x120640)",
 "parameters": {"sightRadius": 15, "sightHeight": 5, "attackRadius": 5.2, "attackHeight": 2, "attackCooldown": 0.4,
                "pushRadius": 30, "pushHeight": 15, "idleTime": [4, 8], "speed": 5, "chaseSpeed": 16, "stun": [0.5, 10]},
 "states": [{"name": "idle", "animation": "idle"}, {"name": "chase", "animation": "run"},
            {"name": "attack", "animation": "attack"}],
 "approximate": ["the attack's knockback direction"]}
```

- **`model`:** the shape of the state machine:

  | Model | What it does |
  |---|---|
  | `stationary` | stands, faces the player, may talk |
  | `patrol` | walks its path (`parameters.path` or a mover) |
  | `patrol_chase_attack` | idles or wanders, chases the player within sight, attacks within reach, goes back |
  | `guard` | holds a spot, attacks within reach |
  | `projectile` | shoots at the player (`parameters.rate`, `parameters.speed`) |

  A reader maps a model onto its own enemies' AI. When nothing fits, it uses `patrol_chase_attack` with the parameters it
  understands.
- **`states`:** documentation, plus the animations each state plays; the reader's model drives them.
- **`approximate`:** what the converter couldn't carry exactly.

### Characters

Characters (NPCs' and enemies' models, skinned and animated) are character packs:

```json
{"id": "parkworker", "pack": "../../characters/cttr_parkworker"}
```

Objects and behaviours name them by id. The path is relative to the asset pack's folder.

## Binary blobs: `geometry.bin`

Little-endian throughout. The file is a header, then sections, each self-describing:

```
file header (16 bytes):   char magic[4] = "TWAP"; u16 major = 1; u16 minor = 0; u32 sectionCount; u32 reserved = 0
section header (16 bytes): char kind[4]; u16 version; u16 recordSize; u32 count; u32 byteSize (the section's data after its header)
```

A reader skips a section of a kind it doesn't know, using `byteSize`. `recordSize` is the size of a section's fixed-size
records where it has them (0 otherwise). A reader skips the bytes past the fields it knows.

The sections, version 1. Indexes count from 0 within their section; -1 is none.

### `TEX ` textures

`count` textures, each:

- u16 width, u16 height (any size; readers may need powers of two and resample);
- u32 flags: 1 = has alpha to blend, 2 = alpha to test (cut-out), 4 = clamp instead of repeat;
- then width x height RGBA8, rows top to bottom.

### `MESH` meshes

`count` meshes, each:

- char name[32] (zero-padded);
- u32 materialCount;
- float boundsMin[3], boundsMax[3];
- then the materials.

Each material:

- i32 texture (-1 none), u32 flags, float colour[4], u32 vertexCount, u32 triangleCount;
- then vertexCount vertices of 36 bytes: float position[3], float normal[3], float uv[2], u8 colour[4];
- then triangleCount x u32 index[3].

The material flags:

| Flag | Meaning |
|---|---|
| 1 | blend (alpha) |
| 2 | double-sided |
| 4 | the vertex colours are prelit light (multiply, no lighting); otherwise lit by the normals |
| 8 | additive |
| 16 | no depth write |

UV (0, 0) is the texture's top left.

### `COLL` collision

`count` collision meshes, each:

- char name[32];
- u32 vertexCount, u32 polygonCount;
- float vertices[vertexCount][3];
- then the polygons, each: u32 surface, u16 cornerCount, u16 flags, then cornerCount x u32 vertex index.

A polygon is convex and flat, its front as for triangles. A triangle mesh is polygons of three corners.

`surface` is the source game's own surface type, kept as it is. Its low 8 bits are the common kind:

| Kind | Meaning |
|---|---|
| 0 | solid (floor or wall by its slope) |
| 1 | wall only (no standing) |
| 2 | ice (slippery) |
| 3 | water |
| 4 | death (falling out, lava) |
| 5 | hurt |
| 6 | no camera (collides with the player only) |
| 7 | trigger (no collision; a volume for events) |

The bits above 8 are the source's raw value, for a reader that knows the source game. Polygon flag 1: it moves (with the mover
whose placement uses this collision).

### `ANIM` animations

Rigid animations for objects (skinned ones are in character packs). `count` clips, each:

- char name[32];
- u32 nodeCount, u32 frameCount, float rate;
- then frameCount x nodeCount transforms, each 12 floats: a 4x3 matrix, row vectors, the translation in the last row.

Node 0 is the object as a whole. Later nodes are the meshes of its `meshes` list, in that order.

## Readers

### The native port

`src/platform/native/packs/` (cttr-character branch, version 1.0):

- **Reading:** it reads `pack.json` and `geometry.bin`.
- **Drawing:** it draws a pack's scenery and placed objects as static geometry, with the character packs' approach (the GS
  through the frame's chain, the textures in the renderer's slots).
- **Collision:** it adds the collision to the level's.
- **Choosing a pack:** `TWIN_ASSET_PACK=<id>` and `TWIN_ASSET_PACK_AT=x,y,z[,turn,scale]` place a pack in the running level.

### Twinsanity Studio

Studio reads the same files for its asset library.

## Writers

- **Crash Tag Team Racing:** `native/tools/cttr_assets.py`. The on-foot worlds' meshes, textures, collision (from
  `WorldDef.p3d`), placements, start, movers, sounds and music.
- **Wrath of Cortex:** to follow, by the Wrath of Cortex helper.

## Changes

- **1.0:** the first version.
