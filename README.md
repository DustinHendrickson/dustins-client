# wowdecomp

A C++ decompilation of the World of Warcraft 3.3.5a client (`Wow.exe`, build 12340, June 2010).

Join the discord for other clients and to show off some cool stuff you build.

https://discord.gg/DsZMGUD8aT

Every function is reconstructed from the original binary and keeps the address it came from in an
`// OFFSET:` tag, so each one can be compared against the original. The source tree follows the layout
of the original client (`Engine/Source/<Module>`, `WoW/Source/<Module>`, original file names).

x64 is the main target. Win32 still builds and is used to check struct layouts against the 32-bit
original.

## Progress

The percentages below measure how much of the original binary has a counterpart in this source tree. They do
**not** say how much of the game works, and the missing part is not missing gameplay: the client builds into a
complete executable and the game is 100% playable. Bugs could still be hiding, though; a ported function can behave differently
from the original.

Measured against the 12340 binary on 2026-10-15. A function counts as ported when its address is
tagged in the source.

| | Ported | Total | Share of the original |
|---|---:|---:|---:|
| Functions | 24,515 | 27,157 | 90.3% |
| Code bytes | 5,270,186 | 5,656,573 | 93.2% |

The total leaves out 658 functions that IDA identifies as compiler library code and 132 import thunks.
Most of the untagged remainder is code the build gets elsewhere: compiler-generated code (static
initializers, template instances) that our compiler emits on its own, C runtime helpers, and third-party
libraries that are linked from their upstream sources instead of being decompiled (FreeType, zlib, PCRE,
parts of FMOD Ex).

## Platforms

| Platform | State | Get it |
|---|---|---|
| [Windows](#windows) (x64, Win32) | Main target | Build from source |
| [macOS](#macos) (Apple Silicon) | Plays | [Download](https://github.com/DustinHendrickson/wowdecomp/releases) or build from source |
| [Nintendo Switch](#nintendo-switch) (homebrew) | Experimental | [Download](https://github.com/DustinHendrickson/wowdecomp/releases) or build from source |
| [Android](#android) (arm64 phones and tablets) | Experimental | [Download](https://github.com/DustinHendrickson/wowdecomp/releases) or build from source |

All platforms build from one source tree; platform code lives in `Mac/`, `Switch/` and `Android/` folders next to
the Windows code. The macOS, Switch and Android ports are in this fork
([DustinHendrickson/wowdecomp](https://github.com/DustinHendrickson/wowdecomp)), not in the upstream repository.

### What every platform needs

- **Game data:** the `Data` folder of a World of Warcraft **3.3.5a (12340)** client, about 17 GB. No game data is
  included in this repository or its downloads. The MPQ archives are the same on every platform, so the `Data`
  folder of a Windows install works everywhere.
- **A server:** set it in `Data/<locale>/realmlist.wtf` (for example `Data/enUS/realmlist.wtf`):

  ```
  set realmlist your.server.address
  ```

  The game reads the server from this file at every start; changing it only in `WTF/Config.wtf` doesn't stick.
- **Warden off:** Warden's modules are 32-bit Windows code. The x64 Windows build, macOS, the Switch and Android
  don't run them, so use a server with Warden turned off.

### Getting the source

```bash
git clone https://github.com/DustinHendrickson/wowdecomp.git
cd wowdecomp
```

In-source builds are refused, so every build below uses a separate `build/<platform>` folder.

## Windows

### Requirements

- Windows 10 or 11
- Visual Studio 2022 with the **Desktop development with C++** workload (MSVC v143 and a Windows SDK)
- CMake 3.10 or newer (the one that ships with Visual Studio works)
- For Win32 only: [NASM](https://www.nasm.us/), on `PATH` or passed as `-DNASM_EXECUTABLE=<path>`, and
  `DivxDecoder.dll` from your 3.3.5a client copied to `ThirdParty/DivxDecoder/` (the x64 build uses its own movie
  decoder instead)

### Building

From a Developer PowerShell, or any shell with `cmake` on `PATH`, in the repository folder:

```powershell
# x64 (main target)
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64
cmake --build build/x64 --config Release --target WowClient --parallel

# Win32 (optional)
cmake -S . -B build/win32 -G "Visual Studio 17 2022" -A Win32
cmake --build build/win32 --config Release --target WowClient --parallel
```

The client is written to `bin/Win/`:

| Target | x64 | Win32 |
|---|---|---|
| `WowClient` | `WowClient64.exe` | `WowClient32.exe` |
| `Wow` | `Wow64.exe` | `Wow32.exe` |

`WowClient` is the client with a crash logger: it opens a console window and writes `WowClient_crash.log` when the
client crashes. `Wow` is the same client without it.

### Running

1. Copy the `Data` folder of your 3.3.5a client into `bin/Win/`, so you have `bin/Win/Data/`.
2. Set your server in `bin/Win/Data/<locale>/realmlist.wtf`.
3. Start `bin/Win/WowClient64.exe` (or `Wow64.exe`).

## macOS

### Requirements

- A Mac with Apple Silicon (M1 or later) and macOS 13 Ventura or newer
- To build: the Xcode Command Line Tools (`xcode-select --install`), and CMake and Ninja (`brew install cmake ninja`)

### Installing a download

1. Download `WoW-3.3.5a-macOS-arm64.zip` from the [releases page](https://github.com/DustinHendrickson/wowdecomp/releases)
   and unzip it.
2. Put `World of Warcraft.app` in your 3.3.5a folder, next to its `Data` folder (where `Wow.exe` is).
3. Set your server in `Data/<locale>/realmlist.wtf`.
4. Open the app. It's signed ad hoc, not notarized by Apple, so macOS blocks a downloaded copy the first time
   ("can't be opened" or "is damaged"). Either try to open it once, then choose **Open Anyway** in System Settings →
   Privacy & Security, or run this in Terminal from the folder holding the app:

   ```bash
   xattr -dr com.apple.quarantine "World of Warcraft.app"
   ```

### Building

```bash
cmake -S . -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/mac --target Wow WowClient
```

This writes `World of Warcraft.app` into `bin/Mac/Apps.noindex/` (a folder Spotlight skips, so only the copy you
install shows up in Spotlight and Launchpad) and `bin/Mac/WowClient`, the same client as a plain executable that
prints its log to the terminal. To install the app into your 3.3.5a folder, next to `Data`:

```bash
cmake --install build/mac --prefix "/path/to/your/3.3.5a folder"
```

Then set your server and open **World of Warcraft** in that folder. Built on your own Mac, it opens without the
quarantine step.

### Notes

- Settings, logs, screenshots and add-ons live in the game folder, as with `Wow.exe`. Logs go to `Logs/`, crash
  reports to `Errors/`.
- Started somewhere without `Data` next to it, the app asks for your 3.3.5a folder once and remembers it. Hold Option
  while launching to choose another.
- On Retina displays the game renders at full resolution. To render at one pixel per point instead (faster, softer):
  `defaults write org.wowdecomp.wow Retina -bool NO`, or `WOWDECOMP_RETINA=0` for `WowClient`.
- The intro and cinematics play. The highest shadow settings were Direct3D-only in the original client and aren't
  available.
- For development, `bin/Mac/WowClient` runs in place with `Data` linked into `bin/Mac/`. Port notes are in
  [docs/MACOS_PORT.md](docs/MACOS_PORT.md).

## Nintendo Switch

Played with a controller, the touch screen, or a USB keyboard and mouse. Port notes, the full controls and status are
in [docs/SWITCH_PORT.md](docs/SWITCH_PORT.md).

### Requirements

- A Switch running custom firmware (current Atmosphère) with the Homebrew Menu
- An SD card with room for the game data (about 17 GB; FAT32 works, no archive is over 4 GB)
- To build: Docker (Docker Desktop on a Mac or Windows)

### Installing

1. Download `WoW-3.3.5a-Switch.zip` from the [releases page](https://github.com/DustinHendrickson/wowdecomp/releases)
   and extract it at the root of the SD card. It puts the game at `sdmc:/switch/wow/WoW.nro`. (Built from source,
   copy `bin/Switch/WoW.nro` there yourself.)
2. Copy your client's `Data` folder next to it, so you have:

   ```
   sdmc:/switch/wow/WoW.nro
   sdmc:/switch/wow/Data/common.MPQ, Data/enUS/, ...
   ```

   Copy one folder at a time and check the copy is complete: a large copy over USB can drop files.
3. Set your server in `sdmc:/switch/wow/Data/<locale>/realmlist.wtf` (easiest on your computer before copying).

Add-ons go in `sdmc:/switch/wow/Interface/AddOns/`, as on a PC. Settings, logs and screenshots are written next to
`WoW.nro`.

### Running

1. Start the **Homebrew Menu over a game**: hold R while starting any game from the HOME menu. Started from the album,
   homebrew gets too little memory; the game says so and quits.
2. Pick World of Warcraft.
3. Hold **Minus** in the game for the controls list. In short: the left stick moves, the right stick turns the
   camera, B jumps, A, X, Y and the D-pad are action buttons, R targets the nearest enemy, ZR + R the nearest friendly
   or neutral unit, and clicking the right stick interacts (loot, talk, use). Menus and windows switch to cursor
   mode, where the D-pad jumps between buttons and A clicks. Plus is Escape. The controls list also has options: a
   hint bar, inverted camera, a larger handheld UI, a 30 fps battery saver, and **Hide chat**.

Docked, the game renders at 1920x1080; handheld at 1280x720. Logs go to `sdmc:/switch/wow/Logs/Console.txt`, crash
reports to `Errors/`: please attach them when reporting problems.

### Building

```bash
Tools/Switch/build.sh
```

The first run builds the Docker image (devkitA64 and Clang, about 1 GB). It writes `bin/Switch/WoW.nro` and
`bin/Switch/Wow.elf`, the same program with symbols for reading crash logs.

## Android

Played by touch, with gamepads, keyboards and mice as extras. Port notes and status are in
[docs/ANDROID_PORT.md](docs/ANDROID_PORT.md).

### Requirements

- An arm64 phone or tablet with Android 11 or newer and OpenGL ES 3.0
- About 18 GB free in the device's internal storage
- A computer and USB cable (or another way) to copy the game data to the device
- To build: see [Building](#building-3) below

### Installing

**1. Install the app.** Download `WoW-3.3.5a-Android-arm64.apk` from the
[releases page](https://github.com/DustinHendrickson/wowdecomp/releases) on the device and open it, or from a
computer run `adb install WoW-3.3.5a-Android-arm64.apk`. Android asks to allow installs from your browser or file
manager: allow it, then tap **Install**. The APK is signed with a development key, so Play Protect may warn about an
unknown developer: choose **Install anyway**.

**2. Allow file access.** Open World of Warcraft. It asks for **All files access**, because it reads its data from
shared storage: tap **Allow**, turn on the switch for World of Warcraft on the page that opens, and go back. The game
creates a `WoW335` folder in the device's internal storage, says no game data was found, and quits.

**3. Copy the game data.** Copy your client's whole `Data` folder into `WoW335`, so you have:

```
Internal storage/WoW335/Data/common.MPQ
Internal storage/WoW335/Data/common-2.MPQ
Internal storage/WoW335/Data/expansion.MPQ
Internal storage/WoW335/Data/lichking.MPQ
Internal storage/WoW335/Data/patch.MPQ, patch-2.MPQ, patch-3.MPQ, ...
Internal storage/WoW335/Data/enUS/              (your locale: enUS, enGB, deDE, frFR, ...)
```

To apps, internal storage is `/sdcard`, so the full path is `/sdcard/WoW335/Data/`.

- **From Windows:** connect the device with a USB cable, choose **File transfer** in the device's USB notification,
  open the device in File Explorer and drag `Data` into `Internal shared storage\WoW335`.
- **From a Mac:** use [OpenMTP](https://openmtp.ganeshrvel.com) or Android File Transfer, or with the Android
  platform tools: `adb push Data /sdcard/WoW335/`.
- **From Linux:** the device shows up as an MTP device in the file manager, or use `adb push` as above.

Copy `Data` only, not `Wow.exe` or the rest of the client. Copying 17 GB over USB takes a while and can drop files:
when it's done, check that the number of files and the size of `Data` on the device match your computer's.

**4. Set your server** in `WoW335/Data/<locale>/realmlist.wtf` (`set realmlist your.server.address`). It's easiest to
edit the file on your computer before copying `Data`.

**5. Play.** Open World of Warcraft and log in.

### Playing

- **Move:** a joystick appears wherever your left thumb lands in the lower left. Double-tap it to autorun.
- **Look:** drag on the right side of the screen. **Zoom:** pinch with two fingers.
- **Select:** tap a creature or player. **Talk, loot, attack or right-click:** hold your finger on it.
- **Abilities:** the wheel of buttons on the right, around Jump, with a number button that turns the page. Hold a
  wheel button to put a spell on it from the spellbook, or to clear it.
- **Top bar:** chat, your character, all bags, an actions list (sit, sheathe, follow, nameplates, hide / show chat and more), a
  Shift / Ctrl / Alt button for the next touch, the touch menu (the game's windows, bag slots and settings such as
  interface size, text size, render resolution and a 30 fps battery saver), and an eye that hides the controls.
- A **How to play** guide opens the first time you enter the world, and any time from the touch menu.

### Files on the device

Everything lives in `/sdcard/WoW335/`:

| Folder | Contents |
|---|---|
| `Data/` | your game data |
| `WTF/` | settings and saved variables |
| `Interface/AddOns/` | add-ons, as on a PC (the `TouchControls` add-on is put back at every start) |
| `Screenshots/` | screenshots |
| `Logs/`, `Errors/` | the console log (`Console.txt`) and crash reports |

### Troubleshooting

- **"No game data found":** `Data` isn't directly inside `WoW335`, or holds no `.MPQ` files. Look for an extra level
  such as `WoW335/Data/Data`, or a folder with a different name.
- **It keeps asking for file access:** turn it on in Settings → Apps → Special app access → All files access → World
  of Warcraft (the wording varies a little between devices).
- **Can't connect:** check `realmlist.wtf` in your locale's folder, and that your server has Warden turned off.
- **Missing textures, or crashes in particular places:** usually an incomplete copy of `Data`. Copy the affected
  `.MPQ` files again.
- **Reporting a problem:** attach `WoW335/Logs/Console.txt` and anything in `WoW335/Errors/`, and say which device you
  use.

### Building

Building has been done on macOS; on Linux, set `ANDROID_HOME` and `JAVA_HOME` to your own paths. You need:

- The Android SDK (`ANDROID_HOME`, default `~/Library/Android/sdk`) with platform `android-35`, build-tools 35.0.0
  and NDK r27 (27.2.12479018, or set `ANDROID_NDK_HOME`)
- A JDK 17 (`JAVA_HOME`; Homebrew's `openjdk@17` is found on its own)
- CMake and Ninja

No Gradle or Android Studio project is used:

```bash
Tools/Android/build.sh            # build bin/Android/WoW.apk
Tools/Android/build.sh install    # build, install on the connected device or emulator over adb, and start it
```

The APK holds `libwow.so`, the app's one Java class (`WowActivity`), its manifest and the `TouchControls` add-on. It's
signed with a debug key made on first use (`build/android/debug.keystore`); keep that file to install later builds as
updates.

## Build options

| CMake option | Default | Effect |
|---|---|---|
| `WOWDECOMP_DISABLE_SIGNATURE_CHECKS` | `OFF` (`ON` for the Switch and Android) | Skip the MPQ file and `.toc.sig` signature checks, so modified game data and interface files load |
| `WOWCLIENT_CRASH_LOG` | `ON` | Build `WowClient` with the crash logger (Windows) |
| `WOWCLIENT_OUTPUT_OVERRIDE` | empty | Name the `WowClient` executable something other than `WowClient64` / `WowClient32` |

## Source layout

| Path | Contents |
|---|---|
| `Engine/Source/` | Engine modules: Storm, Gx (Direct3D 9 / OpenGL), Model2, Frame/FrameXML UI, SoundEngine, Lua, ... |
| `WoW/Source/` | Game modules: objects, world rendering, UI, networking, database, glue screens |
| `WoW/Common/` | Code shared by the game modules |
| `ThirdParty/` | Libraries linked from their upstream sources |
| `Tools/WowClient/` | The `WowClient` executable target |
| `Tools/XvidDecoder/` | x64 movie decoder that replaces the original 32-bit `DivxDecoder.dll` |
| `Tools/Switch/` | The Switch build: Docker image, build script, the `SwitchControls` add-on |
| `Tools/Android/` | The Android app: build script, manifest, `WowActivity` and the `TouchControls` add-on |
| `Tools/OsWindowTest/`, `Tools/GxDrawTest/`, `Tools/NetTest/`, `Tools/SoundTest/` | macOS self-tests for the window and input, OpenGL device, network and sound layers (`--self-test` / no arguments) |
| `docs/` | Port notes for macOS, the Switch and Android |

## Special thanks

- [binana](https://github.com/thunderbrewhq/binana)
- [thunderbrew](https://github.com/thunderbrewhq/thunderbrew)
- [whoa](https://github.com/whoahq/whoa)
- TOMRUS, for the initial IDB
- Everyone who has reported bugs so far
- The WoW modding community
