// Android implementation of the Os input layer (Os/Win64/Input.cpp): the game's event queue, filled from the
// activity's input events.
//
// The activity's thread (WoW/Source/Android/main.cpp) copies each touch, key and mouse event into a locked list
// (AndroidInputHandleEvent); the game thread drains it once a frame, when the queue runs dry (OsInputGet), and turns it
// into the keyboard and mouse events the game knows. The game sees a keyboard and one mouse:
//
// - Touch: a finger is the mouse. A tap is a left click where it lands, a drag a left drag, and a touch held still for
//   half a second a right click. (The hybrid controls of docs/ANDROID_PORT.md, joystick and camera, build on this.)
// - Keyboards (USB, Bluetooth) send their keys, and characters as typed on a US layout; Back is Escape.
// - Mice move the pointer, click and scroll.
//
// A text box that keeps the keyboard focus a moment opens Android's keyboard over a copy of its text
// (WowActivity.editText); what is entered replaces the box's text.

#include <android/input.h>
#include <android/keycodes.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
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
const uint64_t kTouchHoldMs = 500;

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
    float x[4];
    float y[4];
    int32_t ids[4];
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
bool s_keyDown[KEY_LAST];
bool s_needPoll = true;

// The window, at the render size the device chose (AndroidInputSetRenderSize), and the surface it is shown on: touch
// coordinates come in surface pixels.
int32_t s_contentWidth;
int32_t s_contentHeight;
int32_t s_surfaceWidth;
int32_t s_surfaceHeight;
bool s_sizeChanged;

// Touch: the finger acting as the mouse.
enum TouchState {
    Touch_None,
    Touch_Pending,      // down, not yet a tap, drag or hold
    Touch_Dragging,
    Touch_Done,         // held (right-clicked); ignored until it lifts
};

TouchState s_touchState;
int32_t s_touchId = -1;
C2iVector s_touchStart;
uint64_t s_touchStartMs;

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

void PutKey(KEY key, bool down) {
    if (key >= 0 && key < KEY_LAST) {
        if (s_keyDown[key] == down) {
            return;
        }

        s_keyDown[key] = down;
    }

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

    OsQueuePut(down ? OS_INPUT_MOUSE_DOWN : OS_INPUT_MOUSE_UP, button, s_mousePos.x, s_mousePos.y, 0);
}

void PutWheel(int32_t notches) {
    if (notches) {
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

// Touch: the finger acting as the mouse (the first one down).
void HandleTouch(const RawEvent& event, uint64_t now) {
    int32_t index = -1;

    for (int32_t i = 0; i < event.count && i < 4; i++) {
        if (event.ids[i] == s_touchId) {
            index = i;
        }
    }

    switch (event.action) {
        case AMOTION_EVENT_ACTION_DOWN:
            s_touchId = event.ids[0];
            s_touchStart = ToContent(event.x[0], event.y[0]);
            s_touchStartMs = now;
            s_touchState = Touch_Pending;
            MoveMouseTo(s_touchStart);
            break;

        case AMOTION_EVENT_ACTION_MOVE: {
            if (index < 0) {
                break;
            }

            C2iVector point = ToContent(event.x[index], event.y[index]);

            if (s_touchState == Touch_Pending && (std::abs(point.x - s_touchStart.x) > kTouchSlop || std::abs(point.y - s_touchStart.y) > kTouchSlop)) {
                PutMouseButton(MOUSE_BUTTON_LEFT, true);
                s_touchState = Touch_Dragging;
            }

            if (s_touchState == Touch_Dragging) {
                MoveMouseTo(point);
            }

            break;
        }

        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            if (event.action == AMOTION_EVENT_ACTION_POINTER_UP && event.pointerId != s_touchId) {
                break;
            }

            if (s_touchState == Touch_Pending && event.action != AMOTION_EVENT_ACTION_CANCEL) {
                PutMouseButton(MOUSE_BUTTON_LEFT, true);
                PutMouseButton(MOUSE_BUTTON_LEFT, false);
            } else if (s_touchState == Touch_Dragging) {
                PutMouseButton(MOUSE_BUTTON_LEFT, false);
            }

            s_touchState = Touch_None;
            s_touchId = -1;
            break;

        default:
            break;
    }
}

// A finger held still long enough right-clicks where it is.
void PollTouchHold(uint64_t now) {
    if (s_touchState == Touch_Pending && now - s_touchStartMs >= kTouchHoldMs) {
        PutMouseButton(MOUSE_BUTTON_RIGHT, true);
        PutMouseButton(MOUSE_BUTTON_RIGHT, false);
        s_touchState = Touch_Done;
    }
}

void HandleMouse(const RawEvent& event) {
    MoveMouseTo(ToContent(event.x[0], event.y[0]));

    PutMouseButton(MOUSE_BUTTON_LEFT, (event.buttons & AMOTION_EVENT_BUTTON_PRIMARY) != 0);
    PutMouseButton(MOUSE_BUTTON_RIGHT, (event.buttons & AMOTION_EVENT_BUTTON_SECONDARY) != 0);
    PutMouseButton(MOUSE_BUTTON_MIDDLE, (event.buttons & AMOTION_EVENT_BUTTON_TERTIARY) != 0);

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
            }

            break;
        }
    }
}

void Poll() {
    uint64_t now = OsGetAsyncTimeMs();
    std::vector<RawEvent> events;

    {
        std::lock_guard<std::mutex> lock(s_rawLock);
        events.swap(s_raw);

        if (s_pendingSize) {
            s_pendingSize = false;
            s_surfaceWidth = s_pendingWidth;
            s_surfaceHeight = s_pendingHeight;
        }

        if (s_pendingFocus >= 0) {
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
                HandleTouch(event, now);
                break;
            case RawEvent::Key:
                HandleKey(event);
                break;
            case RawEvent::Mouse:
                HandleMouse(event);
                break;
        }
    }

    // The app went to the background (no window) or came back: the device stops drawing meanwhile.
    uint32_t generation;
    bool window = AndroidGetWindow(&generation) != nullptr;

    if (generation != s_windowGeneration) {
        s_windowGeneration = generation;
        Notify(window ? OsMacWindowEvent_Restored : OsMacWindowEvent_Minimized);
    }

    PollTouchHold(now);
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

        for (int32_t i = 0; i < event.count && i < 4; i++) {
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
