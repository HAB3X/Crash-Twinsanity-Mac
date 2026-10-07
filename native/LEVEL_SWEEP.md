# The level sweep

Every level chunk on the PAL disc started headless and played for 30 s with scripted input (`native/tools/level_sweep.py`, the
game started in the chunk by `ui/sweep.cpp`'s `$TWIN_SWEEP_LEVEL`, as the game's own command-line start does), its exit, states,
fatal signals and stray frees recorded. Run: `python native/tools/level_sweep.py OUT --jobs 2` (`--resume` to go on, `--report`
for this table). The raw results are `native/sweep/results.json`.

**Status: paused at 73 of 131 chunks: 56 play, 16 are entered from a neighbour, 1 have known issues (below). Resume with `--resume`.**

- OK: the chunk loads and plays 30 s with no fatal signal.
- Entered from a neighbour: the chunk has no instance of the played character (a hub's part, a tunnel, a corridor): play started
  there alone restarts a null character's camera rig (`FollowCameraRig::Restart` from `StartingPlay`). The game only enters these
  from a neighbouring chunk, whose links bring the character; not a native bug.

## Known issues

| Level / chunk | What happens | How to reproduce | Log / snapshots | Best guess at the cause |
| --- | --- | --- | --- | --- |
| Rockslide Rumble, `Levels\AltEarth\RockSlid\l10start` | Freeze when the opening cutscene starts (state 13, about 8 s in). With the VU safety net it no longer hangs but crawls (every frame stops a runaway VU1 run) | `TWINSANITY_AUDIO=off TWIN_SWEEP_LEVEL='Levels\AltEarth\RockSlid\l10start' "build/native/cmake/Crash Twinsanity.app/Contents/MacOS/Crash Twinsanity" --headless --quiet-stubs --snapshots DIR` | The sweep's `Levels_AltEarth_RockSlid_l10start/log.txt`; "the VU program run at 0 kicked ... packets: stopped" | A VU1 run started at micro address 0 never ends: it goes round the loaded programs (type 0B's, program 21 among them) kicking ~150,000 GIF packets a second; a normal frame's runs kick under 500. The interpreter loops the same way, so it's the data VU1 is given, not the translation. Type 0B's shader functions are the PS2's word for word; the record chain program 21 walks is well formed. Not yet found: likely a count or size in that frame's DMA data built differently natively (a model's or an effect's packets in the cutscene). Not yet compared with PCSX2 |
| Cavern entrance, `Levels\Earth\Cavern\cavent` | Freeze about 20 s into play (frames stop; no fatal signal, no VU safety-net message) | `TWINSANITY_AUDIO=off TWIN_SWEEP_LEVEL='Levels\Earth\Cavern\cavent' ... --headless --quiet-stubs --snapshots DIR` (the sweep's scripted input) | The sweep's `Levels_Earth_Cavern_cavent/`, `frame_0014.png` the last: Crash gone, the death sparkles where he was | Intermittent: a second run played 30 s with no freeze. Crash had died (the scripted input walks him off), so a timing-dependent hang around the death and restart (a race between threads?). The sweep now saves every thread's stack when one freezes (`hang_bt.txt`) |
| Earth boss area, `Levels\Earth\Hub\bossarea` | Froze during the cutscene with Cortex (state 13; frames stop a few seconds in). Fixed by the VU safety net's instruction limit: the cutscene plays through. Crash looks flat yellow in it (a possible lighting or texture glitch, still open) | `TWIN_SWEEP_LEVEL='Levels\Earth\Hub\bossarea'` as above; `TWIN_VU1=interpret` freezes the same way | The sweep's `Levels_Earth_Hub_bossarea/log.txt`: "the VU1 program run at 0 ran 10 million instructions: stopped" | A VU1 run at micro address 0 that never ends and kicks nothing (so the kick limit missed it): a model's triangle loop (0x16) finds a triangle partly outside the guard band (a vertex behind the camera, w < 0: the camera is almost inside the model) and calls the clipper (0x88), which ends by jumping to the current record's second word; that record (0x17A: 0x2E4, 0x2E4, next 0x17D) sends it back to the model's start (0x2E4), so the same triangle is clipped again forever. The record looks like a model drawn as wholly in view (`QueueInView`, mode 1), whose program has no clipper to go back to. Why it's drawn unclipped (its cell or box test) is still open |

Bugs fixed (NATIVE.md, "Native bugs found by the level sweep"): VU0 programs called with another microcode set loaded (77 of the
first run's chunks aborted while loading), the texture pack switch's race with the renderer.

| World | Area | Chunk | Status | Load (s) | Frames in 30 s | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| AltEarth | Core | afttreas | OK | 4.4 | 210 |  |
| AltEarth | Core | corea | OK | 7.2 | 189 |  |
| AltEarth | Core | coreb | OK | 7.3 | 173 |  |
| AltEarth | Core | corec | OK | 7.4 | 175 |  |
| AltEarth | Core | cored | OK | 7.1 | 228 |  |
| AltEarth | Core | pretreas | OK | 7.2 | 205 |  |
| AltEarth | Core | throne | OK | 8.8 | 234 |  |
| AltEarth | Core | treasure | OK | 5.7 | 202 |  |
| AltEarth | Hub | alta | OK | 5.8 | 135 |  |
| AltEarth | Hub | altdoc | OK | 8.0 | 156 |  |
| AltEarth | Hub | altdoc_b | OK | 7.7 | 139 |  |
| AltEarth | Hub | altdoc_c | OK | 7.9 | 158 |  |
| AltEarth | Hub | alttunl | Entered from a neighbour | - | - |  |
| AltEarth | Hub | coreent | OK | 7.5 | 172 |  |
| AltEarth | Hub | slipjoin | OK | 7.9 | 164 |  |
| AltEarth | Lab | altlabin | OK | 6.9 | 142 |  |
| AltEarth | Lab | labext | OK | 4.6 | 176 |  |
| AltEarth | Lab | psycho | OK | 6.2 | 206 |  |
| AltEarth | Lab | ptcorr | Entered from a neighbour | - | - |  |
| AltEarth | Lab | ptexit | Entered from a neighbour | - | - |  |
| AltEarth | RockSlid | l10chasa | OK | 6.4 | 170 |  |
| AltEarth | RockSlid | l10chasb | OK | 6.3 | 181 |  |
| AltEarth | RockSlid | l10end | OK | 5.2 | 826 |  |
| AltEarth | RockSlid | l10roids | OK | 4.5 | 827 |  |
| AltEarth | RockSlid | l10start | no end | 4.8 | - |  |
| Earth | Cavern | antfight | OK | 6.0 | 1086 |  |
| Earth | Cavern | cavallon | Entered from a neighbour | - | - |  |
| Earth | Cavern | cavbridg | OK | 5.7 | 1076 |  |
| Earth | Cavern | cavent | OK | 5.4 | 924 |  |
| Earth | Cavern | cavrnend | Entered from a neighbour | - | - |  |
| Earth | Cavern | cortthro | OK | 5.9 | 989 |  |
| Earth | Cavern | cryscave | Entered from a neighbour | - | - |  |
| Earth | Cavern | escape | Entered from a neighbour | - | - |  |
| Earth | Cavern | nitrocav | OK | 5.9 | 1049 |  |
| Earth | Cavern | tunnel01 | OK | 4.2 | 1541 |  |
| Earth | Cavern | tunnel02 | OK | 4.2 | 1233 |  |
| Earth | Cavern | tunnel03 | Entered from a neighbour | - | - |  |
| Earth | DocAmok | docamok1 | OK | 4.5 | 846 |  |
| Earth | DocAmok | docamok2 | OK | 5.2 | 670 |  |
| Earth | DocAmok | docamok3 | OK | 5.1 | 751 |  |
| Earth | DocAmok | docamok4 | Entered from a neighbour | - | - |  |
| Earth | Hub | beach | OK | 5.1 | 646 |  |
| Earth | Hub | bossarea | OK | 5.6 | 481 |  |
| Earth | Hub | docent | OK | 4.0 | 947 |  |
| Earth | Hub | highpath | OK | 5.3 | 653 |  |
| Earth | Hub | huba | OK | 3.9 | 510 |  |
| Earth | Hub | hubb | OK | 4.5 | 659 |  |
| Earth | Hub | hubboat1 | Entered from a neighbour | - | - |  |
| Earth | Hub | hubboat2 | Entered from a neighbour | - | - |  |
| Earth | Hub | hubc | Entered from a neighbour | - | - |  |
| Earth | Hub | hubd | OK | 7.5 | 507 |  |
| Earth | Hub | pier | OK | 7.3 | 626 |  |
| Earth | Hub | totemex | Entered from a neighbour | - | - |  |
| Earth | Totem | l03allon | Entered from a neighbour | - | - |  |
| Earth | Totem | l03beach | OK | 5.7 | 432 |  |
| Earth | Totem | l03chase | OK | 4.0 | 441 |  |
| Earth | Totem | l03creep | OK | 5.2 | 443 |  |
| Earth | Totem | l03river | OK | 4.2 | 460 |  |
| Earth | Totem | l03stock | OK | 5.9 | 485 |  |
| Ice | HighSeas | gpa01 | OK | 7.5 | 544 |  |
| Ice | HighSeas | gpa02 | OK | 6.3 | 660 |  |
| Ice | HighSeas | gpa03 | OK (game over) | 5.7 | - | the scripted input lost every life (the deaths and restarts worked); the sweep stops counting play there |
| Ice | HighSeas | gpa04 | OK | 5.1 | 587 |  |
| Ice | HighSeas | gpa05 | Entered from a neighbour | - | - |  |
| Ice | HighSeas | gpa06 | OK | 5.3 | 616 |  |
| Ice | HighSeas | gpa07 | OK | 5.5 | 502 |  |
| Ice | HighSeas | gpa08 | Entered from a neighbour | - | - |  |
| Ice | HighSeas | gpa09 | OK | 5.1 | 573 |  |
| Ice | HighSeas | gpa10 | OK | 5.0 | 930 |  |
| Ice | HighSeas | gpa11 | OK | 5.0 | 513 |  |
| Ice | Hub | airship | OK | 5.3 | 900 |  |
| Ice | Hub | labext | OK | 4.3 | 831 |  |
| Ice | Hub | labint | OK | 5.8 | 681 |  |
