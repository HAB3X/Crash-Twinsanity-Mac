# Crash Twinsanity for Mac

Crash Twinsanity (PS2) running natively on Mac, no emulator. It's built on the full C++ decompilation of the game by Smartkin.

This is a work in progress and it still has bugs.

## Play

For Apple silicon and Intel Macs. Download `Crash-Twinsanity-macOS.zip` from the [releases](https://github.com/HAB3X/Crash-Twinsanity-Mac/releases), unzip it and open Crash Twinsanity. The first time, right click the app and choose Open, then pick your own PAL disc image (SLES 52568). The game is not included.

[native/PLAYING.md](native/PLAYING.md) has the controls and options.

## What works

* Native graphics on the GPU at up to 8x resolution, with HD texture packs
* Sound, music and movies
* Saving and loading
* Keyboard, mouse and controllers, with remappable controls
* A new options screen, 60 Hz, widescreen and 21:9
* Community fixes and cutscene skipping
* A level select, and a mods folder for edited game files

## Still to do

* A steady 60 fps
* Testing on a real Intel Mac (it runs under Rosetta)
* Linux and Windows
* More save slots

## Building

See [docs/BUILDING.md](docs/BUILDING.md).

## Credits

Smartkin and everyone behind the [Twinsanity decompilation](https://github.com/Smartkin/twinsanity-reversed), which this is built on. CRASHARKI for the HD texture pack. [Crash Twinsanity Improved](https://github.com/AlexMollard/Crash-Twinsanity-Improved) for the community fixes and cutscene skips.

Crash Twinsanity is a game by Traveller's Tales and Vivendi Universal Games. This repository has none of its code or data: you need your own copy of the disc.
