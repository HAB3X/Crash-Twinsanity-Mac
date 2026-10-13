# Characters from other games

The native port can draw another game's character in place of Twinsanity's Crash. The character comes from a **character pack**
that a converter makes on your machine from your own disc. The first is Crash from Crash Bandicoot: The Wrath of Cortex (WoC,
Traveller's Tales' NU2 engine, PS2).

Twinsanity's own Crash keeps playing underneath: his movement, physics, collision, scripts, effects and shadow are Twinsanity's.
Only the picture changes, and the pack's animations are picked from what Twinsanity's Crash plays. This is stage 1 of
`~/Documents/decomps/ROADMAP.md`; stage 2 (WoC's own movement) is at the end.

No game data is in the repository. Packs live in the settings folder and are never committed or shipped.

## Using it

1. Make the pack from your WoC disc image (the `.bin` of a raw MODE2/2352 dump, or an `.iso`):

   ```sh
   python3 native/tools/woc_character.py "/path/to/Crash Bandicoot - The Wrath of Cortex (USA) (v1.01).bin"
   ```

   It writes `characters/woc_crash/pack.bin` in the settings folder (`~/Library/Application Support/Crash Twinsanity` on macOS,
   `$XDG_CONFIG_HOME/crash-twinsanity` or `~/.config/crash-twinsanity` elsewhere). It takes about 8 seconds and needs only
   Python 3.

2. Pick the character on the options' Characters tab, or for tests set `TWIN_CHARACTER=woc_crash` (it wins over the option).
   The option is kept in `options.txt` as `character=<pack folder>` (empty for Twinsanity's Crash).

The Characters tab (`ui/settings.cpp`, `ui/settingsview.cpp`):

- **Character:** Twinsanity Crash, then every installed pack. A pack's name is its `name.txt`'s first line when it has one,
  else made from its folder (`woc_crash` is "Wrath of Cortex Crash", `cttr_crash_japan_caveman` is "Tag Team Racing Crash:
  Japan Caveman"). It applies at once, in the menus and in play.
- **The preview:** the chosen pack drawn large, at 1.5 times the overlay's reference size, on the CPU (`RenderPreview` in
  `graphics/characters.cpp`). It plays a showcase (idle, run, idle, a taunt or fidget) and turns slowly; dragging it with the
  mouse turns it, and it starts turning by itself again two seconds after the mouse lets go. The camera fits everything the
  showcase does. Twinsanity's own Crash has no preview card yet: his model is the game's, loaded with a level.
- **Movement:** Twinsanity (its movement on the new model) or Original (the pack's own game's), shown only for a pack. Original
  is marked "not yet" until a pack's own movement is wired in; Twinsanity's is played meanwhile.
- `$TWIN_UI_SETTINGS_SHOTS` takes the tab's shots too (`settings_characters*`).

Converter options:

| Option | What |
|---|---|
| `--out DIR` | Write the pack somewhere else |
| `--preview DIR` | Also draw PNG strips and animated GIFs of the idle, run, jumps, spin, slide and more, from the pack's own data (needs numpy and Pillow) |
| `--list` | List the character's files in `CHARS.DAT` |
| `--character NAME` | Another of `CHARS.DAT`'s `chars\a\<name>` characters (only Crash is checked) |

Test switches in the port:

| Variable | What |
|---|---|
| `TWIN_CHARACTER=<folder>` | The pack drawn for Crash |
| `TWIN_CHARACTER_TRACE=1` | Log every change of Twinsanity's animation slot, the clip shown, and the stepping clips' pace |
| `TWIN_CHARACTER_TURN=<degrees>`, `TWIN_CHARACTER_SCALE=<x>` | Override the model's turn (180) and scale (the heights' ratio) |
| `TWIN_SWEEP_INPUT=<steps>` | The level sweep's input scripted: `FROM-TO:BUTTONS:DEGREES:PUSH;...` (buttons `X S C T` or `-`), looping |
| `TWIN_SWEEP_FRAME_SECONDS=<s>` | The sweep's play time counted by frames, not the wall clock (keeps a slowed run's input in step) |
| `TWIN_SNAPSHOT_EVERY=<s>`, `TWIN_SNAPSHOT_WIDTH=<px>` | `--snapshots`' spacing (0: every frame) and width |

## The converter (native/tools/woc_character.py)

What it reads, all worked out on the USA v1.01 disc:

- **The disc:** ISO 9660 read from the raw sectors (user data at byte 24 of each 2352-byte MODE2 sector).
- **`CHARS.DAT`:** NU2's `.dat` archive. The first word is the table's offset. The table has a version, a count, an
  (offset, size, unpacked size, packed flag) record per file, then a tree of names. A folder node holds its last child's index;
  a file node holds its negated file number; sibling links go backwards. Packed files are RNC ProPack method 2.
- **`chars\a\crash\crash.ghg`:** the PS2 loader's memory image of the model (`NuGHGRead`), with file offsets for pointers.
  - Header: counts and offsets of textures, materials, joints (`NUJOINTDATA` records of 0x60 bytes, then the local bind
    matrices `T` and inverse bind matrices `INV_WT`), the joint and point-of-interest indexes, and the layers.
  - **Textures:** each record is a header (width, height, the formats), then two GS uploads: the palette (16x16 or 8x2,
    32-bit) and the texels. 8-bit textures over 64 pixels wide are uploaded as 32-bit pixels and are un-swizzled through the
    GS's block layouts. The palette's index bits 3 and 4 are swapped (CSM1), and alpha 0x80 is opaque.
  - **Materials:** `numtl_s`. The texture number is at 0x1A4, counted from 1 (0 is none). The diffuse colour is at 0x180
    (0.5 shows the texture as it is). The alpha mode is in the attribute bits 0-3.
  - **Skins:** each layer (`NULAYERDATA`) has a skin and a blend-shape skin. A skin is a list of geoms (next, material, DMA
    chain), and each geom's chain has a CNT, REFs of the joints' matrices for VU1 (the address over 0x40 is the joint: that's
    the palette), and a CALL of a packet. A packet is VIF code in batches of up to ~30 strip vertices:
    - A vertex is two quadwords: `(x, y, z, normal x)` and `(u, v, normal y, normal z)`. The low bytes of the last five floats
      are flags, and bit 0 of the last one is the strip's ADC (no triangle ends at this vertex).
    - An S-16 pair in a vertex's place takes the batch's vertex `k` again (counted from 1), with its own ADC.
    - The weights go to the vertex's third and fourth quadwords: floats whose low 3 bits are the matrix's slot in the palette.
    - Blend shapes' offsets (VU addresses 0x38 and 0x39) aren't converted.
- **`chars\a\crash\<group>\<action>.ani`:** `nuanimdata_s`. Per 32-frame chunk, per joint, nine curves (translation;
  rotation in radians, applied X then Y then Z; scale) of cubic Hermite keys `(time, 1/span, slope, value)` or constants. A
  joint's local matrix follows `NuAnimCurveSetApplyToJoint2`: rotation, the rest orientation (flag 0x20), scale (flag 8),
  translation, the locator pivot (the joint's flag 8), then Z negated. A joint a clip leaves alone keeps its bind matrix. The
  clips run at 30 frames a second.

Checked with the converter's previews (the bind pose against the textures, then idle, run, jump, spin, slide and more skinned;
a mesh that tore pointed out the weights' slot bits) and in game.

## The pack (format version 1)

One file, `pack.bin`, little-endian. The model space is the source game's (WoC: Y up, facing -Z, Crash about 1 unit tall).
Matrices are 4 rows of 3 floats, for row vectors: the x, y and z axes, then the translation.

| Part | Layout |
|---|---|
| Header (64 bytes) | `"TWCP"`, `u32 version` (1), `u32` joints, textures, materials, animations, `f32` bounds min[3], max[3], 16 reserved bytes |
| Joints (84 bytes each) | `char name[32]`, `s32 parent` (-1 root), `f32 inverseBind[12]` |
| Textures | `u16 width, height` (powers of two, 8 to 512), then RGBA8 rows (alpha 255 opaque) |
| Materials | `s32 texture` (-1 none), `u32 flags` (1 alpha-blended), `f32 colour[4]` (multiplies the texture; 1 as it is), `u32` vertex and triangle counts, the vertexes, then `u16[3]` triangles (counterclockwise seen from outside: their cross product along the vertex normals) |
| Vertex (52 bytes) | `f32 position[3], normal[3], uv[2]` (v down the texture), `u8 joints[4]`, `f32 weights[4]` (summing to 1) |
| Animations | `char name[32]` (WoC's `group/action`, e.g. `a/run`), `u32 frames`, `f32 rate` (frames a second), then frames x joints skinning matrices (inverse bind times the joint's place in the model) |

Skinning matrices are stored instead of joint rotations, so a reader needs no knowledge of the source engine's skeleton. They're
interpolated as rotations about each joint's bind place (from the inverse bind matrix), so fast turns don't shrink the model.

Textures: anything up to 64 KB (128x128 RGBA) goes in one of the renderer's 20 ring slots of GS memory, like the game's own.
Bigger ones (256x256 and 512x512, up to 1 MB) go in its large textures' slot (1.25 MB), shared with the game's wide textures.
The Tag Team Racing helper's packs (256x256) load.

## Drawing in the port

`src/platform/native/graphics/characters.cpp`, hooked into `ModelNode::Draw` (`src/game/ogi.cpp`, under `TWIN_NATIVE`) for the
instance that is the progress's Crash:

- The pose is computed on the CPU: the clip's matrices at the time, blended with the last pose over 0.12 s when the clip
  changes. The vertexes are skinned with it, placed (turned 180 degrees about Y and scaled to Twinsanity Crash's OGI height,
  about 1.95 against WoC's 1.0), and lit by the chunk's ambient and three strongest lights.
- Triangles are clipped in clip space (near and far planes, the VU programs' guard band) and projected with the view's own
  matrices, so depth matches the game's buffer.
- They go to the GS as PACKED ST/RGBAQ/XYZ2 triangle lists in a chain of the port's own, CALLed from the bucket of the player's
  own skin. The chain sets every GS register it uses (TEX0/1, CLAMP, PRMODE, ALPHA, TEST, ZBUF, FBA), and the bucket's next
  material sends its own again.
- Textures are uploaded into the renderer's slots through `ClaimTextureSlot` (`renderer/textures.cpp`), the same ring the
  game's textures use.

Nothing is drawn outside the emulated PS2 pipeline, so the hardware and software renderers, the internal resolution and the
texture filtering options all apply to it.

## The mapping

Twinsanity's Crash plays animations from his game object's animation slots. The port finds the slot of the root's strongest
playing animation and shows a clip for it (`characters_mapping.inc`). The slots were found by tracing them during scripted play
on the beach.

| Slot | Twinsanity | WoC clip | How |
|---|---|---|---|
| 8 | standing | `b/idle` | looped |
| 9 | creeping (stick pushed a little) | `a/tiptoe` | looped, paced |
| 10 | walking | `a/walk` | looped, paced |
| 11 | running | `a/sprint` | looped, paced |
| 15 | spin attack | `d/cptrspin` | fitted, turned 3 times |
| 16 | falling | `f/fall` | looped |
| 19 | jump from standing | `b/jump` | fitted |
| 20 | jump while running | `a/runjump` (WoC's somersault) | fitted |
| 21, 22 | double jump | `a/starjump` | fitted |
| 29 | landing still | `a/land` | once |
| 30 | landing on the run | `a/run` | looped, paced |
| 31, 32, 35 | crouching down, crouched, standing up | `a/crchdwn`, `a/crchidle`, `a/crchstnd` | fitted, looped, fitted |
| 34 | crawling | `a/crawl` | looped, paced |
| 36, 38 | slide start, sliding | `a/slidattk` | once, continuing |
| 97, 98, 99 | idle fidgets | `b/scratch`, `e/pickear`, `b/sigh` | fitted |
| others | | `b/idle` | looped |

- **Looped** clips move on with the character's game clock, so they stop in the pause.
- **Paced** clips (the ones that step) play at the speed Crash moves over the ground, divided by the clip's own stride. The
  stride is measured from the feet's travel when the pack loads. The pace is kept between a third and 2.5 times.
- **Fitted** clips are stretched over Twinsanity's own animation.
- **Once** clips play through and hold their last frame.
- Packs without a clip fall back (`a|b` in the table), so other games' packs that name their clips like WoC's work too.

## What's approximate

- **Movement is Twinsanity's.** Speeds, jump heights and timings are Twinsanity's, so WoC's clips are fitted or paced to them.
  Twinsanity runs about twice as fast over the ground as WoC's run cycle, so the run uses WoC's sprint.
- **The spin.** WoC's spin draws a separate tornado model (`crshspin.ghg`) over a stretched pose. The port shows WoC's
  arms-out copter spin, turned three times, under Twinsanity's own spin effect.
- **Only on foot.** Slots not traced yet (vehicles, deaths, hurt, ledge hangs, pushes, cutscene-only animations) show the idle,
  and so do scripted cutscenes. Exit points, attachments, the shadow volume and collision stay Twinsanity Crash's.
- **No blend shapes.** WoC's face shapes (`.bsa`) and the blend-shape offsets aren't converted, so the face doesn't emote.
- **Lighting.** The lighting is the chunk's ambient plus its three strongest lights, halved because WoC's textures are full
  intensity where Twinsanity's are stored at half. Twinsanity's VU lighting program isn't reproduced exactly.
- **Blended materials** (the eyelids, the spikes' edges) are alpha-blended with an alpha test, and drawn after the opaque ones.
  WoC drew them in two passes.
- **Turning.** The facing is Twinsanity's, and there are no turn or lean clips.

## Stage 2: WoC's own movement

The options' "Movement" choice has "Original" for this, marked not available yet. To make it:

1. **Tuning.** WoC's `MoveInfo` (`crashwoc-decomp-gc/src/gamecode/creature.h`, 0x48 bytes):
   - speeds: idle, tiptoe, walk, run, sprint, slide, crawl, dangle, wade;
   - jump height, dangle gap;
   - frame counts: three jump stages, star jump, somersault, spin, spin reset, super spin and its wait, slam wait, slide,
     crouch, jump land.
   `LoadCharacterModels` derives the land and slam frames from the clips' lengths. `CrashMoveInfo` is at GameCube
   `0x8013CE50`. The USA PS2 executable (`SLUS_202.38`) is stripped, so find the table by its pattern of floats and shorts. Read
   it from the disc at run time, like the pack: tables copied out of the executable are game data.
2. **The logic.** `MovePlayer` (C in the GameCube decomp, a draft in the PS2 decomp's `creature.c`) and `MoveCRASH` (still
   assembly everywhere; PS2 PAL v1.03 at 0x250A98).
   - Decompile `MoveCRASH` from the user's own executable, with Ghidra's EmotionEngine module and the decomps' symbol maps for
     names.
   - Rewrite its behaviour as the port's own C++, and cite the retail addresses. The decomps are GPL or unlicensed: read
     them, don't copy them.
3. **Collision.** `MovePlayer` asks WoC's terrain code for floors, walls and platforms. An adapter has to answer those questions
   from Twinsanity's collision (`collision.cpp`, the character controllers' ground tests).
4. **Units.** WoC's numbers are per 60 Hz frame and in WoC units. Crash is about 1 unit tall there and 1.95 in Twinsanity, so
   scale distances by about 1.95, and frame counts by the game's rate.
5. **A new character kind** (`PlayableCharacter` 6 or up) with its own controllers, or a mode of Crash's, chosen by the option.
   Twinsanity's scripts still expect Crash's slots and exit points.
