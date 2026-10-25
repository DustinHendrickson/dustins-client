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
  parameter arrays, `ARB_fog_linear` and `ARB_fragment_program_shadow`. `program.env` is a uniform array in each
  program, which uploads only the parameters that changed since it last drew (uniform buffers rewritten on nearly
  every draw make mobile drivers copy them). All 2,778 programs in the game's shader files translate, compile and link (checked on the emulator's
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
- **Program cache:** linked programs are saved in `Cache/GLES/` in the game folder, named for their sources and the
  driver, so each compiles once per install instead of hitching the game the first time an effect appears. The
  emulator's driver refuses saved programs; the cache then turns itself off for the run.
- **Diagnostics:** `adb shell setprop debug.wow.gl 1` logs, once a second, frame times (the worst, how many over 50 ms),
  draw counts, GL errors, and the time spent building programs, uploading textures, in per-draw setup and in the swap;
  `debug.wow.gltrace N` also traces every draw of N frames, checks render-target copies, and writes the generated
  programs to `Logs/gl-programs.txt`.

## Touch and small screens (milestone 5 plan)

The game has one keyboard and one mouse; a phone has several fingers at once. The design: **the native input layer
(`Os/Android/Input.cpp`) owns every finger** and turns each into keys, mouse movement or clicks according to where it
went down, so a thumb moving, a finger turning the camera and another pressing a button all work together. A Lua
add-on, **TouchControls** (packed in the APK, installed into `Interface/AddOns` at start like the Switch's
SwitchControls), draws the on-screen controls in the game's own UI and tells the native layer where they are. The game
code changes are small hooks; the rules live in the input layer and the add-on.

### How a finger is routed

When a finger goes down, the input layer decides what it is, once, for as long as it stays down:

1. **On an overlay control** (joystick area, jump, interact, the touch action buttons, ...): that control. The
   add-on registers each control's screen rectangle and what it does (`TouchControls_SetControl(name, left, bottom,
   right, top, action)`); the input layer hit-tests these first. Controls press keys or run their binding without
   touching the mouse, so they work while another finger turns the camera.
2. **On the game's UI** (a window, an action bar, the minimap, unit frames, chat): the mouse. The input layer asks the
   game whether a frame takes the mouse at that point (a new hook on `CSimpleTop`, the frame layer's mouse focus); if
   so the finger is the mouse: tap = left click, hold = right click, drag = left drag (items, spells, sliders), as now.
3. **Anywhere else, the 3D world**: the camera. Dragging turns the camera (mouselook, as right-drag), a quick tap
   clicks the world there (selects a unit, as left click), a press-and-hold right-clicks it (talk, loot, attack). A
   second finger on the world while one is turning starts a **pinch**, which zooms (mouse wheel).

Only one finger is "the mouse" at a time; while the camera finger is in mouselook, a tap on the UI by another finger
releases mouselook for the click and resumes it, so turning and clicking an action button don't fight.

### Movement and camera

- **Floating joystick** on the left third of the screen: it appears where the thumb lands. Its direction sends the
  movement keys (W / S, strafe Q / E, eight directions); pushing to the rim runs, a short push walks (the walk toggle).
  The game only moves at walk or run speed, so this is all an analog stick can do here. Double-tap the area: autorun.
- **Camera drag** turns the camera and the character together (mouselook, as holding the right mouse button), so the
  joystick always moves where you look; with a sensitivity option and the Switch's invert option.
- **Pinch** to zoom; a two-finger drag on a window scrolls it.

### Overlay controls (TouchControls add-on)

- **Jump**, **Interact** (the Switch port's: loot the nearest body, talk to the nearest NPC, use the nearest object,
  shared as `Ui/Mac/PortActions`), **Target nearest enemy** (Tab) and **nearest friendly / neutral** (the Switch's
  `TargetNonHostile`), **Chat** (opens the chat box and the keyboard), **Menu** (Escape), and a **hide controls**
  toggle for screenshots and cutscenes.
- **Action buttons**, both styles with an option to switch: a **thumb wheel** (twelve large buttons fanned
  around the bottom-right corner, bound to action bar 1, with a button that pages it to bars 2 and 3) and the
  **default bars enlarged** for touch. The default bars stay usable for drag-and-drop setup either way.
- Drawn with the game's UI art at a size from the screen's physical size; positions and opacity adjustable in an
  options panel; saved per character like other add-on settings.

### Small screens

- **UI scale from density**: the UI is sized so that a button is about 9 mm across, whatever the phone (Android's
  dpi), unless the player sets a scale in the video options.
- **Safe areas**: `UIParent` is inset by the notch, the rounded corners and the gesture bar (Android's insets), so
  nothing important sits under them.
- **Bigger targets**: enlarged action bars, close buttons and scroll bars; tooltips and chat in a readable font size;
  a compact chat frame that fades when idle.
- **Text**: tapping an edit box opens Android's keyboard (done); Back closes windows (Escape, done).
- **Quality of life from the Switch port**: auto loot on by default, interact and non-hostile targeting, back-to-play
  after windows close (here: windows don't capture the camera finger).

### Performance, battery, lifecycle

- **Frame rate**: 60 fps by default with steady frame pacing (on 90 and 120 Hz displays too), and a **battery saver**
  option capping at a steady 30; render resolution from the existing smaller modes (1080p, 900p, 720p, 540p of the
  screen's shape).
- **Thermals**: Android's performance hints (ADPF) report the game thread's work so the system sizes clocks to it.
- **Background**: the game keeps its connection with drawing stopped (done); audio pauses; on low memory
  (`onTrimMemory`) the texture cache is flushed.
- **Textures**: DXT is decoded to RGBA where the GPU lacks it, four to eight times the memory. The test phone is a
  Pixel with a Mali GPU, which has no DXT (it has ETC2 and ASTC), so this is the path there: to be measured on it, with
  transcoding to ETC2 (the same size as DXT, in every ES 3 GPU) as the fix if memory or load times suffer.

### Controllers

Bluetooth and USB gamepads get the Switch port's play / cursor modes and mapping (left stick moves, right stick looks,
face buttons are action keys), with Android's button names in the hints. Touch and a controller can be used together.

### Game code changes

- A frame-layer hook: does a UI frame take the mouse at a point (for routing fingers).
- `WoW/Source/Ui/Android/TouchControlsScript.cpp`: the add-on's Lua functions (controls' rectangles and actions,
  options, density, insets, mode), as `SwitchControlsScript.cpp` is for the Switch; Interact and non-hostile
  targeting shared with it.
- Pausing and resuming sound with the app; releasing caches on low memory.

### Order of work

1. Finger routing with the frame-layer hook; camera drag, tap and hold on the world; pinch.
2. Joystick and overlay controls with the TouchControls add-on and its Lua bridge.
3. Action buttons (wheel and enlarged bars, switchable), options panel.
4. Small-screen changes: density scale, safe areas, bigger targets, chat.
5. Frame rate cap and pacing, background audio, low memory; controllers.
6. On the phone: frame times (`debug.wow.gl`), texture memory, battery; tuning.

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
- **Milestone 5 (touch): in progress.** The finger router (`Os/Android/Input.cpp`) and the TouchControls add-on
  (`Tools/Android/assets/AddOns/TouchControls`, installed into `Interface/AddOns` at every start) are in. Tested on the
  emulator in the world with `adb shell input`: the joystick walks and strafes in all eight directions, a drag on the
  world turns the camera and the character, a tap on the world doesn't move anything, and the jump, menu, page and
  wheel controls press their keys.
  - The look: every control is a round icon (`SetPortraitToTexture`) in the minimap's ring, lit while a finger holds
    it (`TouchControls_IsHeld`). The wheel is packed into the corner around jump, no higher than about half the screen (so the right side above it is open for turning the camera), each button clear of its neighbours by a fingertip. It shows 12, 8, 6 or 4 buttons (a menu option): fewer are bigger, and the page button steps through the bar's keys in groups ("1a", "1b") before turning to the next page with anything on it (in a stance or form page 1 is that bar). It only shows what keys 1 to = cast now
    (the main bar's page, a stance's or possession's bar, a vehicle's), with range, mana, cooldown and active states;
    the game's action bars are put away, leaving the experience bar at the screen's edge and the stance and pet bars
    above it. The page button pages to the next page with anything on it, and play starts on one.
  - The micro buttons and bags live in a menu dialog (the gear at the top), with the style switch.
  - A control under a game window (a bag, the map, a dialog) gives its fingers to the window.
  - The game's cursor isn't drawn while fingers are in use, and the mouse leaves the screen after each tap, so
    nothing stays highlighted or keeps a tooltip.
  - Readable on a phone: the first time in the world the interface size is set for the screen's physical height (about
    640 units high on a phone, 120% here, so the tallest windows still fit; more on a tablet), the game's fonts are
    enlarged 15% and chat text is 16. The menu has sliders for all three (interface size 64% to 150%, applied when the
    finger lifts; text 100% to 150%; chat 10 to 24), and the game's own UI Scale slider reaches 150% too. With the
    larger interface the quest tracker sits left of the minimap, the chat above the joystick, and the experience bar
    across the middle half of the screen.
  - A drag up or down over anything that takes the mouse wheel (a list, the chat, the minimap) scrolls it; other
    drags hold the button, for sliders and dragging items. A tap skips a movie (the engine's `CSimpleMovieFrame`) or
    an in-game cinematic (the add-on).
  - The joystick's area is excluded from the system's back gesture, and the chat keeps clear of a camera hole on the
    screen's edge (the window's safe insets, checked again when the phone is turned end over end).
  - The add-on gives its actions keys F13 to F16 for the session only and never saves bindings: they come from the
    server a moment after login, and a set saved before they arrive unbinds every default key.
  - Leaving the app (or losing the focus to a system dialog) lets go of every finger, key and button. Keys count
    their holders, so two controls on one key or modifier don't release each other. A second finger landing while
    the first is still being placed waits for it (a pinch's two fingers land within a frame or two). A flick that
    lifts before it is placed clicks nothing; a camera drag that ends before the turn starts lets go off the screen.
  - The add-on: a battery saver in the menu holds the game to 30 frames a second (paced at the swap, and told to the
    display so 90 and 120 Hz screens run at the game's rate); auto loot is turned on once; the form and stealth bar
    stays on the main bar, see-through, so it still hides itself; the controls go while UIParent is hidden (the
    world map, cinematics); a window over a control takes its fingers (menus, popups, loot rolls included).
  - Rendering for phones: the menu's render resolution slider steps through the screen-shaped modes (gxResolution and
    a device restart, applied when the finger lifts); the first time, the render resolution goes to at most 1080 high
    and weather and particle density down a step. Each frame's work is reported to Android's performance hints
    (ADPF, Android 13 on) against the frame rate's time; on low memory the textures kept for reuse are let go.
  - Interact and the targetings sit in a row along the bottom, left of the wheel; the quest tracker is a short block
    beside the minimap; the chat is kept above the joystick (put back if the game restores a dragged position).
  - The right thumb's cluster turns the camera too: a finger dragged off any button (past a wider slop than a tap's)
    turns the camera from there, and buttons are hit as circles, so the gaps between them are the world's. The wheel,
    interact and targeting buttons press on the finger's lift (the game casts on the key's release anyway), so a drag
    presses nothing; jump presses on landing.
  - More touch: a tap on the UI by a second finger while the first turns the camera goes through (mouselook lets go
    for the click and takes the mouse back); a double tap on the joystick presses autorun's key; the menu has a
    camera speed slider and an invert option.
  - Tested on a Pixel 7 Pro (Tensor G2, Mali-G710, Android 17): 50 to 59 frames a second at the full 3120x1440 in
    Goldshire. In Stormwind at 2340x1080: 22 frames a second with the full-screen glow on, 63 with it off (the same
    at 1560x720 with it on), the game thread at about a third of a core and the GPU the limit. The glow copies the
    frame and renders its blur off screen, which on a tiled GPU stores and reloads the whole frame; it is off by
    default on phones (the add-on, once) with a switch in the menu, until it is reworked for tiled GPUs.
  - Memory there: graphics 290 to 620 MB (textures decoded from DXT to RGBA, which the Mali GPU needs), the game's
    heap about 340 MB, about 0.7 to 1 GB in all.
  - Going to the background saves the settings (Config.wtf) and the add-ons' saved variables: Android ends a game
    swiped away from the recent apps with no logout, which is the only other time they are written.
  - The display may run at 120 Hz whatever the game asks: the swap waits for as many refreshes as make a frame at
    the game's rate (two for 60, four for the battery saver's 30), from the display's refresh rate asked every two
    seconds.
  - `/touch debug` logs the controls' areas and each bar page's used slots to `Logs/Console.txt`; the property
    `debug.wow.input` at 1 logs every touch, how it is routed and what goes to the game (`adb logcat -s WoWInput`).
  Not yet tested: pinch (the emulator does it with Ctrl and a drag), a hold on the world, two fingers at once, and
  interact and the targeting buttons next to NPCs and enemies.
