# Crash Twinsanity for Mac

Crash Twinsanity (PS2) running natively on Mac, no emulator. Built on the full C++ decompilation of the game by Smartkin.

This is a work in progress and still has bugs.

## Play

Download the latest release, unzip it and open Crash Twinsanity. The first time, right click the app and choose Open, then pick your own PAL disc image (SLES 52568). The game is not included.

The HD texture pack is in `textures`. Copy the `SLES-52568` folder into `~/Library/Application Support/Crash Twinsanity/textures/`.

## Build

```
cmake -S native -B build/native/cmake -G Ninja
cmake --build build/native/cmake --target release
```

The app ends up in `build/native/dist`.

## What works

* Native graphics on the GPU at up to 8x resolution, HD texture packs
* Sound, music and movies
* Saving and loading
* Keyboard, mouse and controllers, with remappable controls
* New options screen, 60 Hz, widescreen and 21:9
* Community fixes and cutscene skipping

## Still to do

* Steady 60 fps (about 40 fps at the beach right now)
* Finish testing every level (73 of 131 areas checked so far)

## Credits

Smartkin and everyone behind the Twinsanity decompilation, which this is built on. CRASHARKI for the HD texture pack. Crash Twinsanity Improved for the community fixes.
