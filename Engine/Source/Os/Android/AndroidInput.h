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

// Whether the frame under the mouse takes the mouse wheel (a list, the chat): a finger dragging up or down there
// scrolls it.
void AndroidInputSetWheelQuery(bool (*query)());

// Whether a UI frame that takes the mouse is at a point (fractions of the screen from its bottom left), whatever the
// mouse is doing: a second finger there taps it while the first turns the camera.
void AndroidInputSetUiAtPointQuery(bool (*query)(float x, float y));

// Camera drag: relative movement per pixel of finger movement (1 by default), and up and down swapped.
void AndroidInputSetCamera(float speed, bool invert);

// The key (and modifiers) a double tap on the joystick presses: autorun's binding. KEY_NONE for none.
void AndroidInputSetAutorunKey(int32_t key, uint32_t modifiers);

// The app went to the background, where Android may end it without warning: the handler runs on the game thread
// at its next input poll, to save what a logout would.
void AndroidInputSetBackgroundHandler(void (*handler)());

// The system is short of memory (onLowMemory, any thread): the handler runs on the game thread at its next input poll,
// to let go of what can be loaded again.
void AndroidInputLowMemory();
void AndroidInputSetLowMemoryHandler(void (*handler)());

// Overlay controls (the TouchControls add-on's, through WoW/Source/Ui/Android/TouchControls.cpp): screen areas a finger
// landing in belongs to, before the UI and the world. Areas are fractions of the screen from its bottom left, as the
// game's UI measures. A key control holds its key (with Shift, Ctrl or Alt) down while the finger is; the joystick
// sends the movement keys from where the finger moved since it landed. Setting a name again replaces it. A finger
// dragged off a key control turns the camera instead (the right thumb's cluster is also where it turns from).
enum AndroidInputModifier {
    AndroidInputModifier_Shift = 1,
    AndroidInputModifier_Ctrl = 2,
    AndroidInputModifier_Alt = 4,
};

enum AndroidInputControlFlag {
    AndroidInputControl_Round = 1,      // the circle in the area, not the whole of it: the gaps around are the world's
    AndroidInputControl_OnLift = 2,     // pressed when the finger lifts without dragging, not when it lands, so a drag
                                        // to turn the camera presses nothing (the game casts on the key's release anyway)
};

// holdKey: for an OnLift control, the key pressed instead when the finger stays on it without moving a moment
// (KEY_NONE for none).
void AndroidInputSetKeyControl(const char* name, float left, float bottom, float right, float top, int32_t key, uint32_t modifiers, uint32_t flags, int32_t holdKey, uint32_t holdModifiers);
void AndroidInputSetJoystick(const char* name, float left, float bottom, float right, float top);
void AndroidInputRemoveControl(const char* name);
void AndroidInputClearControls();

// The joystick while a finger holds it: where it landed (fractions of the screen from the bottom left) and how far it
// is pushed, -1 to 1 each way (right and up positive). False while it isn't held.
bool AndroidInputGetJoystick(float* originX, float* originY, float* x, float* y);

// Whether a finger is holding the key control of that name, for the add-on to draw it pressed.
bool AndroidInputControlHeld(const char* name);

// The on-screen modifier latch: Shift, Ctrl and / or Alt (AndroidInputModifier) held for the next finger on the UI or
// the world, from its landing to its lift; then it's spent (GetLatch is 0 again).
void AndroidInputSetLatch(uint32_t modifiers);
uint32_t AndroidInputGetLatch();

// Whether a finger pressed the key control of that name since the last time this was asked of it.
bool AndroidInputControlPressed(const char* name);

// OsIMEEnable: a text box gained or lost the keyboard focus.
void AndroidInputSetTextFocus(bool focus);

// OsClipboard: the game copying text while the keyboard reads a box's text; true if it was taken for that.
bool AndroidInputCaptureCopy(const char* text);

// 0 Control, 1 Shift, 2 Alt, as OsGuiIsModifierKeyDown takes them.
int32_t AndroidIsModifierDown(int32_t modifier);

#endif
