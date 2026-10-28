// Android implementation of the Os input layer (Os/Win64/Input.cpp): the game's event queue, filled from the
// activity's input events.
//
// The activity's thread (WoW/Source/Android/main.cpp) copies each touch, key and mouse event into a locked list
// (AndroidInputHandleEvent); the game thread drains it once a frame, when the queue runs dry (OsInputGet), and turns it
// into the keyboard and mouse events the game knows. The game sees a keyboard and one mouse:
//
// - Touch: each finger gets a role where it lands, kept until it lifts (docs/ANDROID_PORT.md, "How a finger is
//   routed"): on an overlay control (the TouchControls add-on's), that control; on the game's UI, the mouse (tap is a
//   left click, hold a right click, drag a left drag); on the 3D world, the camera (drag turns the camera and the
//   character, as holding the right button; tap clicks there, hold right-clicks), with a second finger on the world
//   pinching to zoom. Whether a point is UI or world is the game's call (AndroidInputSetPointQuery), read the poll
//   after the mouse moved there.
// - Keyboards (USB, Bluetooth) send their keys, and characters as typed on a US layout; Back is Escape.
// - Mice move the pointer, click and scroll.
//
// A text box that keeps the keyboard focus a moment opens Android's keyboard over a copy of its text
// (WowActivity.editText); what is entered replaces the box's text.

#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Engine/Source/Event/Types.h"
#include "Engine/Source/Os/Android/AndroidInput.h"
#include "Engine/Source/Os/Android/AndroidSystem.h"
#include "Engine/Source/Os/Gui.h"
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/Mac/OsMacWindow.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

namespace {

// Larger than the 16 events Windows keeps: one poll can produce a burst (text typed as characters).
const int32_t kQueueSize = 512;

// Touch: how far a finger moves before it drags rather than taps, in pixels at the render size, and how long it is
// held still to right-click.
const int32_t kTouchSlop = 12;
// On a button the slop is wider: a thumb pressing a button wobbles more than one tapping the world.
const int32_t kButtonSlop = 36;
const uint64_t kTouchHoldMs = 500;

// Fingers followed at once.
const int32_t kFingers = 10;

// Pinch: how far the fingers' distance changes, in pixels, for one notch of the mouse wheel.
const int32_t kPinchNotch = 36;

// A double tap on the joystick (autorun): the taps no longer than this, and no further apart.
const uint64_t kJoystickTapMs = 250;
const uint64_t kDoubleTapMs = 400;

// Scrolling a list: how far a finger drags, as a fraction of the screen's height, for one notch of the mouse wheel.
const float kScrollNotch = 0.035f;

// Joystick: how far the finger moves for a full push, as a fraction of the screen's height, and how much of that is a
// dead zone. Each of its eight directions holds the movement keys for it.
const float kJoystickRadius = 0.11f;
const float kJoystickDeadZone = 0.3f;

// A text box gets Android's keyboard once it has kept the focus a moment: some are focused only in passing (while the
// world loads), as on the Switch.
const uint64_t kKeyboardFocusMs = 200;

// An event as the activity's thread copies it (an AInputEvent is only valid during its callback).
struct RawEvent {
    enum Type {
        Touch,
        Key,
        Mouse,
    } type;

    int32_t action;         // AMOTION_EVENT_ACTION_* (masked) or AKEY_EVENT_ACTION_*
    int32_t pointerId;      // the finger an action is about
    int32_t count;          // fingers down
    float x[kFingers];
    float y[kFingers];
    int32_t ids[kFingers];
    int32_t keyCode;
    int32_t metaState;
    int32_t repeat;
    int32_t buttons;        // mouse buttons held (AMOTION_EVENT_BUTTON_*)
    float scroll;           // mouse wheel, notches up
};

std::mutex s_rawLock;
std::vector<RawEvent> s_raw;

// Window and focus changes from the activity's thread.
int32_t s_pendingWidth;
int32_t s_pendingHeight;
bool s_pendingSize;
int32_t s_pendingFocus = -1;
bool s_pendingClose;

// The game's queue and state, on the game thread.
int32_t s_queueHead;
int32_t s_queueTail;
OSEVENT s_queue[kQueueSize];
int32_t s_mouseSpeed = 10;
uint32_t s_osButtonState;
RECT s_defaultWindowRect;
OS_MOUSE_MODE s_osMouseMode;
// The game's idle processing (network and timers), which Windows and macOS run only while a modal loop holds the game
// (dragging or resizing the window); here, while Android's keyboard dialog is open (EditText).
void (*s_eventPollProc)();
C2iVector s_mousePos;
// The game's cursor is drawn only while a mouse is in use: a finger has no pointer to show (OsMacCursorHidden).
bool s_mouseInUse;
bool s_keyDown[KEY_LAST];
// How many hold each key (a finger on a control, the joystick, the keyboard): it goes up when the last lets go.
uint8_t s_keyHolds[KEY_LAST];
bool s_needPoll = true;

// The window, at the render size the device chose (AndroidInputSetRenderSize), and the surface it is shown on: touch
// coordinates come in surface pixels.
int32_t s_contentWidth;
int32_t s_contentHeight;
int32_t s_surfaceWidth;
int32_t s_surfaceHeight;
bool s_sizeChanged;

// Touch: the fingers down, each with its role.
enum FingerRole {
    Finger_Free,
    Finger_Deciding,    // the mouse moved there; UI or world is known the next poll
    Finger_Waiting,     // down while the mouse's finger is still Deciding: a pinch, or the mouse next, once it's known
    Finger_Ui,          // the mouse, on the game's UI
    Finger_World,       // on the world, not yet a tap, hold or drag
    Finger_Camera,      // turning the camera (the right button held, relative movement)
    Finger_Pinch,       // one of two fingers zooming
    Finger_Done,        // its click is done (a hold); ignored until it lifts
    Finger_Ignored,     // a finger with nothing to do (a second one on the UI, the one left after a pinch)
    Finger_Key,         // on a key control, holding its key
    Finger_Joystick,    // on the joystick
    Finger_UiTap,       // on the UI while another finger has the mouse on the world: a tap, through the camera's hold
};

struct Finger {
    int32_t id = -1;
    FingerRole role = Finger_Free;
    C2iVector start;
    C2iVector position;
    uint64_t downMs = 0;
    bool dragging = false;
    bool scrolling = false;     // Finger_Ui: dragging over a list, which scrolls it (the mouse wheel)
    bool hovering = false;      // Finger_Ui: held still, so the mouse hovers (tooltips) and right-clicks on the lift
    int32_t scrollY = 0;        // where the last wheel notch was sent from
    bool lifted = false;        // lifted while Deciding or Waiting: its tap happens once decided
    uint32_t decidePoll = 0;    // the poll after which its role can be decided
    int32_t key = KEY_NONE;     // Finger_Key: the key it holds, and its modifiers, and the control's name
    uint32_t modifiers = 0;
    std::string control;
    bool onLift = false;        // Finger_Key: its key goes down and up when it lifts, not while it's down
    int32_t holdKey = KEY_NONE; // Finger_Key: the key a still hold presses instead
    uint32_t holdModifiers = 0;
};

// Overlay controls, set by the TouchControls add-on (AndroidInputSetKeyControl, AndroidInputSetJoystick).
struct Control {
    std::string name;
    float left;     // fractions of the screen from its bottom left
    float bottom;
    float right;
    float top;
    bool joystick;
    int32_t key;
    uint32_t modifiers;
    uint32_t flags;     // AndroidInputControlFlag
    int32_t holdKey;    // pressed instead of key when the finger stays still a moment (OnLift controls)
    uint32_t holdModifiers;
};

std::vector<Control> s_controls;
// Key controls whose key a finger has pressed since the add-on last asked (AndroidInputControlPressed): a tap can be
// down and up between two of its frames.
std::vector<std::string> s_pressedControls;

// The joystick: the finger on it, and the movement keys it holds.
int32_t s_joystickFinger = -1;
uint32_t s_joystickKeys;

Finger s_fingers[kFingers];
int32_t s_mouseFinger = -1;     // the finger that owns the mouse, if any
uint32_t s_pollCount;
uint32_t s_parkMousePoll;       // the poll at which the mouse leaves the screen, 0 for none

// Camera drag: movement not yet sent (the game switches to relative mode a poll after the right button goes down).
float s_lookX;
float s_lookY;

// Pinch: its two fingers and their distance when the last notch was sent.
int32_t s_pinchA = -1;
int32_t s_pinchB = -1;
float s_pinchDistance;

// The game's answer to "what is under the mouse": AndroidInputPoint.
int32_t (*s_pointQuery)();
bool (*s_uiAtPointQuery)(float x, float y);
bool (*s_uiOverControlsQuery)(float x, float y);

// Camera drag: relative mouse movement per pixel the finger moves, and whether up and down are swapped (the
// add-on's options, AndroidInputSetCamera).
float s_cameraSpeed = 1.0f;
bool s_cameraInvert;

// Autorun on a double tap of the joystick: its binding's key (AndroidInputSetAutorunKey), and when the last tap was.
int32_t s_autorunKey = KEY_NONE;
uint32_t s_autorunModifiers;
uint64_t s_joystickTapMs;

// A tap on the UI by a second finger while the first turns the camera: mouselook lets go for the click, which happens
// once the game is out of relative mode, and takes the mouse back after (PollTouch).
bool s_tapThroughPending;

// The on-screen modifier latch (AndroidInputSetLatch): Shift, Ctrl or Alt held down for the next finger on the UI or the
// world, from its landing to its lift, then let go.
uint32_t s_latch;
int32_t s_latchFinger = -1;
C2iVector s_tapThroughPoint;
bool (*s_wheelQuery)();
bool (*s_multiLineQuery)();
void (*s_lowMemoryHandler)();
void (*s_backgroundHandler)();
std::atomic<bool> s_lowMemory;

// The game window (OsMacWindow.h): the activity's, at the render size the game asked for, and the device's listener for
// focus and the app going to the background.
int32_t s_window;
OsMacWindowListener s_listener;
void* s_listenerParam;
uint32_t s_windowGeneration;

// Text input.
bool s_textFocus;
bool s_secureFocus;
uint64_t s_textFocusSinceMs;

enum KeyboardState {
    Keyboard_Idle,
    Keyboard_Copying,   // Ctrl+A, Ctrl+C sent: the box's text arrives through the clipboard (AndroidInputCaptureCopy)
    Keyboard_Open,
};

KeyboardState s_keyboardState;
bool s_copyCaptured;
std::string s_copyText;

void ClampToContent(C2iVector& point) {
    point.x = point.x < 0 ? 0 : (point.x >= s_contentWidth ? s_contentWidth - 1 : point.x);
    point.y = point.y < 0 ? 0 : (point.y >= s_contentHeight ? s_contentHeight - 1 : point.y);
}

// Input diagnostics: with the property debug.wow.input at 1, every touch, how it is routed and what goes to the game
// are logged (adb logcat -s WoWInput).
void Trace(const char* format, ...) {
    static int32_t s_trace = -1;

    if (s_trace < 0) {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("debug.wow.input", value);
        s_trace = value[0] == '1';
    }

    if (s_trace) {
        va_list args;
        va_start(args, format);
        __android_log_vprint(ANDROID_LOG_INFO, "WoWInput", format, args);
        va_end(args);
    }
}

void PutKey(KEY key, bool down) {
    if (key >= 0 && key < KEY_LAST) {
        if (down ? s_keyHolds[key]++ > 0 : (s_keyHolds[key] == 0 || --s_keyHolds[key] > 0)) {
            Trace("key %d %s skipped: %d hold it", key, down ? "down" : "up", s_keyHolds[key]);
            return;
        }

        s_keyDown[key] = down;
    }

    Trace("key %d %s", key, down ? "down" : "up");
    OsQueuePut(down ? OS_INPUT_KEY_DOWN : OS_INPUT_KEY_UP, key, 1, 0, 0);
}

void TapKey(KEY key) {
    OsQueuePut(OS_INPUT_KEY_DOWN, key, 1, 0, 0);
    OsQueuePut(OS_INPUT_KEY_UP, key, 1, 0, 0);
}

// Ctrl+<key>, as the editing shortcuts edit boxes know.
void ControlKey(KEY key) {
    bool control = s_keyDown[KEY_LCONTROL] || s_keyDown[KEY_RCONTROL];

    if (!control) {
        OsQueuePut(OS_INPUT_KEY_DOWN, KEY_LCONTROL, 1, 0, 0);
    }

    TapKey(key);

    if (!control) {
        OsQueuePut(OS_INPUT_KEY_UP, KEY_LCONTROL, 1, 0, 0);
    }
}

void MoveMouseTo(const C2iVector& point) {
    if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
        return;
    }

    if (point.x == s_mousePos.x && point.y == s_mousePos.y) {
        return;
    }

    s_mousePos = point;
    Trace("mouse to %d,%d", point.x, point.y);
    OsQueuePut(OS_INPUT_MOUSE_MOVE, 0, point.x, point.y, 0);
}

void PutMouseButton(MOUSEBUTTON button, bool down) {
    if (down == ((s_osButtonState & button) != 0)) {
        return;
    }

    if (down) {
        s_osButtonState |= button;
    } else {
        s_osButtonState &= ~button;
    }

    Trace("mouse button %d %s at %d,%d (mode %d)", button, down ? "down" : "up", s_mousePos.x, s_mousePos.y, s_osMouseMode);
    OsQueuePut(down ? OS_INPUT_MOUSE_DOWN : OS_INPUT_MOUSE_UP, button, s_mousePos.x, s_mousePos.y, 0);
}

void PutWheel(int32_t notches) {
    if (notches) {
        Trace("wheel %d", notches);
        OsQueuePut(OS_INPUT_MOUSE_WHEEL, notches * 120, s_mousePos.x, s_mousePos.y, 0);
    }
}

// Surface pixels to the render size.
C2iVector ToContent(float x, float y) {
    C2iVector point;
    point.x = s_surfaceWidth ? static_cast<int32_t>(x * s_contentWidth / s_surfaceWidth) : static_cast<int32_t>(x);
    point.y = s_surfaceHeight ? static_cast<int32_t>(y * s_contentHeight / s_surfaceHeight) : static_cast<int32_t>(y);
    ClampToContent(point);
    return point;
}

// Characters, as UTF-16 units: the game's OS_INPUT_CHAR.
void TypeText(const std::string& text) {
    const uint8_t* c = reinterpret_cast<const uint8_t*>(text.c_str());

    while (*c) {
        uint32_t codepoint;
        int32_t extra;

        if (*c < 0x80) {
            codepoint = *c;
            extra = 0;
        } else if ((*c & 0xE0) == 0xC0) {
            codepoint = *c & 0x1F;
            extra = 1;
        } else if ((*c & 0xF0) == 0xE0) {
            codepoint = *c & 0x0F;
            extra = 2;
        } else {
            codepoint = *c & 0x07;
            extra = 3;
        }

        c++;

        for (int32_t i = 0; i < extra && (*c & 0xC0) == 0x80; i++, c++) {
            codepoint = (codepoint << 6) | (*c & 0x3F);
        }

        if (codepoint >= 0x10000) {
            codepoint -= 0x10000;
            OsQueuePut(OS_INPUT_CHAR, 0xD800 + (codepoint >> 10), 1, 0, 0);
            OsQueuePut(OS_INPUT_CHAR, 0xDC00 + (codepoint & 0x3FF), 1, 0, 0);
        } else if (codepoint >= 0x20 || codepoint == '\t') {
            OsQueuePut(OS_INPUT_CHAR, codepoint, 1, 0, 0);
        }
    }
}

// A finger still on the screen by its pointer id: one lifted while Deciding is done with its pointer, which the next
// touch may already be using.
Finger* FindFinger(int32_t id) {
    for (Finger& finger : s_fingers) {
        if (finger.role != Finger_Free && !finger.lifted && finger.id == id) {
            return &finger;
        }
    }

    return nullptr;
}

int32_t FingerIndex(const Finger* finger) {
    return static_cast<int32_t>(finger - s_fingers);
}

bool BeyondSlop(const Finger& finger) {
    return std::abs(finger.position.x - finger.start.x) > kTouchSlop || std::abs(finger.position.y - finger.start.y) > kTouchSlop;
}

void ClickAt(const C2iVector& point, MOUSEBUTTON button) {
    MoveMouseTo(point);
    PutMouseButton(button, true);
    PutMouseButton(button, false);
}

// Sends camera movement once the game is in relative mode (mouselook), which it enters a poll after the right button.
void FlushLook() {
    if (s_osMouseMode != OS_MOUSE_MODE_RELATIVE) {
        return;
    }

    int32_t dx = static_cast<int32_t>(s_lookX);
    int32_t dy = static_cast<int32_t>(s_lookY);

    if (dx || dy) {
        OsQueuePut(OS_INPUT_MOUSE_MOVE_RELATIVE, 0, dx, dy, 0);
        s_lookX -= static_cast<float>(dx);
        s_lookY -= static_cast<float>(dy);
    }
}

void EndCamera(Finger& finger) {
    if (finger.role == Finger_Camera) {
        // Not yet turning (relative mode comes a poll after the button): a release where it went down would be a
        // right-click on the world there (talk, attack), so it happens off the screen.
        if (s_osMouseMode != OS_MOUSE_MODE_RELATIVE) {
            MoveMouseTo(C2iVector { -1, -1 });
        }

        PutMouseButton(MOUSE_BUTTON_RIGHT, false);
        s_lookX = 0.0f;
        s_lookY = 0.0f;
    }
}

float PinchDistance() {
    const Finger& a = s_fingers[s_pinchA];
    const Finger& b = s_fingers[s_pinchB];
    float dx = static_cast<float>(a.position.x - b.position.x);
    float dy = static_cast<float>(a.position.y - b.position.y);
    return std::sqrt(dx * dx + dy * dy);
}

void BeginPinch(Finger& first, Finger& second) {
    EndCamera(first);

    if (s_mouseFinger == FingerIndex(&first)) {
        s_mouseFinger = -1;
    }

    first.role = Finger_Pinch;
    second.role = Finger_Pinch;
    s_pinchA = FingerIndex(&first);
    s_pinchB = FingerIndex(&second);
    s_pinchDistance = PinchDistance();
}

// The control a point is in, the last set winning where they overlap (the add-on sets them back to front). None where
// a window drawn over the controls takes the mouse there: the finger is the UI's.
const Control* ControlAt(const C2iVector& point) {
    if (!s_contentWidth || !s_contentHeight) {
        return nullptr;
    }

    float x = static_cast<float>(point.x) / static_cast<float>(s_contentWidth);
    float y = 1.0f - static_cast<float>(point.y) / static_cast<float>(s_contentHeight);

    for (auto control = s_controls.rbegin(); control != s_controls.rend(); ++control) {
        if (x >= control->left && x <= control->right && y >= control->bottom && y <= control->top) {
            if (control->flags & AndroidInputControl_Round) {
                // In pixels, where the circle is round.
                float dx = (x - (control->left + control->right) * 0.5f) * static_cast<float>(s_contentWidth);
                float dy = (y - (control->bottom + control->top) * 0.5f) * static_cast<float>(s_contentHeight);
                float radius = (control->right - control->left) * 0.5f * static_cast<float>(s_contentWidth);

                if (dx * dx + dy * dy > radius * radius) {
                    continue;
                }
            }

            if (!(control->flags & AndroidInputControl_OnTop) && s_uiOverControlsQuery && s_uiOverControlsQuery(x, y)) {
                return nullptr;
            }

            return &*control;
        }
    }

    return nullptr;
}

const KEY kModifierKeys[] = { KEY_LSHIFT, KEY_LCONTROL, KEY_LALT };

void PutControlKey(int32_t key, uint32_t modifiers, bool down) {
    if (down) {
        for (int32_t i = 0; i < 3; i++) {
            if (modifiers & (1u << i)) {
                PutKey(kModifierKeys[i], true);
            }
        }

        if (key != KEY_NONE) {
            PutKey(static_cast<KEY>(key), true);
        }
    } else {
        if (key != KEY_NONE) {
            PutKey(static_cast<KEY>(key), false);
        }

        for (int32_t i = 2; i >= 0; i--) {
            if (modifiers & (1u << i)) {
                PutKey(kModifierKeys[i], false);
            }
        }
    }
}

// The joystick's push as -1 to 1 each way, right and up positive.
void JoystickPush(const Finger& finger, float* x, float* y) {
    float radius = kJoystickRadius * static_cast<float>(s_contentHeight);
    *x = static_cast<float>(finger.position.x - finger.start.x) / radius;
    *y = static_cast<float>(finger.start.y - finger.position.y) / radius;
    float length = std::sqrt(*x * *x + *y * *y);

    if (length > 1.0f) {
        *x /= length;
        *y /= length;
    }
}

// Holds the movement keys for the joystick's direction: one of eight, or none inside the dead zone.
void SetJoystickKeys(uint32_t keys) {
    static const KEY s_keys[] = { KEY_W, KEY_S, KEY_Q, KEY_E };

    for (int32_t i = 0; i < 4; i++) {
        uint32_t bit = 1u << i;

        if ((keys & bit) != (s_joystickKeys & bit)) {
            PutKey(s_keys[i], (keys & bit) != 0);
        }
    }

    s_joystickKeys = keys;
}

void UpdateJoystick(const Finger& finger) {
    float x;
    float y;
    JoystickPush(finger, &x, &y);

    float length = std::sqrt(x * x + y * y);
    uint32_t keys = 0;

    if (length >= kJoystickDeadZone) {
        // sin 22.5 degrees: each direction covers 45.
        float nx = x / length;
        float ny = y / length;
        keys |= ny > 0.383f ? 1u : 0u;
        keys |= ny < -0.383f ? 2u : 0u;
        keys |= nx < -0.383f ? 4u : 0u;
        keys |= nx > 0.383f ? 8u : 0u;
    }

    SetJoystickKeys(keys);
}

void FingerDown(int32_t id, const C2iVector& point, uint64_t now) {
    Finger* finger = nullptr;

    for (Finger& slot : s_fingers) {
        if (slot.role == Finger_Free) {
            finger = &slot;
            break;
        }
    }

    if (!finger) {
        Trace("finger %d down at %d,%d: no free slot", id, point.x, point.y);
        return;
    }

    *finger = Finger();
    finger->id = id;
    finger->start = point;
    finger->position = point;
    finger->downMs = now;

    Trace("finger %d down at %d,%d (content %dx%d, %zu controls, mouse finger %d, joystick finger %d)", id, point.x, point.y,
        s_contentWidth, s_contentHeight, s_controls.size(), s_mouseFinger, s_joystickFinger);

    // Overlay controls come first, whatever the game has under them.
    if (const Control* control = ControlAt(point)) {
        Trace("  on control %s", control->name.c_str());

        if (control->joystick) {
            if (s_joystickFinger < 0) {
                finger->role = Finger_Joystick;
                s_joystickFinger = FingerIndex(finger);

                // The second tap of a double tap: autorun (moving after it stops it, as the movement keys do).
                if (s_autorunKey != KEY_NONE && s_joystickTapMs && now - s_joystickTapMs <= kDoubleTapMs) {
                    s_joystickTapMs = 0;
                    PutControlKey(s_autorunKey, s_autorunModifiers, true);
                    PutControlKey(s_autorunKey, s_autorunModifiers, false);
                }
            } else {
                finger->role = Finger_Ignored;
            }
        } else {
            finger->role = Finger_Key;
            finger->key = control->key;
            finger->modifiers = control->modifiers;
            finger->control = control->name;
            finger->onLift = (control->flags & AndroidInputControl_OnLift) != 0;
            finger->holdKey = finger->onLift ? control->holdKey : KEY_NONE;
            finger->holdModifiers = control->holdModifiers;

            if (!finger->onLift) {
                PutControlKey(finger->key, finger->modifiers, true);
            }
        }

        return;
    }

    if (s_mouseFinger < 0) {
        // A latched modifier goes down with it, for its click (Shift + click, Ctrl + click, ...).
        if (s_latch && s_latchFinger < 0) {
            s_latchFinger = FingerIndex(finger);
            PutControlKey(KEY_NONE, s_latch, true);
        }

        // The mouse goes there; once the game has seen it move, its focus says whether this is UI or world.
        finger->role = Finger_Deciding;
        finger->decidePoll = s_pollCount + 1;
        s_mouseFinger = FingerIndex(finger);
        MoveMouseTo(point);
        return;
    }

    Finger& owner = s_fingers[s_mouseFinger];

    // The first finger isn't placed yet (it decides a poll or two after landing, and two fingers of a pinch land
    // closer together than that): this one waits for it (PollTouch).
    if (owner.role == Finger_Deciding) {
        finger->role = Finger_Waiting;
        return;
    }

    // A second finger on the UI while the first is on the world taps the UI (an action bar, a unit frame, a window),
    // through the camera's hold on the mouse; elsewhere it zooms.
    if (owner.role == Finger_World || owner.role == Finger_Camera) {
        float x = s_contentWidth ? static_cast<float>(point.x) / static_cast<float>(s_contentWidth) : 0.0f;
        float y = s_contentHeight ? 1.0f - static_cast<float>(point.y) / static_cast<float>(s_contentHeight) : 0.0f;

        if (s_uiAtPointQuery && s_uiAtPointQuery(x, y)) {
            finger->role = Finger_UiTap;
            Trace("  on the UI beside the camera");
            return;
        }

        BeginPinch(owner, *finger);
        return;
    }

    finger->role = Finger_Ignored;
}

// Further than a tap's wobble from where it landed on a button: a drag, which turns the camera.
bool BeyondButtonSlop(const Finger& finger) {
    return std::abs(finger.position.x - finger.start.x) > kButtonSlop || std::abs(finger.position.y - finger.start.y) > kButtonSlop;
}

void ButtonToCamera(Finger& finger, const C2iVector& point) {
    if (!finger.onLift) {
        PutControlKey(finger.key, finger.modifiers, false);
    }

    finger.control.clear();

    // The mouse is another finger's: nothing for this one.
    if (s_mouseFinger >= 0) {
        finger.role = Finger_Ignored;
        return;
    }

    Trace("finger %d dragged off its button: camera", finger.id);
    s_mouseFinger = FingerIndex(&finger);
    MoveMouseTo(finger.start);
    PutMouseButton(MOUSE_BUTTON_RIGHT, true);
    finger.role = Finger_Camera;
    s_lookX = static_cast<float>(point.x - finger.start.x) * s_cameraSpeed;
    s_lookY = static_cast<float>(point.y - finger.start.y) * s_cameraSpeed * (s_cameraInvert ? -1.0f : 1.0f);
}

// A finger on the world: a tap left-clicks there (selects), a hold right-clicks (talk, loot, attack).
void WorldTap(Finger& finger) {
    ClickAt(finger.start, MOUSE_BUTTON_LEFT);
}

void FingerMove(Finger& finger, const C2iVector& point) {
    C2iVector previous = finger.position;
    finger.position = point;

    switch (finger.role) {
        case Finger_Ui:
            // Hovering, the mouse follows the finger with no button, so its tooltip follows too (reading along a bag).
            if (finger.hovering) {
                MoveMouseTo(point);
                break;
            }

            // Up or down over something that takes the mouse wheel (a list, the chat) scrolls it; any other drag holds
            // the button down and moves the mouse, for sliders, item dragging and moving windows.
            if (!finger.dragging && !finger.scrolling && BeyondSlop(finger)) {
                int32_t dx = std::abs(point.x - finger.start.x);
                int32_t dy = std::abs(point.y - finger.start.y);

                if (dy > dx && s_wheelQuery && s_wheelQuery()) {
                    finger.scrolling = true;
                    finger.scrollY = finger.start.y;
                } else {
                    PutMouseButton(MOUSE_BUTTON_LEFT, true);
                    finger.dragging = true;
                }
            }

            if (finger.scrolling) {
                // Content follows the finger: dragging down shows what is above, a notch up of the wheel.
                int32_t notch = std::max(1, static_cast<int32_t>(s_contentHeight * kScrollNotch));
                int32_t notches = (point.y - finger.scrollY) / notch;

                if (notches) {
                    PutWheel(notches);
                    finger.scrollY += notches * notch;
                }
            } else if (finger.dragging) {
                MoveMouseTo(point);
            }

            break;

        case Finger_World:
            if (BeyondSlop(finger)) {
                // Turning starts where the finger went down: the mouse is there, on the world.
                PutMouseButton(MOUSE_BUTTON_RIGHT, true);
                finger.role = Finger_Camera;
                s_lookX = static_cast<float>(point.x - finger.start.x) * s_cameraSpeed;
                s_lookY = static_cast<float>(point.y - finger.start.y) * s_cameraSpeed * (s_cameraInvert ? -1.0f : 1.0f);
            }

            break;

        case Finger_Camera:
            s_lookX += static_cast<float>(point.x - previous.x) * s_cameraSpeed;
            s_lookY += static_cast<float>(point.y - previous.y) * s_cameraSpeed * (s_cameraInvert ? -1.0f : 1.0f);
            FlushLook();
            break;

        case Finger_Joystick:
            UpdateJoystick(finger);
            break;

        case Finger_Key:
            // Dragged off its button: the camera turns from there (the button's key, if down, lets go first).
            if (BeyondButtonSlop(finger)) {
                ButtonToCamera(finger, point);
            }

            break;

        case Finger_Pinch: {
            if (s_pinchA < 0 || s_pinchB < 0) {
                break;
            }

            float distance = PinchDistance();
            float change = distance - s_pinchDistance;

            // Spreading the fingers zooms in (the wheel forward).
            while (change >= kPinchNotch) {
                PutWheel(1);
                change -= kPinchNotch;
                s_pinchDistance += kPinchNotch;
            }

            while (change <= -kPinchNotch) {
                PutWheel(-1);
                change += kPinchNotch;
                s_pinchDistance -= kPinchNotch;
            }

            break;
        }

        default:
            break;
    }
}

void FingerUp(Finger& finger, bool cancel) {
    Trace("finger %d %s (role %d)", finger.id, cancel ? "cancelled" : "up", finger.role);

    switch (finger.role) {
        case Finger_Deciding:
        case Finger_Waiting:
            if (!cancel) {
                // Its tap happens once the game says what it is on.
                finger.lifted = true;
                return;
            }

            break;

        case Finger_Ui:
            if (finger.scrolling) {
                break;
            }

            // A hold read the tooltip: lifted where it began, the right-click it stood for; slid away first, nothing.
            if (finger.hovering) {
                if (!cancel && !BeyondButtonSlop(finger)) {
                    ClickAt(finger.start, MOUSE_BUTTON_RIGHT);
                }

                break;
            }

            if (finger.dragging) {
                PutMouseButton(MOUSE_BUTTON_LEFT, false);
            } else if (!cancel) {
                ClickAt(finger.start, MOUSE_BUTTON_LEFT);
            }

            break;

        case Finger_World:
            if (!cancel) {
                WorldTap(finger);
            }

            break;

        case Finger_Camera:
            EndCamera(finger);
            break;

        case Finger_Key:
            if (!finger.onLift) {
                PutControlKey(finger.key, finger.modifiers, false);
            } else if (!cancel) {
                PutControlKey(finger.key, finger.modifiers, true);
                PutControlKey(finger.key, finger.modifiers, false);
            }

            if (!cancel && !finger.control.empty()) {
                // Only some controls are asked about: the oldest go once there are more than a few.
                if (s_pressedControls.size() >= 16) {
                    s_pressedControls.erase(s_pressedControls.begin());
                }

                s_pressedControls.push_back(finger.control);
            }

            break;

        case Finger_Joystick:
            SetJoystickKeys(0);
            s_joystickFinger = -1;

            // A quick touch that didn't push: half of a double tap (FingerDown).
            if (!cancel && !BeyondSlop(finger) && OsGetAsyncTimeMs() - finger.downMs <= kJoystickTapMs) {
                s_joystickTapMs = OsGetAsyncTimeMs();
            }

            break;

        case Finger_UiTap:
            if (!cancel && !BeyondSlop(finger)) {
                s_tapThroughPending = true;
                s_tapThroughPoint = finger.start;

                // Turning the camera, the right button holds the mouse: it lets go first.
                if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
                    PutMouseButton(MOUSE_BUTTON_RIGHT, false);
                }
            }

            break;

        case Finger_Pinch: {
            // The other finger has nothing left to do until it lifts too.
            int32_t other = FingerIndex(&finger) == s_pinchA ? s_pinchB : s_pinchA;

            if (other >= 0 && s_fingers[other].role == Finger_Pinch) {
                s_fingers[other].role = Finger_Ignored;
            }

            s_pinchA = -1;
            s_pinchB = -1;
            break;
        }

        default:
            break;
    }

    // The latched modifier's finger has done its click: the modifier lets go, and the latch is spent.
    if (s_latchFinger == FingerIndex(&finger)) {
        PutControlKey(KEY_NONE, s_latch, false);
        s_latch = 0;
        s_latchFinger = -1;
    }

    if (s_mouseFinger == FingerIndex(&finger)) {
        s_mouseFinger = -1;

        // With no finger on it, the mouse leaves the screen once the game has taken the tap where it was, so nothing
        // stays highlighted or keeps its tooltip.
        s_parkMousePoll = s_pollCount + 2;
    }

    finger = Finger();
}

void HandleTouch(const RawEvent& event, uint64_t now) {
    if (event.action != AMOTION_EVENT_ACTION_MOVE) {
        Trace("touch action %d pointer %d of %d", event.action, event.pointerId, event.count);
    }

    switch (event.action) {
        case AMOTION_EVENT_ACTION_DOWN:
            // The first finger on an empty screen: any other still thought down missed its lift (a gesture the system
            // took, say), and lets go now rather than holding the mouse or the joystick for good.
            for (Finger& finger : s_fingers) {
                if (finger.role != Finger_Free && !finger.lifted) {
                    FingerUp(finger, true);
                }
            }

            [[fallthrough]];

        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            for (int32_t i = 0; i < event.count && i < kFingers; i++) {
                if (event.ids[i] == event.pointerId) {
                    FingerDown(event.pointerId, ToContent(event.x[i], event.y[i]), now);
                }
            }

            break;

        case AMOTION_EVENT_ACTION_MOVE:
            for (int32_t i = 0; i < event.count && i < kFingers; i++) {
                if (Finger* finger = FindFinger(event.ids[i])) {
                    FingerMove(*finger, ToContent(event.x[i], event.y[i]));
                }
            }

            break;

        case AMOTION_EVENT_ACTION_POINTER_UP:
        case AMOTION_EVENT_ACTION_UP:
            if (Finger* finger = FindFinger(event.pointerId)) {
                FingerUp(*finger, false);
            }

            break;

        case AMOTION_EVENT_ACTION_CANCEL:
            for (Finger& finger : s_fingers) {
                if (finger.role != Finger_Free) {
                    FingerUp(finger, true);
                }
            }

            break;

        default:
            break;
    }
}

// Each poll: deciding fingers learn what they are on, held ones right-click, and camera movement waiting for relative
// mode goes out.
void PollTouch(uint64_t now) {
    s_pollCount++;

    if (s_parkMousePoll && s_pollCount >= s_parkMousePoll) {
        s_parkMousePoll = 0;

        if (s_mouseFinger < 0) {
            MoveMouseTo(C2iVector { -1, -1 });
        }
    }

    for (Finger& finger : s_fingers) {
        if (finger.role == Finger_Deciding && s_pollCount > finger.decidePoll) {
            int32_t point = s_pointQuery ? s_pointQuery() : AndroidInputPoint_Ui;
            finger.role = point == AndroidInputPoint_World ? Finger_World : Finger_Ui;
            Trace("finger %d decided: %s", finger.id, point == AndroidInputPoint_World ? "world" : point == AndroidInputPoint_Ui ? "ui" : "nothing (ui)");

            if (finger.lifted) {
                // Moved past the slop before it was known what it was on: a flick, which clicks nothing.
                FingerUp(finger, BeyondSlop(finger));
                continue;
            }

            // It may have moved meanwhile: a drag starts now.
            FingerMove(finger, finger.position);
        }

        // A finger that landed while the mouse's was deciding: with it on the world, the two pinch; with it gone (a
        // quick tap), this one is the mouse now, and taps in turn; with it on the UI, this one has nothing to do.
        if (finger.role == Finger_Waiting) {
            Finger* owner = s_mouseFinger >= 0 ? &s_fingers[s_mouseFinger] : nullptr;

            if (!owner) {
                finger.role = Finger_Deciding;
                finger.decidePoll = s_pollCount + 1;
                s_mouseFinger = FingerIndex(&finger);
                MoveMouseTo(finger.start);
            } else if (owner->role == Finger_Deciding) {
                continue;
            } else if (finger.lifted) {
                FingerUp(finger, true);
            } else if (owner->role == Finger_World || owner->role == Finger_Camera) {
                BeginPinch(*owner, finger);
            } else {
                finger.role = Finger_Ignored;
            }
        }

        // A still hold on a control with a hold key: that key instead of the control's (the spellbook from a wheel
        // button); the lift then presses nothing.
        if (finger.role == Finger_Key && finger.holdKey != KEY_NONE && !BeyondButtonSlop(finger) && now - finger.downMs >= kTouchHoldMs) {
            Trace("finger %d held on %s: hold key", finger.id, finger.control.c_str());
            PutControlKey(finger.holdKey, finger.holdModifiers, true);
            PutControlKey(finger.holdKey, finger.holdModifiers, false);
            finger.role = Finger_Done;
            finger.control.clear();
            continue;
        }

        // Held still on the UI: a hover, for its tooltip; the right-click waits for the lift (FingerUp), so reading an
        // item doesn't use, sell or buy it. On the world, a hold right-clicks at once (talk, loot, attack).
        if (finger.role == Finger_Ui && !finger.dragging && !finger.scrolling && !finger.hovering && now - finger.downMs >= kTouchHoldMs) {
            Trace("finger %d hovering", finger.id);
            finger.hovering = true;
        } else if (finger.role == Finger_World && now - finger.downMs >= kTouchHoldMs) {
            ClickAt(finger.start, MOUSE_BUTTON_RIGHT);
            finger.role = Finger_Done;
        }
    }

    if (s_tapThroughPending && s_osMouseMode != OS_MOUSE_MODE_RELATIVE) {
        s_tapThroughPending = false;
        ClickAt(s_tapThroughPoint, MOUSE_BUTTON_LEFT);

        // The camera finger takes the mouse back where it is, and turns on from there.
        if (s_mouseFinger >= 0 && s_fingers[s_mouseFinger].role == Finger_Camera) {
            Finger& camera = s_fingers[s_mouseFinger];
            camera.start = camera.position;
            MoveMouseTo(camera.position);
            PutMouseButton(MOUSE_BUTTON_RIGHT, true);
            s_lookX = 0.0f;
            s_lookY = 0.0f;
        } else if (s_mouseFinger >= 0) {
            MoveMouseTo(s_fingers[s_mouseFinger].position);
        }
    }

    FlushLook();
}

// The app lost the focus or went to the background, where no lifts come: every finger lets go, and every key and
// button still down goes up, so nothing stays held (the joystick running on, a drag, mouselook).
void ReleaseAll() {
    for (Finger& finger : s_fingers) {
        if (finger.role != Finger_Free) {
            FingerUp(finger, true);
        }
    }

    for (int32_t key = 0; key < KEY_LAST; key++) {
        if (s_keyHolds[key]) {
            s_keyHolds[key] = 1;
            PutKey(static_cast<KEY>(key), false);
        }
    }

    PutMouseButton(MOUSE_BUTTON_LEFT, false);
    PutMouseButton(MOUSE_BUTTON_RIGHT, false);
    PutMouseButton(MOUSE_BUTTON_MIDDLE, false);
    s_lookX = 0.0f;
    s_lookY = 0.0f;
    s_tapThroughPending = false;
}

void HandleMouse(const RawEvent& event) {
    static C2iVector s_last;
    C2iVector point = ToContent(event.x[0], event.y[0]);

    // While the game turns the camera (relative mode) the mouse's movement is what counts, not where it is.
    if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
        if (point.x != s_last.x || point.y != s_last.y) {
            OsQueuePut(OS_INPUT_MOUSE_MOVE_RELATIVE, 0, point.x - s_last.x, point.y - s_last.y, 0);
        }
    } else {
        MoveMouseTo(point);
    }

    s_last = point;

    // Buttons change only on button events: a hover reports none held, which would let go of a finger's drag.
    switch (event.action) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_BUTTON_PRESS:
        case AMOTION_EVENT_ACTION_BUTTON_RELEASE:
            PutMouseButton(MOUSE_BUTTON_LEFT, (event.buttons & AMOTION_EVENT_BUTTON_PRIMARY) != 0);
            PutMouseButton(MOUSE_BUTTON_RIGHT, (event.buttons & AMOTION_EVENT_BUTTON_SECONDARY) != 0);
            PutMouseButton(MOUSE_BUTTON_MIDDLE, (event.buttons & AMOTION_EVENT_BUTTON_TERTIARY) != 0);
            break;

        default:
            break;
    }

    if (event.scroll != 0.0f) {
        PutWheel(event.scroll > 0.0f ? 1 : -1);
    }
}

KEY KeyFromCode(int32_t code) {
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) {
        return static_cast<KEY>(KEY_A + (code - AKEYCODE_A));
    }

    if (code >= AKEYCODE_0 && code <= AKEYCODE_9) {
        return static_cast<KEY>(KEY_0 + (code - AKEYCODE_0));
    }

    if (code >= AKEYCODE_F1 && code <= AKEYCODE_F12) {
        return static_cast<KEY>(KEY_F1 + (code - AKEYCODE_F1));
    }

    if (code >= AKEYCODE_NUMPAD_0 && code <= AKEYCODE_NUMPAD_9) {
        return static_cast<KEY>(KEY_NUMPAD0 + (code - AKEYCODE_NUMPAD_0));
    }

    switch (code) {
        case AKEYCODE_SHIFT_LEFT: return KEY_LSHIFT;
        case AKEYCODE_SHIFT_RIGHT: return KEY_RSHIFT;
        case AKEYCODE_CTRL_LEFT: return KEY_LCONTROL;
        case AKEYCODE_CTRL_RIGHT: return KEY_RCONTROL;
        case AKEYCODE_ALT_LEFT: return KEY_LALT;
        case AKEYCODE_ALT_RIGHT: return KEY_RALT;
        case AKEYCODE_SPACE: return KEY_SPACE;
        case AKEYCODE_GRAVE: return KEY_TILDE;
        case AKEYCODE_EQUALS: return KEY_PLUS;
        case AKEYCODE_MINUS: return KEY_MINUS;
        case AKEYCODE_LEFT_BRACKET: return KEY_BRACKET_OPEN;
        case AKEYCODE_RIGHT_BRACKET: return KEY_BRACKET_CLOSE;
        case AKEYCODE_SLASH: return KEY_SLASH;
        case AKEYCODE_BACKSLASH: return KEY_BACKSLASH;
        case AKEYCODE_SEMICOLON: return KEY_SEMICOLON;
        case AKEYCODE_APOSTROPHE: return KEY_APOSTROPHE;
        case AKEYCODE_COMMA: return KEY_COMMA;
        case AKEYCODE_PERIOD: return KEY_PERIOD;
        case AKEYCODE_ESCAPE:
        case AKEYCODE_BACK: return KEY_ESCAPE;
        case AKEYCODE_ENTER:
        case AKEYCODE_NUMPAD_ENTER: return KEY_ENTER;
        case AKEYCODE_DEL: return KEY_BACKSPACE;
        case AKEYCODE_FORWARD_DEL: return KEY_DELETE;
        case AKEYCODE_TAB: return KEY_TAB;
        case AKEYCODE_DPAD_LEFT: return KEY_LEFT;
        case AKEYCODE_DPAD_UP: return KEY_UP;
        case AKEYCODE_DPAD_RIGHT: return KEY_RIGHT;
        case AKEYCODE_DPAD_DOWN: return KEY_DOWN;
        case AKEYCODE_INSERT: return KEY_INSERT;
        case AKEYCODE_MOVE_HOME: return KEY_HOME;
        case AKEYCODE_MOVE_END: return KEY_END;
        case AKEYCODE_PAGE_UP: return KEY_PAGEUP;
        case AKEYCODE_PAGE_DOWN: return KEY_PAGEDOWN;
        case AKEYCODE_CAPS_LOCK: return KEY_CAPSLOCK;
        case AKEYCODE_NUM_LOCK: return KEY_NUMLOCK;
        case AKEYCODE_SCROLL_LOCK: return KEY_SCROLLLOCK;
        case AKEYCODE_BREAK: return KEY_PAUSE;
        case AKEYCODE_SYSRQ: return KEY_PRINTSCREEN;
        case AKEYCODE_NUMPAD_ADD: return KEY_NUMPAD_PLUS;
        case AKEYCODE_NUMPAD_SUBTRACT: return KEY_NUMPAD_MINUS;
        case AKEYCODE_NUMPAD_MULTIPLY: return KEY_NUMPAD_MULTIPLY;
        case AKEYCODE_NUMPAD_DIVIDE: return KEY_NUMPAD_DIVIDE;
        case AKEYCODE_NUMPAD_DOT: return KEY_NUMPAD_DECIMAL;
        case AKEYCODE_NUMPAD_EQUALS: return KEY_NUMPAD_EQUALS;
        default: return KEY_NONE;
    }
}

// The character a key types on a US layout, 0 for none.
uint16_t KeyChar(int32_t code, bool shift, bool capsLock) {
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) {
        bool upper = shift != capsLock;
        return static_cast<uint16_t>((upper ? 'A' : 'a') + (code - AKEYCODE_A));
    }

    if (code >= AKEYCODE_0 && code <= AKEYCODE_9) {
        static const char s_shifted[] = ")!@#$%^&*(";
        return static_cast<uint16_t>(shift ? s_shifted[code - AKEYCODE_0] : '0' + (code - AKEYCODE_0));
    }

    if (code >= AKEYCODE_NUMPAD_0 && code <= AKEYCODE_NUMPAD_9) {
        return static_cast<uint16_t>('0' + (code - AKEYCODE_NUMPAD_0));
    }

    switch (code) {
        case AKEYCODE_SPACE: return ' ';
        case AKEYCODE_GRAVE: return shift ? '~' : '`';
        case AKEYCODE_MINUS: return shift ? '_' : '-';
        case AKEYCODE_EQUALS: return shift ? '+' : '=';
        case AKEYCODE_LEFT_BRACKET: return shift ? '{' : '[';
        case AKEYCODE_RIGHT_BRACKET: return shift ? '}' : ']';
        case AKEYCODE_BACKSLASH: return shift ? '|' : '\\';
        case AKEYCODE_SEMICOLON: return shift ? ':' : ';';
        case AKEYCODE_APOSTROPHE: return shift ? '"' : '\'';
        case AKEYCODE_COMMA: return shift ? '<' : ',';
        case AKEYCODE_PERIOD: return shift ? '>' : '.';
        case AKEYCODE_SLASH: return shift ? '?' : '/';
        case AKEYCODE_NUMPAD_ADD: return '+';
        case AKEYCODE_NUMPAD_SUBTRACT: return '-';
        case AKEYCODE_NUMPAD_MULTIPLY: return '*';
        case AKEYCODE_NUMPAD_DIVIDE: return '/';
        case AKEYCODE_NUMPAD_DOT: return '.';
        default: return 0;
    }
}

void HandleKey(const RawEvent& event) {
    KEY key = KeyFromCode(event.keyCode);
    bool down = event.action == AKEY_EVENT_ACTION_DOWN;

    if (key != KEY_NONE) {
        if (down && event.repeat > 0) {
            // Auto-repeat: another press without a release, as Windows sends it.
            OsQueuePut(OS_INPUT_KEY_DOWN, key, event.repeat + 1, 0, 0);
        } else {
            PutKey(key, down);
        }
    }

    bool commandKeys = (event.metaState & (AMETA_CTRL_ON | AMETA_ALT_ON | AMETA_META_ON)) != 0;

    if (down && !commandKeys) {
        uint16_t ch = KeyChar(event.keyCode, (event.metaState & AMETA_SHIFT_ON) != 0, (event.metaState & AMETA_CAPS_LOCK_ON) != 0);

        if (ch) {
            OsQueuePut(OS_INPUT_CHAR, ch, 1, 0, 0);
        }
    }
}

void Notify(OsMacWindowEvent event) {
    if (s_listener) {
        s_listener(s_listenerParam, event);
    }
}

bool TextMode() {
    return s_textFocus || s_secureFocus;
}

// Android's keyboard dialog, which takes the player's time: it runs on a thread of its own while this one keeps the
// game's network and timers going (the idle processing Windows runs during a window drag), so a long chat message
// doesn't drop the connection. Nothing is drawn meanwhile; the dialog covers the game.
bool EditText(const char* title, const char* text, bool password, std::string* result) {
    std::mutex lock;
    std::condition_variable doneChanged;
    bool done = false;
    bool entered = false;

    std::thread dialog([&] {
        entered = AndroidEditText(title, text, password, result);

        std::lock_guard<std::mutex> guard(lock);
        done = true;
        doneChanged.notify_all();
    });

    std::unique_lock<std::mutex> guard(lock);

    while (!done) {
        if (doneChanged.wait_for(guard, std::chrono::milliseconds(33), [&] { return done; })) {
            break;
        }

        guard.unlock();

        if (s_eventPollProc) {
            s_eventPollProc();
        }

        guard.lock();
    }

    guard.unlock();
    dialog.join();

    return entered;
}

// The keyboard for a focused text box: first its text (Ctrl+A, Ctrl+C, captured on the way to the clipboard), then
// Android's keyboard over it (on a later poll, once the game has handled the keys).
void PollKeyboard(uint64_t now) {
    switch (s_keyboardState) {
        case Keyboard_Idle:
            if (TextMode() && s_textFocusSinceMs && now - s_textFocusSinceMs >= kKeyboardFocusMs) {
                s_textFocusSinceMs = 0;
                s_copyText.clear();
                s_copyCaptured = false;

                if (s_secureFocus) {
                    s_keyboardState = Keyboard_Open;
                } else {
                    ControlKey(KEY_A);
                    ControlKey(KEY_C);
                    s_keyboardState = Keyboard_Copying;
                }
            }

            break;

        case Keyboard_Copying:
            s_keyboardState = Keyboard_Open;
            break;

        case Keyboard_Open: {
            s_keyboardState = Keyboard_Idle;

            if (!TextMode()) {
                break;
            }

            std::string text;

            if (EditText(s_secureFocus ? "Password" : "World of Warcraft", s_copyText.c_str(), s_secureFocus, &text)) {
                ControlKey(KEY_A);

                if (text.empty()) {
                    TapKey(KEY_BACKSPACE);
                } else {
                    TypeText(text);
                }

                // OK (or the keyboard's Done) is the box's Enter: the chat line goes, a popup accepts, the password
                // logs in. A phone has no Enter key of its own. Not in a box of several lines, where Enter is a new
                // line.
                if (!s_multiLineQuery || !s_multiLineQuery()) {
                    TapKey(KEY_ENTER);
                }
            }

            break;
        }
    }
}

void UpdateGestureExclusion();

void Poll() {
    uint64_t now = OsGetAsyncTimeMs();
    std::vector<RawEvent> events;
    bool exclusionStale = false;

    {
        std::lock_guard<std::mutex> lock(s_rawLock);
        events.swap(s_raw);

        if (s_pendingSize) {
            s_pendingSize = false;
            s_surfaceWidth = s_pendingWidth;
            s_surfaceHeight = s_pendingHeight;
            exclusionStale = true;
        }

        if (s_pendingFocus >= 0) {
            if (!s_pendingFocus) {
                ReleaseAll();
            }

            OsQueuePut(OS_INPUT_FOCUS, s_pendingFocus, 0, 0, 0);
            Notify(s_pendingFocus ? OsMacWindowEvent_FocusGained : OsMacWindowEvent_FocusLost);
            s_pendingFocus = -1;
        }

        if (s_pendingClose) {
            s_pendingClose = false;
            OsQueuePut(OS_INPUT_CLOSE, 0, 0, 0, 0);
        }
    }

    for (const RawEvent& event : events) {
        switch (event.type) {
            case RawEvent::Touch:
                s_mouseInUse = false;
                HandleTouch(event, now);
                break;
            case RawEvent::Key:
                HandleKey(event);
                break;
            case RawEvent::Mouse:
                s_mouseInUse = true;
                HandleMouse(event);
                break;
        }
    }

    // The app went to the background (no window) or came back: the device stops drawing meanwhile.
    uint32_t generation;
    bool window = AndroidGetWindow(&generation) != nullptr;

    if (generation != s_windowGeneration) {
        s_windowGeneration = generation;

        if (!window) {
            ReleaseAll();

            if (s_backgroundHandler) {
                s_backgroundHandler();
            }
        }

        Notify(window ? OsMacWindowEvent_Restored : OsMacWindowEvent_Minimized);
    }

    if (s_lowMemory.exchange(false) && s_lowMemoryHandler) {
        s_lowMemoryHandler();
    }

    // The joystick's gesture exclusion is in the surface's pixels.
    if (exclusionStale) {
        UpdateGestureExclusion();
    }

    PollTouch(now);
    PollKeyboard(now);
}

}

int32_t AndroidInputHandleEvent(const AInputEvent* input) {
    RawEvent event = {};
    int32_t source = AInputEvent_getSource(input);

    if (AInputEvent_getType(input) == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(input);

        // Volume keys stay with the system.
        if (code == AKEYCODE_VOLUME_UP || code == AKEYCODE_VOLUME_DOWN || code == AKEYCODE_VOLUME_MUTE) {
            return 0;
        }

        event.type = RawEvent::Key;
        event.action = AKeyEvent_getAction(input);
        event.keyCode = code;
        event.metaState = AKeyEvent_getMetaState(input);
        event.repeat = AKeyEvent_getRepeatCount(input);
    } else if (AInputEvent_getType(input) == AINPUT_EVENT_TYPE_MOTION) {
        int32_t action = AMotionEvent_getAction(input);
        event.action = action & AMOTION_EVENT_ACTION_MASK;
        event.pointerId = AMotionEvent_getPointerId(input, (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        event.count = static_cast<int32_t>(AMotionEvent_getPointerCount(input));

        for (int32_t i = 0; i < event.count && i < kFingers; i++) {
            event.x[i] = AMotionEvent_getX(input, i);
            event.y[i] = AMotionEvent_getY(input, i);
            event.ids[i] = AMotionEvent_getPointerId(input, i);
        }

        if ((source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE) {
            event.type = RawEvent::Mouse;
            event.buttons = AMotionEvent_getButtonState(input);
            event.scroll = event.action == AMOTION_EVENT_ACTION_SCROLL ? AMotionEvent_getAxisValue(input, AMOTION_EVENT_AXIS_VSCROLL, 0) : 0.0f;
        } else if ((source & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) {
            event.type = RawEvent::Touch;
        } else {
            return 0;
        }
    } else {
        return 0;
    }

    std::lock_guard<std::mutex> lock(s_rawLock);
    s_raw.push_back(event);

    return 1;
}

void AndroidInputSetSurfaceSize(int32_t width, int32_t height) {
    std::lock_guard<std::mutex> lock(s_rawLock);
    s_pendingWidth = width;
    s_pendingHeight = height;
    s_pendingSize = true;
}

void AndroidInputSetFocus(bool focus) {
    std::lock_guard<std::mutex> lock(s_rawLock);
    s_pendingFocus = focus ? 1 : 0;
}

void AndroidInputClose() {
    std::lock_guard<std::mutex> lock(s_rawLock);
    s_pendingClose = true;
}

void AndroidInputSetRenderSize(int32_t width, int32_t height) {
    if (width != s_contentWidth || height != s_contentHeight) {
        s_contentWidth = width;
        s_contentHeight = height;
        s_sizeChanged = true;
        s_mousePos.x = width / 2;
        s_mousePos.y = height / 2;
    }
}

namespace {

void SetControl(const char* name, float left, float bottom, float right, float top, bool joystick, int32_t key, uint32_t modifiers, uint32_t flags, int32_t holdKey, uint32_t holdModifiers) {
    Control control = { name ? name : "", left, bottom, right, top, joystick, key, modifiers, flags, holdKey, holdModifiers };

    for (Control& existing : s_controls) {
        if (existing.name == control.name) {
            existing = control;
            return;
        }
    }

    s_controls.push_back(control);
}

// The joystick's area keeps the system's back gesture off its edge of the screen, so a thumb landing near the edge
// moves rather than going back (the game's Escape). Sent when it changes.
void UpdateGestureExclusion() {
    static std::vector<int32_t> s_sent;
    std::vector<int32_t> rects;

    for (const Control& control : s_controls) {
        if (control.joystick && s_surfaceWidth && s_surfaceHeight) {
            rects.push_back(static_cast<int32_t>(control.left * s_surfaceWidth));
            rects.push_back(static_cast<int32_t>((1.0f - control.top) * s_surfaceHeight));
            rects.push_back(static_cast<int32_t>(control.right * s_surfaceWidth));
            rects.push_back(static_cast<int32_t>((1.0f - control.bottom) * s_surfaceHeight));
        }
    }

    if (rects != s_sent) {
        s_sent = rects;
        AndroidSetGestureExclusion(rects.data(), static_cast<int32_t>(rects.size() / 4));
    }
}

}

void AndroidInputSetKeyControl(const char* name, float left, float bottom, float right, float top, int32_t key, uint32_t modifiers, uint32_t flags, int32_t holdKey, uint32_t holdModifiers) {
    SetControl(name, left, bottom, right, top, false, key, modifiers, flags, holdKey, holdModifiers);
}

void AndroidInputSetJoystick(const char* name, float left, float bottom, float right, float top) {
    SetControl(name, left, bottom, right, top, true, KEY_NONE, 0, 0, KEY_NONE, 0);
    UpdateGestureExclusion();
}

void AndroidInputRemoveControl(const char* name) {
    for (auto control = s_controls.begin(); control != s_controls.end(); ++control) {
        if (control->name == (name ? name : "")) {
            bool joystick = control->joystick;
            s_controls.erase(control);

            if (joystick) {
                UpdateGestureExclusion();
            }

            return;
        }
    }
}

void AndroidInputClearControls() {
    // Fingers on controls keep their roles until they lift, so their keys come back up.
    s_controls.clear();
    UpdateGestureExclusion();
}

bool AndroidInputGetJoystick(float* originX, float* originY, float* x, float* y) {
    if (s_joystickFinger < 0 || !s_contentWidth || !s_contentHeight) {
        return false;
    }

    const Finger& finger = s_fingers[s_joystickFinger];
    *originX = static_cast<float>(finger.start.x) / static_cast<float>(s_contentWidth);
    *originY = 1.0f - static_cast<float>(finger.start.y) / static_cast<float>(s_contentHeight);
    JoystickPush(finger, x, y);
    return true;
}

bool AndroidInputControlPressed(const char* name) {
    for (auto pressed = s_pressedControls.begin(); pressed != s_pressedControls.end(); ++pressed) {
        if (*pressed == name) {
            s_pressedControls.erase(pressed);
            return true;
        }
    }

    return false;
}

bool AndroidInputControlHeld(const char* name) {
    for (const Finger& finger : s_fingers) {
        if (finger.role == Finger_Key && finger.control == name) {
            return true;
        }
    }

    return false;
}

void AndroidInputSetPointQuery(int32_t (*query)()) {
    s_pointQuery = query;
}

void AndroidInputSetWheelQuery(bool (*query)()) {
    s_wheelQuery = query;
}

void AndroidInputSetMultiLineQuery(bool (*query)()) {
    s_multiLineQuery = query;
}

void AndroidInputSetLatch(uint32_t modifiers) {
    // Changed while a finger holds it: that finger's modifiers go up first.
    if (s_latchFinger >= 0) {
        PutControlKey(KEY_NONE, s_latch, false);
        s_latchFinger = -1;
    }

    s_latch = modifiers;
}

uint32_t AndroidInputGetLatch() {
    return s_latch;
}

void AndroidInputSetUiAtPointQuery(bool (*query)(float x, float y)) {
    s_uiAtPointQuery = query;
}

void AndroidInputSetUiOverControlsQuery(bool (*query)(float x, float y)) {
    s_uiOverControlsQuery = query;
}

void AndroidInputSetCamera(float speed, bool invert) {
    s_cameraSpeed = speed > 0.0f ? speed : 1.0f;
    s_cameraInvert = invert;
}

void AndroidInputSetAutorunKey(int32_t key, uint32_t modifiers) {
    s_autorunKey = key;
    s_autorunModifiers = modifiers;
}

void AndroidInputSetBackgroundHandler(void (*handler)()) {
    s_backgroundHandler = handler;
}

void AndroidInputLowMemory() {
    s_lowMemory = true;
}

void AndroidInputSetLowMemoryHandler(void (*handler)()) {
    s_lowMemoryHandler = handler;
}

void AndroidInputSetTextFocus(bool focus) {
    // Asked again while the box has the focus (a click on it, CSimpleEditBox::OnLayerMouseDown): Android's keyboard
    // comes back if it was closed.
    if (focus && (!s_textFocus || s_keyboardState == Keyboard_Idle)) {
        s_textFocus = true;
        s_textFocusSinceMs = OsGetAsyncTimeMs();
    } else if (!focus) {
        s_textFocus = false;
        s_textFocusSinceMs = 0;
    }
}

bool AndroidInputCaptureCopy(const char* text) {
    if (s_keyboardState != Keyboard_Copying) {
        return false;
    }

    s_copyText = text ? text : "";
    s_copyCaptured = true;
    return true;
}

void OsQueuePut(OSINPUT id, int32_t param0, int32_t param1, int32_t param2, int32_t param3) {
    int32_t next = s_queueTail == kQueueSize - 1 ? 0 : s_queueTail + 1;

    if (next == s_queueHead) {
        s_queueHead = s_queueHead == kQueueSize - 1 ? 0 : s_queueHead + 1;
    }

    OSEVENT& event = s_queue[s_queueTail];
    event.id = id;
    event.param[0] = param0;
    event.param[1] = param1;
    event.param[2] = param2;
    event.param[3] = OsGetAsyncTimeMs();

    s_queueTail = next;
}

int32_t OsQueueGet(OSINPUT* id, int32_t* param0, int32_t* param1, int32_t* param2, int32_t* param3) {
    if (s_queueHead == s_queueTail) {
        return 0;
    }

    OSEVENT& event = s_queue[s_queueHead];
    *id = event.id;
    *param0 = event.param[0];
    *param1 = event.param[1];
    *param2 = event.param[2];
    *param3 = event.param[3];

    s_queueHead = s_queueHead == kQueueSize - 1 ? 0 : s_queueHead + 1;

    return 1;
}

void OsInputSetWindowResizeLock(int32_t resizeLock) {
}

float OsInputGetMouseSpeed() {
    return s_mouseSpeed * 0.1f;
}

int32_t OsInputIsKeyDown(KEY key) {
    return key >= 0 && key < KEY_LAST && s_keyDown[key];
}

int32_t OsInputIsMouseButtonDown(MOUSEBUTTON button) {
    return (s_osButtonState & button) != 0;
}

void OsInputInitialize() {
    s_mouseSpeed = 10;
}

void OsInputDestroy() {
    OsInputSetMouseMode(OS_MOUSE_MODE_NORMAL);
}

void OsInputSetEventPollProc(void (*pollProc)()) {
    s_eventPollProc = pollProc;
}

int32_t OsInputGet(OSINPUT* id, int32_t* param0, int32_t* param1, int32_t* param2, int32_t* param3) {
    *id = static_cast<OSINPUT>(-1);

    if (s_sizeChanged && s_contentWidth) {
        s_sizeChanged = false;

        if (s_contentWidth != s_defaultWindowRect.right || s_contentHeight != s_defaultWindowRect.bottom) {
            s_defaultWindowRect.left = 0;
            s_defaultWindowRect.top = 0;
            s_defaultWindowRect.right = s_contentWidth;
            s_defaultWindowRect.bottom = s_contentHeight;

            *id = OS_INPUT_SIZE;
            *param0 = s_contentWidth;
            *param1 = s_contentHeight;
            *param2 = 0;
            *param3 = 0;

            return 1;
        }
    }

    // The activity's events are read once per drain of the queue (once a frame): the game calls this until it
    // returns 0.
    if (s_queueHead == s_queueTail && s_needPoll) {
        s_needPoll = false;
        Poll();
    }

    if (!OsQueueGet(id, param0, param1, param2, param3)) {
        s_needPoll = true;
        return 0;
    }

    return 1;
}

void OsInputSetMouseMode(OS_MOUSE_MODE mode) {
    if (mode >= OS_MOUSE_MODES) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return;
    }

    if (mode == s_osMouseMode) {
        return;
    }

    if (mode == OS_MOUSE_MODE_RELATIVE) {
        s_osMouseMode = mode;
    } else {
        // The cursor comes back where relative mode started, as RestoreMouse does on Windows.
        s_osMouseMode = OS_MOUSE_MODE_NORMAL;
        OsQueuePut(OS_INPUT_MOUSE_MOVE, 0, s_mousePos.x, s_mousePos.y, 0);
    }
}

void OsInputSetMouseSpeed(float speed) {
    float clamped = speed < 0.1f ? 0.1f : (speed >= 2.0f ? 2.0f : speed);
    s_mouseSpeed = static_cast<int32_t>(clamped * 10.0f);
}

void OsInputGetMousePosition(int32_t* x, int32_t* y) {
    if (x) {
        *x = s_mousePos.x;
    }

    if (y) {
        *y = s_mousePos.y;
    }
}

void OsInputSetMousePosition(int32_t x, int32_t y) {
    s_mousePos.x = x;
    s_mousePos.y = y;
    ClampToContent(s_mousePos);
}

int32_t OsGetDefaultWindowRect(tagRECT* rect) {
    if (!rect) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (!s_defaultWindowRect.right || !s_defaultWindowRect.bottom) {
        if (!s_contentWidth) {
            return 0;
        }

        s_defaultWindowRect.left = 0;
        s_defaultWindowRect.top = 0;
        s_defaultWindowRect.right = s_contentWidth;
        s_defaultWindowRect.bottom = s_contentHeight;
    }

    *rect = s_defaultWindowRect;

    return 1;
}

intptr_t OsWindowProc(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam) {
    // No window messages on Android; the Gx device still takes this as its window procedure.
    return 0;
}

void OsInputEnableSecureMode(int32_t enable) {
    bool focus = enable != 0;

    if (focus && (!s_secureFocus || s_keyboardState == Keyboard_Idle)) {
        s_secureFocus = true;
        s_textFocusSinceMs = OsGetAsyncTimeMs();
    } else if (!focus) {
        s_secureFocus = false;
    }
}

int32_t AndroidIsModifierDown(int32_t modifier) {
    switch (modifier) {
        case 0:
            return s_keyDown[KEY_LCONTROL] || s_keyDown[KEY_RCONTROL];
        case 1:
            return s_keyDown[KEY_LSHIFT] || s_keyDown[KEY_RSHIFT];
        case 2:
            return s_keyDown[KEY_LALT] || s_keyDown[KEY_RALT];
        default:
            return 0;
    }
}

void OsMacInitializeApplication() {
}

void* OsMacCreateGameWindow(const char* title, int32_t x, int32_t y, int32_t width, int32_t height) {
    int32_t screenWidth;
    int32_t screenHeight;
    AndroidGetScreenSize(&screenWidth, &screenHeight);

    AndroidInputSetRenderSize(width > 0 ? width : screenWidth, height > 0 ? height : screenHeight);
    AndroidGetWindow(&s_windowGeneration);
    s_window = 1;

    return &s_window;
}

void* OsMacGetGameView(void* window) {
    return window;
}

float OsMacGetBackingScale(void* window) {
    return 1.0f;
}

float OsMacGetRenderScale(void* window) {
    return 1.0f;
}

void OsMacDestroyGameWindow(void* window) {
    if (!window) {
        return;
    }

    if (OsGuiGetWindow(0) == window) {
        OsInputSetMouseMode(OS_MOUSE_MODE_NORMAL);
        OsGuiSetGxWindow(nullptr);
    }

    s_window = 0;
    s_listener = nullptr;
}

void OsMacSetSystemCursorHidden(void* window, bool hidden) {
}

bool OsMacCursorHidden() {
    return !s_mouseInUse;
}

void OsMacSetGameCursor(void* window, const uint32_t* pixels, int32_t hotspotX, int32_t hotspotY) {
}

void OsMacGetContentSize(void* window, int32_t* width, int32_t* height) {
    *width = s_contentWidth;
    *height = s_contentHeight;
}

void OsMacGetContentScreenOrigin(void* window, int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}

void OsMacSetWindowFullscreen(void* window, bool fullscreen, int32_t width, int32_t height) {
    // The game fills the screen at the size asked for, which the display scales; the surface is made after this.
    if (width > 0 && height > 0) {
        AndroidInputSetRenderSize(width, height);
    }
}

void OsMacSetWindowListener(void* window, OsMacWindowListener listener, void* param) {
    s_listener = listener;
    s_listenerParam = param;
}
