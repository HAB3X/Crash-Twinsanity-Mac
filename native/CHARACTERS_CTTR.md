# Crash Tag Team Racing's on-foot Crash

`native/tools/cttr_character.py` makes Crash Tag Team Racing's on-foot Crash into a character pack, in the same format as
`native/tools/woc_character.py` writes (`PACK_VERSION` 1, the `TWCP` file; native/CHARACTERS.md describes it). The port loads it
like the Wrath of Cortex pack. Nothing of the game's goes into the repository: the tool reads the player's own disc image.

    python3 native/tools/cttr_character.py "/path/to/Crash Tag Team Racing.iso" [--costume NAME] [--out DIR] [--preview DIR] [--list]

- The pack goes to the settings folder's `characters/cttr_crash/` (`characters/cttr_crash_<costume>/` for another costume).
- `TWIN_CHARACTER=cttr_crash`, or the Characters page, picks it.
- `--list` prints the costumes and the animation clips.
- `--preview` draws frames (PNG) and animated GIFs of the idle, run, walk and jump from the pack's own data.
- `--max-texture N` halves textures with a side over N (by default they keep their full size).

Worked out on the USA disc (SLUS_211.91). Converting the classic costume takes about 15 s and writes 8.6 MB.

## The disc's archives (RCF)

The files sit in RCF archives, signed "ATG CORE CEMENT LIBRARY", version 2.1.

**The header:**

- 0x00: the signature, padded to 32 bytes.
- 0x20: the version bytes, `02 01 00 01`.
- 0x24: the directory's offset and size.
- 0x2C: the name table's offset and size.
- 0x34: a word.
- 0x38: the file count.

**The directory** has 12 bytes a file: a hash, an offset and a size, sorted by hash.

**The name table** starts with two words. Then, a file each:

- a time stamp;
- the alignment (0x800);
- a word;
- the name's length, counting its 0;
- the name;
- three bytes of padding.

The names aren't in the directory's order. A name's hash ties it to its directory entry:

    h = h * 31 + toupper(c) + 32

This is over the name's characters, kept to 32 bits. It was found from the `.rsd` sound files: their names differ in one hex
digit, four characters from the end, and their hashes differ by multiples of 31^4. Every name on the disc matches (COMMON.RCF has
two directory entries with no name).

**Where Crash is:**

- `ADEFAULT/DEFAULT.RCF` (1.9 GB, 2888 files) holds every costume and every animation set.
- `ONFOOT/ONFOOT*.RCF` and the world RCFs hold copies of the files their levels need.

The tool reads DEFAULT.RCF first.

## Pure3D

A Pure3D file starts with `P3D\xff`. A chunk is:

- its ID;
- its header size (its own data);
- its total size, its children included.

Names are a length byte, then the text, padded with zeros. These are the chunks the tool reads.

**Texture** (0x19000), containing an image (0x19001: width, height, depth, palettized, has alpha, format 12 "PS2 8 bit"),
containing the image data (0x19002). The image data is a size word, then a `P3DI` block:

- the storage format (14: 32 bit);
- the width, height and depth;
- the palette's size (1024) and the texels' size;
- the palette: 32 bit, alpha 0x80 opaque, with index bits 3 and 4 swapped (the GS's 16x16 CLUT layout);
- the 8 bit texels, as GS memory holds them after an upload as 32 bit pixels at half the width and height.

The tool reads the texels back through the GS's PSMCT32 and PSMT8 address layouts, as `woc_character.py` does for Wrath of
Cortex's. Every costume's textures are 8 bit: the classic one has one 256x256 (`crash2.bmp`); the others have a 64x64 outfit
texture and often a 128x128 head.

**Shader** (0x11000): its name, a version, the PDDI shader ("simple") and a translucency flag. Its parameters are children:

- `TEX` (0x11002): the texture's name;
- `LIT`, `CBVP`, `CBVY`, `BLMO`... (0x11003): integers;
- `DIFF` (0x11005): a colour, 0xAARRGGBB.

**Skeleton** (0x23000, "Skeleton 2"): its name, then a version, the joint count, the partition count and the limb count.

- The joints are children (0x23001): a name, the parent's index (its own index for the root) and a rest pose.
- The rest pose is a 4x4 matrix in the parent's space, with row vectors and the translation in the last row.
- The classic Crash has 39 joints: Motion_Root, Balance_Root, Character_Root, Pelvis, the legs (Hip, Knee, Ankle, Ball), Spine_2,
  the arms (Shoulder, Elbow, Wrist), some finger joints, `item` (a hand prop's place: the chicken leg), Neck, Jaw, Nose, Brows,
  Ears, Hair and Eyes.
- The costumes have 33 to 56 joints. The Japanese Crash has 70, for his own face rig.

**Skin** (0x10001): its name, a version, its skeleton's name and the primitive group count. Then:

- a vertex compression hint (0x10021);
- primitive groups (0x10020): a version, the shader's name, the primitive type (1, a triangle strip), the vertex type, the vertex
  count, the index count, the matrix count, then the memory-imaged and optimized flags. Each group's children are:
  - a vertex shader (0x10011, empty);
  - a memory image vertex description (0x10014);
  - the PS2 memory image (0x10012), a word, a parameter and a size, then VIF codes ("The PS2 memory image" below);
  - the matrix palette (0x1000D): a count, then joint indexes;
  - its bounds;
- a sort order (0x122000), a render status (0x10017) and the skin's bounds.

The shadow (`shadow_crbShape`, 0x10019) is a separate low-detail skin that the game draws flattened. The composite drawable
(0x123000) lists the skin under the skeleton. The tool takes the one skin.

**Animation** (0x121000): a version, a name, the kind (`PTRN`), the frame count (a float), the rate (30) and a cyclic flag (0 on
every clip). Its children are:

- a header (0x121006) with channel counts (0x121007);
- the group list (0x121002);
- sync frames (0x121402).

A group (0x121001) has a version, a name (a joint's, or an IK control's), an ID (its place in the rig's channel table, not a
joint index) and the channel count. The channels:

| Chunk | Parameter | Values |
|---|---|---|
| 0x121112 | `ROT` | Compressed quaternions: the key count, the key frames (16 bit), then x, y, z a key as 16 bit fractions of 32767; w is the positive rest |
| 0x121102 | `TRAN` | One animated coordinate: its index, the constants, the key count, the frames, a float a key |
| 0x121103 | `TRAN` | Two animated coordinates: the constant one's index, the constants, the key count, the frames, two floats a key |
| 0x121104 | `TRAN` | Three animated coordinates: the key count, the frames, three floats a key |

Each channel is followed by an interpolation mode chunk (0x121110: on). Rotations are slerped between keys and translations
interpolated linearly. A joint's local matrix is the quaternion as a row-vector rotation, with the translation in the last row.
A joint without a channel keeps its rest pose. The quaternion's matrix matches the rest poses exactly (the Nose, for one), so
its convention is certain.

## The PS2 memory image

The 0x10012 chunk holds the VU1 skin program's input, in batches.

**A batch:**

- It starts with an UNPACK of three quadwords to VU address 0: the vertex count (48 at most), words for the program, then the GIF
  tag (NLOOP the count, PRIM 4 a triangle strip, registers ST, RGBAQ, XYZF2).
- Then, with STCYCL 5/1 (4/1 for a rigid group), one UNPACK an attribute, a quadword slot of the vertex each.
- A batch's first two vertices repeat the last batch's last two: the strip runs on across batches. The decoded strip has exactly
  the group's vertex count.

**The slots:**

| Slot | Format | What |
|---|---|---|
| 3 | V2-32 (z and w masked to the column register: q = 1) | u, v. **u's lowest mantissa bit is the strip's ADC**: set on a new strip's first two vertices, so no triangle ends there. The exporter joins strips without repeated vertices; without the flag, long slivers join the feet, hands and body |
| 4 | V3-16, 4096 = 1 | The normal |
| 4 | V4-5 (instead) | A baked grey vertex colour (Buckethead's body), with no normal |
| 5 | V3-32 | The position, in the skin's model space (the bind pose) |
| 6 | V4-8 unsigned, STMOD offset over STROW's matrix base | Matrix slots: the value over 4 is the matrix palette's row |
| 7 | V4-16, 4096 = 1 | Three weights for the x, y and z slots; the w slot takes 1 minus the three. Typical: (3072, 1365) on slots (m0, m2, m0, m2) gives 0.75 and 0.25. A rigid group has no weights: the x slot alone |

An untextured group (Buckethead's bucket: shader `lambert1`, no `TEX`, `DIFF` 0xCCCCCCFF) has u and v at 0, so it has no ADC
flags. Its strips join through repeated vertices, which make degenerate triangles that are dropped.

## The pack

- **Space:** Pure3D's Crash already has the pack's axes: Y up, facing -Z, his left (`_L`) at +X. Only the size changes:
  `SCALE` 0.386 brings his 2.58 units (to the hair's tip) to Wrath of Cortex Crash's 0.996. The port scales a pack by its height
  anyway.
- **Triangles** are wound by their normals (`oriented`, as in `woc_character.py`). A group with vertex colours instead of normals
  gets normals from its faces, made to agree across shared edges and turned outward.
- **Textures** keep their full size: the classic costume's 256x256, the others' 64x64 and 128x128. `--max-texture N` halves
  them, each texel the mean of four, until no side is over N (`--max-texture 128` for a loader that takes only 64 KB a
  texture, as woc-character's did at fb124c9).
- **Materials:** one a primitive group, coloured by the shader's `DIFF`. Blending comes from the shader's translucency or `BLMO`,
  or the image's alpha.
- **Joints:** the skeleton as it is, with inverse bind matrices from the rest poses.
- **Clips:** the skinning matrices at every frame (30 a second), from frame 0 to the last key. Some clips need extra handling:
  - **The rig's IK controls are skipped.** These are `Right_Foot`, `Left_Knee`, `Right_Arm`, `Left_Elbow`, `Left_Arm_World` and
    the like. Every joint has its own forward-kinematics rotation keys, which match the IK solution as exported.
  - **Motion_Root's translation is dropped.** The run carries the distance covered on it, so without it the loops play in place.
  - **A loop's last key repeats its first.** On the idle and the edge-hang idle, frame 41 is frame 0. That frame is dropped, so a
    looped clip doesn't stop on it.
  - **`onfoot_mouthflap` is a partial layer.** It moves under half the idle's joints, so the idle fills the joints it leaves out.
    Otherwise the body would show the T pose.
  - **`onfoot_spin` animates the spin model's skeleton** (`Feet`, `Spine`, `Head`, `Arm_1`...), not this one, and is skipped
    ("What the pack can't express").
- **Names:** clip names are kept to 31 characters by the format, so `onfoot_edgehang_springup_recover` becomes
  `onfoot_edgehang_springup_recove`.
- **Copies under Wrath of Cortex's names:** the port's mapping (`characters_mapping.inc`) shows Twinsanity's animations as WoC
  clip names, so the clips that do the same are copied under those names (`WOC_NAMES` in the tool). A pack loads in place of
  Wrath of Cortex's with no code of its own. The copies add about 1 MB.

## Costumes

All are in DEFAULT.RCF, as `art\characters\crash_<costume>_onfoot_model.p3d`. `--costume` takes the part in bold.

| Costume | Model | Joints | Notes |
|---|---|---|---|
| **classic** (default) | `crash_onfoot_model.p3d` | 39 | The usual Crash, one 256x256 texture, 2 groups |
| **blacktie** | `crash_blacktie_onfoot_model.p3d` | | Tuxedo |
| **buckethead** | `crash_buckethead_onfoot_model.p3d` | 56 | A bucket on his head (untextured, rigid on the head). The body has baked vertex colours, no normals. The bucket makes the pack taller, so the port draws him a little smaller |
| **caveman** | `crash_caveman_onfoot_model.p3d` | 33 | Loincloth, club on the back |
| **hiphop** | `crash_hiphop_onfoot_model.p3d` | | |
| **madscientist** | `crash_madscientist_onfoot_model.p3d` | | Lab coat, goggles |
| **naked** | `crash_naked_onfoot_model.p3d` | | |
| **nega** | `crash_nega_onfoot_model.p3d` | | Evil Crash |
| **raiders** | `crash_raiders_onfoot_model.p3d` | | |
| **skeleton** | `crash_skeleton_onfoot_model.p3d` | | |
| **startrek** | `crash_startrek_onfoot_model.p3d` | | |
| **japan** | `crash_japan_onfoot_model.p3d` | 70 | The Japanese release's redesigned Crash, with his own face rig |
| **japan_blacktie** ... **japan_startrek** | `japan_crash_<costume>_onfoot_model.p3d` | | The Japanese costumes (no `japan_nega` or `japan_skeleton`) |

The Japanese costumes take `art\animation\crash_japan_onfoot_animations.p3d`: 53 clips with the same names and different timings.
It has no `onfoot_death`, `onfoot_impact`, `onfoot_idle_arm_farts` or `onfoot_mouthflap`, so no `b/die` copy, and adds
`onfoot_scrambling_for_wampapumpa`. The others take `crash_onfoot_animations.p3d` (56 clips).
`crash_onfoot_midway_animations.p3d` (ONFOOT5.RCF) is a 48-clip subset. The racing model (`crash_model.p3d`), the spin model
(`crash_spin_model.p3d`) and their animations are for the kart and the spin.

## The animations and Twinsanity's states

| Tag Team Racing clip | Frames | Fight tree state | Twinsanity state | Copied as (WoC name) |
|---|---|---|---|---|
| `onfoot_idle` | 42 (41 looped) | `onfoot_idle` (Locomotion: idle, with walk at speed 1 and run at speed 2) | Idle | `b/idle` |
| `onfoot_walk` | 25 | Locomotion's walk | Walk, tiptoe | `a/walk` |
| `onfoot_run` | 17 | Locomotion's run | Run | `a/run`, `a/sprint` |
| `onfoot_jump_takeoff` | 7 | `onfoot_jump_takeoff` | Jump (its start) | first part of `b/jump`, `a/runjump` |
| `onfoot_jump_rise` | 15 | `onfoot_jump_rise` | Jump (going up) | second part of `b/jump`, `a/runjump` |
| `onfoot_jump_fall` | 13 | `onfoot_jump_fall` | Fall | `f/fall`, `g/longfall` |
| `onfoot_jump_landing` | 35 | `onfoot_jump_landing` | Land | `a/land` |
| `onfoot_jump_switch` | 9 | `onfoot_doublejump_hang` | Double jump's top | |
| `onfoot_doublejump_start` | 36 | `onfoot_doublejump_start` (a somersault) | Double jump | |
| `onfoot_doublejump_recover`, `_bounce` | 26, 21 | Its landing, a bounce | Land after a double jump | |
| `onfoot_bellyflop_antic`, `_fall`, `_land` | 9, 7, 29 | Belly flop | Body slam | `a/bodyslam` (antic + fall) |
| `onfoot_spin` | 27 | `onfoot_spinning` | Spin | Not in the pack: the spin model's skeleton |
| `onfoot_spinkick_recover`, `_recover_run` | 31, 17 | After the spin, standing or running | The spin's end | |
| `onfoot_edgehang_catch` | 27 | Grab a ledge | Ledge grab | |
| `onfoot_edgehang_idle` | 61 (60 looped) | Hang | Hang | `b/hngabout` |
| `onfoot_edgehang_left_*`, `_right_*` | 7 to 22 | Shuffle along the ledge | Hang and move | `b/hngwalk` (left loop) |
| `onfoot_edgehang_springup`, `_recover` | 13, 13 | Climb up | Pull up | `b/hngpulup` (both) |
| `onfoot_death` | 96 | `onfoot_resetting` | Death | `b/die` |
| `onfoot_impact` | 54 | `impact` (hit) | Hurt | |
| `onfoot_longidle_chickenkick`, `_nausea`, `_idle_arm_farts` | 293, 146, 246 | Long idle | Idle fidgets | |
| `onfoot_itchy` | 141 | | Idle fidget | `b/itch` |
| `onfoot_lipsync_*`, `onfoot_mouthflap` | 42, 51 | Talking in cutscenes (NIS) | | |
| The 21 acting clips (`onfoot_wave`, `_thumbs_up`, `_sad`, `_afraid`, `_confused`, `_begging`, `_blankstare`, ...) | 42 to 381 | Hub conversations | | |

Twinsanity's slide, crawl and crouch, swim and wade, tightrope and pole states have nothing matching in Tag Team Racing: the
port's mapping falls back to its default (the idle).

## What the pack can't express

- **The spin.** Tag Team Racing swaps Crash's model for `crash_spin_model.p3d` while he spins: a tornado with its own 10-joint
  skeleton (`Feet`, `Spine`, `Head`, `Arm_1`, ... with wobble joints), its own textures (motion blur) and its own clip
  (`onfoot_spin`). The format has one model and one skeleton. The spin-kick recovery clips are in the pack, but not the spin.
- **Vertex colours.** Buckethead's body has baked grey shading per vertex in place of normals. The pack has no vertex colours;
  the converter drops them and computes normals.
- **IK.** The clips carry the rig's IK targets (foot, knee, hand and elbow controls, `Left_Arm_World`). The pack has only the
  forward-kinematics result as exported, so no foot planting on slopes.
- **Loops and clip timing.** Every clip's cyclic flag is 0; the fight tree says what loops. The pack has no loop flag, and the
  port's mapping decides how each clip plays.
- **Layers.** The mouth flap and lip sync are layered over other clips in the game. The pack bakes the mouth flap over the idle.
- **Props and effects.** The chicken leg in `onfoot_longidle_chickenkick` is a character effect on the `item` joint
  (`design\characters\crash.god`: "Turn Chicken On"). The double jump and spin effects are particle effects. None are in the pack.

## Faithful movement

The pack changes only the picture: Twinsanity's own Crash still moves underneath. Moving like Tag Team Racing's Crash would take
three things.

**The fight tree is data, and readable.** `art\animation\fighttrees\CrashOnFoot.fig` is plain text. It has 28 state nodes
(`stateNode`), each with:

- its tracks: `Animation`, `Locomotion`, `SoundEvent`, `Event`;
- its exits (`exitCondition`), each to a branch (`/globalBank/CrashOnFoot/...`) on a condition group.

The conditions are `CharacterCondition` (`IsOnFoot`, `IsJapanese`) and `OnFootCharacterCondition`: `IsJumping`, `IsSpinning`,
`IsFalling`, `IsLanding`, `IsDoubleJumping`, `IsDoubleJumpReset`, `IsBellyFlopping`, `IsEdgeHanging`,
`IsEdgeHangingLeft`/`Right`, `IsClimbingUp`, `IsWalkingOrRunning`, `IsResetting`, `IsDrowning`, `IsIdleForALongTime`,
`IsFallingAfterHit`. `NodeIsDone` ends a state when its clip does. Read as a state machine, it gives the animation side exactly:

- **Idle** goes to jump_rise, spinning, edgehang_catch, jump_fall, resetting, the long idles or impact.
- **Jump:** takeoff, then rise. Rise goes to bellyflop, fall, double jump, landing or edge hang. Landing goes to idle.
- **Spin:** spinning goes on to `spinkick_recover_run` when moving, `onfoot_spinkick_recover` when not, or jump_rise.

The Locomotion track blends idle, walk and run by speed (`walkSpeed 1`, `runSpeed 2`, blend times 0.2 s), as choreo's
`LocomotionDriver` does.

**The physics is code.** Speeds, acceleration, jump heights, gravity, the double jump's reset, the belly flop and edge detection
are in the executable, with nothing in the data files read here. `design\characters\crash.god` (a Lua script, like every `.god`;
the game embeds Lua 5.0) sets only presentation: the model prefix, radius 1.2, roundness 0.6, HUD colour and effects. The
executable's strings name the classes:

- `choreo::LocomotionDriver`, `choreo::AnimationDriver`, `choreo::Puppet`, `pure3d::LocomotionLayer`;
- `fightt::LocomotionTrack` (and its parameter templates), `fightt::ConditionFactoryTemplate<OnFootCharacterCondition>`;
- `OnFootCharacterAnimator`, `OnFootCharacterObject`, `OnFootManager`, `OnFootDefinition`, `Avatar::InitializeForWalking`;
- `ravenphysics::CollisionObject`, `CollisionVolume`, `RigidBodyLoader`, `RagdollLoader`, `Articulation`,
  `BroadPhaseDetectorSearchAndPrune`;
- the setters `SetGravity` and `SetOnFootDefinition`.

**What exists for the code:**

- The GameCube decomp (`cttr-decomp-gc`, CC0, about 45,000 symbols) names `Avatar::InitializeForWalking`, `OnFootManager_Get`,
  `OnFootRenderer_Get` and `GlobalFightTreeNode_Get`. Most of the on-foot code is unnamed functions, so finding the locomotion
  means following from those, then matching the stripped PS2 executable (Ghidra's function matching).
- Nobody has decompiled choreo or ravenphysics.
- The leaked Simpsons: Hit & Run source must not be used, nor anything derived from it.

**A practical middle way:**

1. Run the fight tree's state machine natively, with the conditions answered from Twinsanity's own player state (jumping,
   falling, on a ledge...).
2. Measure Tag Team Racing Crash's run speed, jump arc and double-jump height in PCSX2.
3. Tune a new character kind to them.

ROADMAP stage 3 estimates this at about two weeks, against months for the faithful code.
