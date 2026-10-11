# The level sweep

Every level chunk on the PAL disc started headless and played for 30 s with scripted input (`native/tools/level_sweep.py`, the
game started in the chunk by `ui/sweep.cpp`'s `$TWIN_SWEEP_LEVEL`, as the game's own command-line start does), its exit, states,
fatal signals and stray frees recorded. Run: `python native/tools/level_sweep.py OUT --jobs 2` (`--resume` to go on, `--report`
for this table). The raw results are `native/sweep/results.json`.

**Status: complete, 131 of 131 chunks: 104 play, 23 are entered from a neighbour, 3 end in a game over (the scripted input loses its lives; deaths and restarts work), 1 has a known issue (Rockslide Rumble's opening, below).**

- OK: the chunk loads and plays 30 s with no fatal signal.
- Entered from a neighbour: the chunk has no instance of the played character (a hub's part, a tunnel, a corridor): play started
  there alone restarts a null character's camera rig (`FollowCameraRig::Restart` from `StartingPlay`). The game only enters these
  from a neighbouring chunk, whose links bring the character; not a native bug.

## Known issues

| Level / chunk | What happens | How to reproduce | Log / snapshots | Best guess at the cause |
| --- | --- | --- | --- | --- |
| Rockslide Rumble, `Levels\AltEarth\RockSlid\l10start` | Freeze when the opening cutscene starts (state 13, about 8 s in). With the VU safety net it no longer hangs but crawls (every frame stops a runaway VU1 run) | `TWINSANITY_AUDIO=off TWIN_SWEEP_LEVEL='Levels\AltEarth\RockSlid\l10start' "build/native/cmake/Crash Twinsanity.app/Contents/MacOS/Crash Twinsanity" --headless --quiet-stubs --snapshots DIR` | The sweep's `Levels_AltEarth_RockSlid_l10start/log.txt`; "the VU program run at 0 kicked ... packets: stopped" | A VU1 run started at micro address 0 never ends: it goes round the loaded programs (type 0B's, program 21 among them) kicking ~150,000 GIF packets a second; a normal frame's runs kick under 500. The interpreter loops the same way, so it's the data VU1 is given, not the translation. Type 0B's shader functions are the PS2's word for word; the record chain program 21 walks is well formed. Found: the same recursion as the boss area's (a record whose two routine words are the same, native/NATIVE.md, "VU1 runs that never end"): its records at 0x117, 0x11a, 0x17a and 0x17d are dropped now and the scene plays at its pace |
| Cavern entrance, `Levels\Earth\Cavern\cavent` | Freeze about 20 s into play (frames stop; no fatal signal, no VU safety-net message) | `TWINSANITY_AUDIO=off TWIN_SWEEP_LEVEL='Levels\Earth\Cavern\cavent' ... --headless --quiet-stubs --snapshots DIR` (the sweep's scripted input) | The sweep's `Levels_Earth_Cavern_cavent/`, `frame_0014.png` the last: Crash gone, the death sparkles where he was | Intermittent: a second run played 30 s with no freeze. Crash had died (the scripted input walks him off), so a timing-dependent hang around the death and restart (a race between threads?). The sweep now saves every thread's stack when one freezes (`hang_bt.txt`) |
| Earth boss area, `Levels\Earth\Hub\bossarea` | Froze during the cutscene with Cortex (state 13; frames stop a few seconds in). Fixed by the VU safety net's instruction limit: the cutscene plays through. Crash looks flat yellow in it (a possible lighting or texture glitch, still open) | `TWIN_SWEEP_LEVEL='Levels\Earth\Hub\bossarea'` as above; `TWIN_VU1=interpret` freezes the same way | The sweep's `Levels_Earth_Hub_bossarea/log.txt`: "the VU1 program run at 0 ran 10 million instructions: stopped" | A VU1 run at micro address 0 that never ends and kicks nothing (so the kick limit missed it): a model's triangle loop (0x16) finds a triangle partly outside the guard band (a vertex behind the camera, w < 0: the camera is almost inside the model) and calls the clipper (0x88), which ends by jumping to the current record's second word; that record (0x17A: 0x2E4, 0x2E4, next 0x17D) sends it back to the model's start (0x2E4), so the same triangle is clipped again forever. The record looks like a model drawn as wholly in view (`QueueInView`, mode 1), whose program has no clipper to go back to. Why it's drawn unclipped (its cell or box test) is still open |

The Cavern entrance and Earth boss area freezes did not recur in the complete run (both ran their 30 s); the boss area's is stopped
by the VU safety net, so its clipper loop is still there. Bugs fixed (NATIVE.md, "Native bugs found by the level sweep"): VU0 programs called with another microcode set loaded (77 of the
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
| Ice | HighSeas | gpa12 | OK | 4.3 | 1124 |  |
| Ice | Hub | airship | OK | 5.3 | 900 |  |
| Ice | Hub | labext | OK | 4.3 | 831 |  |
| Ice | Hub | labint | OK | 5.8 | 681 |  |
| Ice | Hub | psycho | OK | 4.2 | 891 |  |
| Ice | Hub | ptcorr | Entered from a neighbour | - | - |  |
| Ice | Hub | ptexit | Entered from a neighbour | - | - |  |
| Ice | Hub | shipent | OK | 2.6 | 956 |  |
| Ice | Hub | slipent | Entered from a neighbour | - | - |  |
| Ice | IceClimb | bergcorr | OK | 5.4 | 987 |  |
| Ice | IceClimb | bergext | OK | 5.5 | 770 |  |
| Ice | IceClimb | bergint | OK | 3.5 | 922 |  |
| Ice | IceClimb | caveent | OK | 5.2 | 937 |  |
| Ice | IceClimb | ukafight | OK | 5.3 | 1144 |  |
| Ice | IceClimb | ukatrans | OK | 5.5 | 1339 |  |
| Ice | SlipSlide | l05brawl | OK | 4.6 | 613 |  |
| Ice | SlipSlide | l05chasa | OK | 4.4 | 962 |  |
| Ice | SlipSlide | l05chasb | Entered from a neighbour | - | - |  |
| Ice | SlipSlide | l05grind | OK | 4.2 | 1151 |  |
| Ice | SlipSlide | l05river | OK | 4.5 | 1074 |  |
| Ice | SlipSlide | l05roids | OK | 4.6 | 979 |  |
| Ice | SlipSlide | l05start | OK | 4.3 | 1131 |  |
| school | Boiler | boiler_1 | OK | 4.4 | 870 |  |
| school | Boiler | boiler_2 | Entered from a neighbour | - | - |  |
| school | Boiler | boiler_3 | OK | 4.1 | 1168 |  |
| school | Boiler | boiler_4 | OK | 4.6 | 597 |  |
| school | Boiler | boiler_5 | OK | 5.3 | 811 |  |
| school | Boiler | boiler_6 | OK | 5.5 | 902 |  |
| school | Boiler | boiler_x | OK (game over) | 4.5 | - | the scripted input lost every life (the deaths and restarts worked); the sweep stops counting play there |
| school | Cortex | cogpa01 | OK | 4.7 | 818 |  |
| school | Cortex | cogpa02 | OK | 4.6 | 606 |  |
| school | Cortex | cogpa03 | OK | 4.4 | 444 |  |
| school | Cortex | cogpa04 | OK | 4.6 | 478 |  |
| school | Cortex | cogpa05 | OK | 4.9 | 525 |  |
| school | Cortex | cogpa06 | OK | 4.4 | 810 |  |
| school | Cortex | cogpa07 | OK | 4.4 | 502 |  |
| school | Cortex | cogpa08 | OK | 5.7 | 467 |  |
| school | Crash | crashent | OK | 4.2 | 876 |  |
| school | Crash | crgpa01 | OK | 3.6 | 986 |  |
| school | Crash | crgpa02 | OK | 3.8 | 883 |  |
| school | Crash | crgpa03 | OK | 3.7 | 880 |  |
| school | Crash | crgpa04 | OK | 3.7 | 910 |  |
| school | Crash | crgpa05 | OK | 4.0 | 1124 |  |
| school | Crash | crgpa06 | OK | 3.5 | 906 |  |
| school | Crash | crgpa07 | OK | 4.1 | 930 |  |
| school | Crash | crgpa08 | OK | 4.2 | 847 |  |
| school | Crash | crlib | OK | 4.1 | 1083 |  |
| school | Madame | ambercor | OK | 3.9 | 1325 |  |
| school | Madame | amberly | OK | 4.3 | 797 |  |
| school | RoofTop | buschase | OK | 3.7 | 1018 |  |
| school | RoofTop | roof01 | OK | 4.1 | 990 |  |
| school | RoofTop | roof02 | Entered from a neighbour | - | - |  |
| school | RoofTop | roof03 | OK (game over) | 3.8 | - | the scripted input lost every life (the deaths and restarts worked); the sweep stops counting play there |
| school | RoofTop | roof04 | Entered from a neighbour | - | - |  |
| school | RoofTop | roof05 | OK | 3.9 | 507 |  |
| school | RoofTop | roofcor1 | OK | 4.6 | 671 |  |
| school | RoofTop | roofcor2 | OK | 4.5 | 637 |  |
| school | Sch_Hub | boil2lck | OK | 5.3 | 906 |  |
| school | Sch_Hub | schship | OK | 4.5 | 621 |  |
| school | Sch_Hub | sch_hub | OK | 3.8 | 460 |  |
| school | Sch_Hub | slk01 | OK | 3.1 | 813 |  |
