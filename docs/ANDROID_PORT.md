# Android port (arm64)

The 3.3.5a client as an Android app for phones and tablets (arm64-v8a), played by touch first, with gamepads and
USB / Bluetooth keyboards and mice as extras. It follows the macOS and Switch ports: the same POSIX layers, an
`Android/` folder where the platform differs, and one source tree on `main`, chosen by configuration.

## Decisions

| | Choice | Why |
|---|---|---|
| Rendering | A native **OpenGL ES 3.0** Gx device (`CGxDeviceGLES`), with the ARB vertex and fragment programs translated to GLSL ES and the fixed-function pipeline generated as GLSL | Android has no desktop OpenGL. A device of our own, rather than a translation library under the desktop one, can be tuned for mobile GPUs |
| Game data | A folder the player can reach: `/sdcard/WoW335/` by default (`Data/`, `WTF/`, `Interface/AddOns/`, `Logs/`), with **All files access** (`MANAGE_EXTERNAL_STORAGE`) | Copying 17 GB and add-ons from a PC or a file manager must be easy. Sideloaded APK only: Google Play doesn't allow that permission for this |
| Touch | **Hybrid**: floating joystick on the left, camera drag on the right, pinch to zoom, tap to click anything (action bars, windows, units), long press to right-click, and a few overlay buttons that work while moving and looking | Combat needs moving, looking and pressing at once; WoW's own action bars are already the best ability buttons |
| Testing | The Android emulator on this Mac (arm64 images run natively on Apple Silicon) for automated checks, and the user's phone for real touch, performance and GPU drivers | |
| App shell | `WowActivity`, a small `NativeActivity` subclass (Tools/Android/java), with JNI calls into it for dialogs, the keyboard, storage access, the clipboard and insets | No Gradle or AndroidX needed; `javac` and `d8` build the one class, and the APK is assembled from it, the native library and the manifest |
| Minimum | Android 11 (API 30), arm64-v8a, OpenGL ES 3.0 | All files access exists from Android 11 |

## How it fits the tree

- **CMake**: `cmake/AndroidToolchain.cmake` wraps the NDK's toolchain. `WOWDECOMP_PLATFORM_ANDROID` and
  `WOWDECOMP_PLATFORM_POSIX`. `wowdecomp_platform_sources` keeps `Mac/` folders and lets an `Android/` file replace the
  `Mac/` one of the same name, as `Switch/` does. The game builds as `libwow.so`, loaded by `WowActivity`.
- **Os** (`Engine/Source/Os/Android/`): the window is the activity's `ANativeWindow`; input comes from the activity's
  input queue (multi-touch, gamepads, keyboards, mice); the clipboard, soft keyboard and system bars through JNI.
- **Files**: BlizzardCore's and Storm's POSIX file layers as on macOS, rooted at the game folder. Android's shared
  storage normally matches names without regard to case, as Windows does; the Switch's case resolver is there for
  devices where it doesn't.
- **Audio**: an FMOD output on AAudio (`fmodex/android/src/fmod_output_aaudio.cpp`), polled like the Switch's audren
  output.
- **Network**: POSIX sockets, as on macOS. The manifest asks for `INTERNET`.
- **Packaging**: `Tools/Android/build.sh` builds `libwow.so` with the NDK, then the APK with `aapt2`, `zipalign` and
  `apksigner` (a debug key, or the user's release key). The SwitchControls-style add-on for touch (`TouchControls`) is
  packed in the APK's assets and installed into `Interface/AddOns` at every start.

## The OpenGL ES device

Android runs the same OpenGL device as macOS and the Switch (`CGxDeviceOpenGL`), unchanged. What differs is the GL
under it: Android has OpenGL ES 3 only, so `CGxDeviceOpenGL/Android` carries out the ~120 desktop GL 1.x and extension
entry points the device calls on ES 3 (`GlExt.h` includes `Android/GlAndroid.h` in place of `<OpenGL/gl.h>`). Every
fix to the device then serves all three platforms.

- **Fixed function as generated shaders** (`GlAndroidPrograms.cpp`). The state set through the fixed-function path
  (matrices, up to 8 lights with color material and separate specular, texture environments and combiners, coordinate
  generation, fog, alpha test, point sizes and sprites) is tracked, and the part that changes code forms a key; each
  key gets a GLSL ES 3.00 program, compiled once and cached. The rest are uniforms, uploaded per group when changed.
- **ARB programs to GLSL ES** (`GlAndroidPrograms.cpp`). `glProgramStringARB` translates the game's `arbvp1` /
  `arbfp1` programs (Cg output): every opcode they use, `_SAT`, swizzles and write masks, `A0` relative addressing of
  parameter arrays, `ARB_fog_linear` and `ARB_fragment_program_shadow`. `program.env` is two uniform buffers that every
  program shares. All 2,778 programs in the game's shader files translate, compile and link (checked on the emulator's
  driver). Alpha test, which desktop GL applies after a fragment program, is appended to each.
- **Stage linking.** A vertex stage is generated for its fragment stage and writes only the varyings it reads.
- **Textures** (`GlAndroidTextures.cpp`). BGRA and the reversed 4444 / 1555 formats are converted on upload; DXT goes
  to the GPU where it has S3TC and is decoded to RGBA where it doesn't (most phones, and the emulator).
  `GL_GENERATE_MIPMAP` becomes `glGenerateMipmap`.
- **Render targets** take the device's copy path as on macOS: a pbuffer framebuffer object, copied into textures.
  The copies are framebuffer blits: ES copies no depth with `glCopyTex*`, and drivers refuse color copies between
  formats (the window is RGB, the game's textures RGBA), some silently.
- **Not available:** wireframe, occlusion queries (ES 3 counts no samples; the device falls back), rectangle textures
  (ARB `RECT` lookups sample 2D textures), user clip planes (the device uses none with vertex programs), and reading
  depth back (`glReadPixels` reports the far plane).
- **Window** (`Android/MacGlContext.cpp`): EGL with an ES 3 context; the surface is made at the game's render size on
  the activity's window and dropped while the app is in the background, when the game keeps running at a slow tick.
- **Diagnostics:** `adb shell setprop debug.wow.gl 1` logs draw counts and GL errors once a second;
  `debug.wow.gltrace N` also traces every draw of N frames, checks render-target copies, and writes the generated
  programs to `Logs/gl-programs.txt`.

## Touch and small screens

The native input layer owns every finger and decides what it does, so moving, looking and pressing work at once with
the game's single mouse:

- **Left third**: a floating joystick where the thumb lands. It sends the movement keys (W/S, strafe Q/E), and
  pushing it to the edge runs.
- **Right of it, on the 3D world**: dragging turns the camera and character (mouselook); a quick tap clicks there
  (selects a unit); a long press right-clicks (talk, loot, attack).
- **On the UI** (action bars, windows, the minimap, unit frames): taps are left clicks, long presses right clicks,
  drags drag (items, spells), at the point touched.
- **Pinch** zooms the camera; a two-finger drag on a window scrolls it.
- **Overlay buttons** that the `TouchControls` add-on lays out and the native layer presses as keys, so they work
  with a thumb on the joystick and a finger on the camera: jump, interact (loot / talk / use nearby, the Switch
  port's), target nearest enemy, target nearest friendly or neutral, open chat, game menu, and a toggle to hide them.
- **Text**: tapping an edit box opens Android's keyboard; Back closes windows (Escape).

Small-screen improvements in the `TouchControls` add-on:

- UI scale from the screen's physical size (density), so text and buttons are readable on a phone; adjustable.
- Safe areas: the UI keeps clear of the notch, rounded corners and gesture bar (Android's insets).
- Bigger action buttons and keybind-free labels; auto loot on by default; the Switch port's interact and
  non-hostile targeting; a compact chat frame.
- Landscape only, the screen kept on while playing, and a 30 / 60 fps choice for battery.

Gamepads use the Switch port's mapping (play / cursor modes, the D-pad menu navigation).

## Milestones

1. **Tooling**: JDK, Android SDK command-line tools, NDK, build tools, the emulator and an arm64 system image.
2. **Boots**: `libwow.so` builds and links, the APK installs, and the app reaches the "no game data" message, then
   opens the archives with data in place (logs in `Logs/Console.txt` and `adb logcat`).
3. **Draws**: the GLES device renders the login screen correctly on the emulator.
4. **Plays**: into the world, rendering checked against the macOS client; sound through AAudio; network play.
5. **Touch**: the input router, overlay buttons and the small-screen improvements.
6. **Phone**: performance and GPU-driver fixes on the user's phone; a release build.

## Status

- **Milestone 1 (tooling): done.** JDK 17 (Homebrew), the SDK command-line tools, platform 35, build tools 35, NDK
  27.2, the emulator and an Android 15 arm64 image; the virtual device `wowtest` (Pixel 7).
- **Milestone 2 (boots): done.** `Tools/Android/build.sh` builds `bin/Android/WoW.apk` (`install` also installs and
  starts it over adb). On the `wowtest` emulator the app asks for All files access through the system page, says
  where to put the Data folder when it is missing, loads the archives, runs Lua and the glue UI, and starts drawing
  the login scene: the GLES device so far creates its EGL context, follows the window through the background, and
  clears and presents. It stops at the first shader effect, which milestone 3 provides.
  - The NDK's `_FORTIFY_SOURCE` is off (cmake/Platform.cmake): it aborts on strings stored after their header, as
    Lua's are.
  - The emulator can't hold the full 3.3.5a Data folder on a Mac short of disk space (its data partition was cut to
    4 GB). For a boot test, the large archives can be empty stand-ins; the game opens them and goes on.
- **Milestone 3 (draws): done.** The login screen renders correctly on the emulator: the interface, and the
  Icecrown scene with its skinned models, particles, lighting, fog and glow. (The emulator test used a trimmed
  `common.MPQ` holding the shaders and the scene's textures, for want of disk space; the scene and interface come from
  the locale archives.)
- **Milestone 4 (plays): in progress.** On the emulator with the full data, the game logs in to the realm, plays its
  login music through AAudio, and enters the world (Elwynn Forest), which renders correctly at about 11 frames per
  second in the emulator's software renderer. Fixed on the way:
  - The input layer ran the game's idle processing on every poll, which handled world packets during the loading
    screen; it now runs only while Android's keyboard dialog holds the game (the dialog is on a thread of its own),
    as Windows does during a window drag, so typing doesn't drop the connection.
  - A click on the focused text box reopens Android's keyboard (`CSimpleEditBox::OnLayerMouseDown`).
  - The display modes come from the display's size, not the window's, which can differ while the activity settles.
  - Texture files get 1 KB of slack in memory: the DXT upload can read a few bytes past the last mip of some files,
    which Android's allocator turns into a fault.
  - The build ID in `Logs/Console.txt` is regenerated on every build (`cmake/BuildId.cmake`).
  - The emulator needs its hardware keyboard on (`hw.keyboard = yes`) to take the Mac's keyboard.
