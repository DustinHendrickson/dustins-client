#ifndef ENGINE_SOURCE_OS_ANDROID_ANDROIDINPUT_H
#define ENGINE_SOURCE_OS_ANDROID_ANDROIDINPUT_H

// Calls between the Android Os files and the activity's event loop (WoW/Source/Android/main.cpp), and the GLES device.

#include <cstdint>

struct AInputEvent;

// The activity's thread: an input event (copied for the game thread); true if the game takes it.
int32_t AndroidInputHandleEvent(const AInputEvent* event);

// The activity's thread: the window's size in pixels, focus, and the app closing.
void AndroidInputSetSurfaceSize(int32_t width, int32_t height);
void AndroidInputSetFocus(bool focus);
void AndroidInputClose();

// The GLES device: the size it renders at (touch coordinates are scaled to it).
void AndroidInputSetRenderSize(int32_t width, int32_t height);

// What is under the mouse, for routing fingers (Input.cpp): the game's UI, the 3D world, or nothing of either (the
// login screens' background, say), which a finger treats as UI. The game registers the query (WoW/Source/Ui/Android).
enum AndroidInputPoint {
    AndroidInputPoint_None = 0,
    AndroidInputPoint_Ui = 1,
    AndroidInputPoint_World = 2,
};

void AndroidInputSetPointQuery(int32_t (*query)());

// Overlay controls (the TouchControls add-on's, through WoW/Source/Ui/Android/TouchControls.cpp): screen areas a finger
// landing in belongs to, before the UI and the world. Areas are fractions of the screen from its bottom left, as the
// game's UI measures. A key control holds its key (with Shift, Ctrl or Alt) down while the finger is; the joystick
// sends the movement keys from where the finger moved since it landed. Setting a name again replaces it.
enum AndroidInputModifier {
    AndroidInputModifier_Shift = 1,
    AndroidInputModifier_Ctrl = 2,
    AndroidInputModifier_Alt = 4,
};

void AndroidInputSetKeyControl(const char* name, float left, float bottom, float right, float top, int32_t key, uint32_t modifiers);
void AndroidInputSetJoystick(const char* name, float left, float bottom, float right, float top);
void AndroidInputRemoveControl(const char* name);
void AndroidInputClearControls();

// The joystick while a finger holds it: where it landed (fractions of the screen from the bottom left) and how far it
// is pushed, -1 to 1 each way (right and up positive). False while it isn't held.
bool AndroidInputGetJoystick(float* originX, float* originY, float* x, float* y);

// Whether a finger is holding the key control of that name, for the add-on to draw it pressed.
bool AndroidInputControlHeld(const char* name);

// OsIMEEnable: a text box gained or lost the keyboard focus.
void AndroidInputSetTextFocus(bool focus);

// OsClipboard: the game copying text while the keyboard reads a box's text; true if it was taken for that.
bool AndroidInputCaptureCopy(const char* text);

// 0 Control, 1 Shift, 2 Alt, as OsGuiIsModifierKeyDown takes them.
int32_t AndroidIsModifierDown(int32_t modifier);

#endif
