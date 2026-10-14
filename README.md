# LEGO Star Wars: The Complete Saga — VR Port

An unofficial first-person VR mod for the PC (Steam) release of **LEGO® Star Wars™: The Complete Saga**
(`LEGOStarWarsSaga.exe`). You see through your minifig's eyes, your controllers are its hands, and you
swing the lightsaber yourself instead of pressing an attack button.

> Work in progress. Some features are built but not yet tested in a game run (see [Status](#status)).
> No game files are included. You need your own copy of the game.

## Features

- **First-person head tracking.** The camera sits at the minifig's eyes and the head is hidden. Stereo
  rendering alternates eyes, and each eye's image is cropped to the runtime's native FOV.
- **Hands follow your controllers.** The body stays animated for collision, and only the hands are drawn.
- **Physical lightsaber.** Swing the blade to damage enemies and break props. It also deflects blaster
  bolts back at the shooter.
- **Gestures.**
  - Pull the left trigger with your hand at your left hip to draw or holster the saber.
  - Grip with your hands at a LEGO pile to build it.
  - Point with your left hand and press X or the left trigger to use the Force on what you point at.
- **Guns aim with the controller, without aim assist.** The gun is always in the right hand. You can set
  `gun_aim=head` to aim where you look instead.
- **Head-relative smooth locomotion.** The stick moves you where you look, and you can turn smoothly or
  in snaps. Room-scale leaning is supported, and the body follows if you walk away from it.
- Menus show on a floating screen.
- **Safety.** The game is always forced into windowed mode, and **Ctrl+Shift+F12** kills it instantly.

## How it works

| Component | What it is |
|---|---|
| `lsw_vr/` → `d3d9.dll` (32-bit) | A proxy `d3d9.dll` that the game loads in place of the system one. It forwards to the real DLL and uses MinHook to hook the device, camera (`NuCameraSetEx`), skeleton (`StoreLocatorCoordinates`), pad input, combat, bolts, build and Force code. It captures each rendered frame and sends it to the host. |
| `lsw_vr/host/` → `lsw_vr_host.exe` (64-bit) | The OpenXR process. It runs outside the game because Meta's 32-bit OpenXR runtime crashes in `xrCreateSession`. It receives frames over shared memory (`src/shared/frame_channel`), submits them to the headset, and sends head and controller poses back. |
| `lsw_vr/host/` → `lsw_vr_tuner.exe` | An optional live tuning window for world scale, stereo strength, eye height and recentering. Ctrl+D dumps what the host submits. |
| `lsw_vr/tools/vr_harness.cpp` | Tests the VR pipeline without the game. |
| `xr_smoke_test/` | A minimal OpenXR session test, used to diagnose runtime problems. |

The engine addresses were reverse-engineered from the Steam executable (unpacked with Steamless) in Ghidra,
using the community `opensaga` decompilation as a reference for names and logic. They only match the
**Steam PC build**.

## Requirements

- The Steam PC version of LEGO Star Wars: The Complete Saga
- A PC VR headset with an OpenXR runtime. The mod was developed on an Oculus Rift CV1 with the Meta runtime.
- Visual Studio 2022 or later (MSVC, with both x86 and x64 toolsets), CMake 3.20 or later, and Ninja
- The [OpenXR loader SDK](https://github.com/KhronosGroup/OpenXR-SDK-Source/releases) for Windows
  (`openxr_loader_windows-*.zip`)
- MinHook is fetched automatically by CMake.

## Building

The mod DLL must be **32-bit**, and the host must be **64-bit**.

```bat
:: 1) Mod DLL: from an x86 developer prompt (vcvars32.bat)
cmake -S lsw_vr -B build\lsw_vr -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build\lsw_vr

:: 2) OpenXR host + tuner: from an x64 developer prompt (vcvars64.bat)
cmake -S lsw_vr\host -B build\host -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
      -DOPENXR_SDK=C:\path\to\openxr_loader_windows-1.1.x
cmake --build build\host
```

## Installing

Copy these files into the game folder, usually
`C:\Program Files (x86)\Steam\steamapps\common\Lego Star Wars Saga\`:

- `d3d9.dll` (from `build\lsw_vr`)
- `lsw_vr_host.exe` and its x64 `openxr_loader.dll` (from `build\host`)
- Optionally, `lsw_vr_tuner.exe`

Start your OpenXR runtime, put the headset on (the session only becomes ready while the headset is worn),
then launch the game from Steam. The mod starts the host itself. To uninstall, delete `d3d9.dll`.

The game folder gets these logs: `lsw_vr.log` (mod) and `lsw_vr_host.log` (host).

## Controls

**In VR**

| Input | Action |
|---|---|
| Left stick | Move, relative to where you look |
| Right stick | Turn (smooth or snap) |
| Swing the saber | Attack, break props, deflect bolts |
| Left trigger at left hip | Draw or holster the saber |
| Grip at a LEGO pile | Build |
| Point with the left hand + X or left trigger | Use the Force on the target |
| Right trigger | Fire the gun |
| Hold left stick click | Calibrate: left stick Y sets world scale, right stick Y sets stereo strength. Tap to switch between stereo and mono. |

**Keyboard (debug/tuning)**

| Key | Action |
|---|---|
| **Ctrl+Shift+F12** | Kill the game immediately |
| F4 | Switch between stereo and mono |
| F5 | Cycle the frame delay (auto / 0 / 1) |
| F6 | Dump the skeleton joints to the log |
| F7 | Turn head tracking on or off |
| F8 | Recenter |
| F10 | Switch between first and third person (for example, during cutscenes) |
| F11 | Hand test pose |
| `[` / `]` | Decrease or increase world scale |
| `-` / `=` | Decrease or increase stereo strength |
| Home / End | Move the eye forward or back |

## Settings

The mod saves its settings to `lsw_vr_settings.txt` in the game folder:

```ini
world_scale=0.300      ; game units per real meter
stereo_strength=1.000
stereo=1
eye_lift=0.000
eye_forward=0.000
pc_fullscreen=0
smooth_turn=1
gun_aim=gun            ; or "head"
```

## Status

The first-person camera, stereo, controllers and skeleton have been tested in a real VR run. The
hands-only rework, room-scale, bolt deflection, point-to-Force and several Force-cancel fixes are built
but still need a test run. Known limitations:

- Cutscenes render in first person. Press F10 to switch views.
- Dual-wielded second guns are not handled.
- Battle droids use a different rig (no hand or head joints) and are handled specially.

## Disclaimer

This is a fan project and is not affiliated with or endorsed by LEGO, Lucasfilm, Disney, TT Games or
Warner Bros. It contains no game code or assets. All trademarks belong to their owners.
