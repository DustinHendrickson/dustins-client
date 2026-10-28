// The game's side of Android's touch controls (TouchControls.h): what the native input layer needs from the UI to
// route fingers, and the Lua functions of the TouchControls add-on (Tools/Android/assets/AddOns/TouchControls):
//
//   TouchControls_SetControl(name, left, bottom, right, top, key[, flags[, holdKey]])
//                                   an overlay control: a finger landing in the area (fractions of the screen from its
//                                   bottom left) holds key down, a binding's name for one ("SPACE", "TAB", "SHIFT-2",
//                                   "F13"), or is the joystick when key is "JOYSTICK". Setting a name again replaces it.
//   TouchControls_RemoveControl(name)
//   TouchControls_ClearControls()
//   held, originX, originY, x, y = TouchControls_GetJoystick()
//                                   while a finger holds the joystick: where it landed and how far it is pushed (-1 to
//                                   1, right and up positive)
//   held = TouchControls_IsHeld(name)    whether a finger is on that key control
//   pressed = TouchControls_WasPressed(name)   whether a finger pressed it since the last time this was asked
//   dpi, left, bottom, right, top, widthInches, heightInches = TouchControls_GetScreen()
//                                   the screen's density, the safe area's margins (the notch, the gesture bar) as
//                                   fractions of the screen, and its physical size
//   interacted = TouchControls_Interact()      loot, talk to or use the nearest thing in reach (Ui/Mac/PortActions)
//   TouchControls_TargetNonHostile([reverse])  the next friendly or neutral unit
//   TouchControls_SetLatch(modifier)           "SHIFT", "CTRL" or "ALT" for the next finger on the UI or world, or nil
//   modifier = TouchControls_GetLatch()        the latched modifier, nil once a finger has used it
//   TouchControls_SetFrameRate(fps)            60, or 30 to save the battery
//   TouchControls_SetCamera(speed, invert)     the camera drag's speed (1 by default) and up / down swapped
//   TouchControls_SetAutorunKey(key)           the key a double tap on the joystick presses (autorun's binding)
//   TouchControls_Log(text)                    a line in Logs/Console.txt
//   TouchControls_LogPlayer()                  the player's state that sitting and sheathing check, in the log

#include "WoW/Source/Ui/Android/TouchControls.h"
#include <cstdio>
#include "Engine/Source/Base/Coordinate.h"
#include <cstring>
#include "Engine/Source/Event/Types.h"
#include "Engine/Source/Frame/CSimpleEditBox.h"
#include "Engine/Source/Frame/CSimpleTop.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Gx/CGxDeviceOpenGL/Android/GlAndroidContext.h"
#include "Engine/Source/Os/Android/AndroidInput.h"
#include "Engine/Source/Os/Android/AndroidSystem.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/Mac/PortActions.h"
#include "WoW/Source/Ui/WorldFrame.h"

namespace {

// What the mouse is over, as the frame layer last found it (CSimpleTop::OnMouseMove): the 3D world is a frame of its
// own, WorldFrame; any other frame is UI.
int32_t PointUnderMouse() {
    CSimpleTop* top = CSimpleTop::GetInstance();
    CSimpleFrame* focus = top ? top->m_mouseFocus : nullptr;

    if (!focus) {
        return AndroidInputPoint_None;
    }

    if (CGWorldFrame::s_currentWorldFrame && focus == CGWorldFrame::s_currentWorldFrame) {
        return AndroidInputPoint_World;
    }

    return AndroidInputPoint_Ui;
}

// A frame that takes the mouse wheel (registered for it, with an OnMouseWheel script) and is visible.
bool TakesWheel(CSimpleFrame* frame) {
    return frame->m_isVisible && (frame->m_enabledEventsBitmask & (1 << SIMPLE_EVENT_MOUSEWHEEL)) && frame->m_onMouseWheel.m_luaRef;
}

// Whether the mouse wheel would scroll something under the mouse: the frame under it or one it is in (a list's button
// in its scroll frame), so a window over a list keeps its own drag. Frames that don't take the mouse themselves (the
// chat's lines) are found as CSimpleTop::OnMouseWheel finds them, when nothing that takes the mouse is there. The 3D
// world isn't one: the wheel zooms the camera there.
bool WheelUnderMouse() {
    CSimpleTop* top = CSimpleTop::GetInstance();

    if (!top) {
        return false;
    }

    CSimpleFrame* world = CGWorldFrame::s_currentWorldFrame;

    if (top->m_mouseFocus && top->m_mouseFocus != world) {
        for (CSimpleFrame* frame = top->m_mouseFocus; frame; frame = frame->m_parent) {
            if (TakesWheel(frame)) {
                return true;
            }
        }

        return false;
    }

    C2Vector pt;
    NDCToDDC(top->m_mousePosition.m_x, top->m_mousePosition.m_y, &pt.x, &pt.y);

    for (int32_t strata = FRAME_STRATA_TOOLTIP; strata >= 0; strata--) {
        CSimpleSortedArray<FRAMEPRIORITY*>& queue = top->m_eventqueue[SIMPLE_EVENT_MOUSEWHEEL][strata];

        for (uint32_t i = 0; i < queue.m_count; i++) {
            CSimpleFrame* frame = queue.m_array[i] ? queue.m_array[i]->m_frame : nullptr;

            if (frame && frame != world && TakesWheel(frame) && frame->TestHitRect(pt)) {
                return true;
            }
        }
    }

    return false;
}

// Whether a frame that takes the mouse is at a point (fractions of the screen), as the frame layer would find it under
// the mouse: the 3D world doesn't count.
bool UiAtPoint(float x, float y) {
    CSimpleTop* top = CSimpleTop::GetInstance();

    if (!top) {
        return false;
    }

    C2Vector pt;
    NDCToDDC(x, y, &pt.x, &pt.y);
    CSimpleFrame* world = CGWorldFrame::s_currentWorldFrame;

    for (int32_t strata = FRAME_STRATA_TOOLTIP; strata >= 0; strata--) {
        CSimpleSortedArray<FRAMEPRIORITY*>& queue = top->m_eventqueue[SIMPLE_EVENT_MOUSE][strata];

        for (uint32_t i = 0; i < queue.m_count; i++) {
            CSimpleFrame* frame = queue.m_array[i] ? queue.m_array[i]->m_frame : nullptr;

            if (frame && frame != world && frame->m_isVisible && frame->TestHitRect(pt)) {
                return true;
            }
        }
    }

    return false;
}

// Whether a frame that takes the mouse is drawn over the touch controls at a point (fractions of the screen): one in a
// stratum above theirs (the add-on's buttons are LOW), as a window from the game or an add-on is.
bool UiOverControls(float x, float y) {
    CSimpleTop* top = CSimpleTop::GetInstance();

    if (!top) {
        return false;
    }

    C2Vector pt;
    NDCToDDC(x, y, &pt.x, &pt.y);
    CSimpleFrame* world = CGWorldFrame::s_currentWorldFrame;

    for (int32_t strata = FRAME_STRATA_TOOLTIP; strata > FRAME_STRATA_LOW; strata--) {
        CSimpleSortedArray<FRAMEPRIORITY*>& queue = top->m_eventqueue[SIMPLE_EVENT_MOUSE][strata];

        for (uint32_t i = 0; i < queue.m_count; i++) {
            CSimpleFrame* frame = queue.m_array[i] ? queue.m_array[i]->m_frame : nullptr;

            if (frame && frame != world && frame->m_isVisible && frame->TestHitRect(pt)) {
                return true;
            }
        }
    }

    return false;
}

// Whether the focused text box has several lines (CSimpleEditBox's multi-line flag).
bool FocusIsMultiLine() {
    CSimpleEditBox* box = CSimpleEditBox::s_currentFocus;
    return box && (box->m_editBoxFlags & 0x2);
}

// A key by its binding name, as the game's key bindings write them.
int32_t KeyFromName(const char* name) {
    static const struct {
        const char* name;
        KEY key;
    } s_named[] = {
        { "SPACE", KEY_SPACE }, { "TAB", KEY_TAB }, { "ESCAPE", KEY_ESCAPE }, { "ENTER", KEY_ENTER },
        { "BACKSPACE", KEY_BACKSPACE }, { "DELETE", KEY_DELETE }, { "INSERT", KEY_INSERT }, { "HOME", KEY_HOME },
        { "END", KEY_END }, { "PAGEUP", KEY_PAGEUP }, { "PAGEDOWN", KEY_PAGEDOWN }, { "UP", KEY_UP },
        { "DOWN", KEY_DOWN }, { "LEFT", KEY_LEFT }, { "RIGHT", KEY_RIGHT }, { "NUMLOCK", KEY_NUMLOCK },
        { "-", KEY_MINUS }, { "=", KEY_PLUS }, { "`", KEY_TILDE }, { "[", KEY_BRACKET_OPEN },
        { "]", KEY_BRACKET_CLOSE }, { "/", KEY_SLASH }, { "\\", KEY_BACKSLASH }, { ";", KEY_SEMICOLON },
        { "'", KEY_APOSTROPHE }, { ",", KEY_COMMA }, { ".", KEY_PERIOD }, { "PRINTSCREEN", KEY_PRINTSCREEN },
        { "PAUSE", KEY_PAUSE }, { "SCROLLLOCK", KEY_SCROLLLOCK }, { "CAPSLOCK", KEY_CAPSLOCK },
        { "NUMPADPLUS", KEY_NUMPAD_PLUS }, { "NUMPADMINUS", KEY_NUMPAD_MINUS }, { "NUMPADMULTIPLY", KEY_NUMPAD_MULTIPLY },
        { "NUMPADDIVIDE", KEY_NUMPAD_DIVIDE }, { "NUMPADDECIMAL", KEY_NUMPAD_DECIMAL }, { "NUMPADEQUALS", KEY_NUMPAD_EQUALS },
    };

    for (const auto& named : s_named) {
        if (!strcmp(named.name, name)) {
            return named.key;
        }
    }

    if (!strncmp(name, "NUMPAD", 6) && name[6] >= '0' && name[6] <= '9' && !name[7]) {
        return KEY_NUMPAD0 + (name[6] - '0');
    }

    if (name[0] && !name[1]) {
        if (name[0] >= 'A' && name[0] <= 'Z') {
            return KEY_A + (name[0] - 'A');
        }

        if (name[0] >= '0' && name[0] <= '9') {
            return KEY_0 + (name[0] - '0');
        }
    }

    // F1 to F12 are consecutive; F13 sits apart in the game's numbering, F14 to F19 follow F12.
    if (name[0] == 'F' && name[1]) {
        int32_t number = 0;

        if (sscanf(name + 1, "%d", &number) == 1) {
            if (number >= 1 && number <= 12) {
                return KEY_F1 + (number - 1);
            }

            if (number == 13) {
                return KEY_F13;
            }

            if (number >= 14 && number <= 19) {
                return KEY_F14 + (number - 14);
            }
        }
    }

    return KEY_NONE;
}

// "SHIFT-CTRL-2" and the like: the modifiers, and the key after them.
int32_t ParseKey(const char* text, uint32_t* modifiers) {
    *modifiers = 0;

    for (;;) {
        if (!strncmp(text, "SHIFT-", 6)) {
            *modifiers |= AndroidInputModifier_Shift;
            text += 6;
        } else if (!strncmp(text, "CTRL-", 5)) {
            *modifiers |= AndroidInputModifier_Ctrl;
            text += 5;
        } else if (!strncmp(text, "ALT-", 4)) {
            *modifiers |= AndroidInputModifier_Alt;
            text += 4;
        } else {
            break;
        }
    }

    return KeyFromName(text);
}

int32_t Script_TouchControls_SetControl(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    const char* key = lua_tostring(L, 6);

    if (!name || !key) {
        return 0;
    }

    float left = static_cast<float>(lua_tonumber(L, 2));
    float bottom = static_cast<float>(lua_tonumber(L, 3));
    float right = static_cast<float>(lua_tonumber(L, 4));
    float top = static_cast<float>(lua_tonumber(L, 5));

    if (!strcmp(key, "JOYSTICK")) {
        AndroidInputSetJoystick(name, left, bottom, right, top);
        return 0;
    }

    // Flags: "round" (the circle in the area), "lift" (pressed on the finger's lift), "top" (on a window of its own,
    // which the UI under it doesn't cover).
    const char* options = lua_tostring(L, 7);
    uint32_t flags = 0;

    if (options && strstr(options, "round")) {
        flags |= AndroidInputControl_Round;
    }

    if (options && strstr(options, "lift")) {
        flags |= AndroidInputControl_OnLift;
    }

    if (options && strstr(options, "top")) {
        flags |= AndroidInputControl_OnTop;
    }

    // A key pressed instead when the finger stays still on the control a moment (the "lift" kind).
    const char* hold = lua_tostring(L, 8);
    uint32_t holdModifiers = 0;
    int32_t holdCode = hold ? ParseKey(hold, &holdModifiers) : KEY_NONE;

    uint32_t modifiers;
    int32_t code = ParseKey(key, &modifiers);

    if (code != KEY_NONE) {
        AndroidInputSetKeyControl(name, left, bottom, right, top, code, modifiers, flags, holdCode, holdModifiers);
    } else {
        AndroidInputRemoveControl(name);
    }

    return 0;
}

int32_t Script_TouchControls_RemoveControl(lua_State* L) {
    if (const char* name = lua_tostring(L, 1)) {
        AndroidInputRemoveControl(name);
    }

    return 0;
}

int32_t Script_TouchControls_ClearControls(lua_State* L) {
    AndroidInputClearControls();
    return 0;
}

int32_t Script_TouchControls_GetJoystick(lua_State* L) {
    float originX;
    float originY;
    float x;
    float y;

    if (!AndroidInputGetJoystick(&originX, &originY, &x, &y)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_pushboolean(L, 1);
    lua_pushnumber(L, originX);
    lua_pushnumber(L, originY);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 5;
}

int32_t Script_TouchControls_IsHeld(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    lua_pushboolean(L, name && AndroidInputControlHeld(name));
    return 1;
}

int32_t Script_TouchControls_WasPressed(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    lua_pushboolean(L, name && AndroidInputControlPressed(name));
    return 1;
}

int32_t Script_TouchControls_GetScreen(lua_State* L) {
    int32_t width;
    int32_t height;
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
    AndroidGetScreenSize(&width, &height);
    AndroidGetSafeInsets(&left, &top, &right, &bottom);

    lua_pushnumber(L, AndroidGetDensityDpi());
    lua_pushnumber(L, width ? static_cast<float>(left) / width : 0.0f);
    lua_pushnumber(L, height ? static_cast<float>(bottom) / height : 0.0f);
    lua_pushnumber(L, width ? static_cast<float>(right) / width : 0.0f);
    lua_pushnumber(L, height ? static_cast<float>(top) / height : 0.0f);

    float dpi = AndroidGetDensityDpi();
    lua_pushnumber(L, dpi > 0.0f ? width / dpi : 0.0f);
    lua_pushnumber(L, dpi > 0.0f ? height / dpi : 0.0f);
    return 7;
}

int32_t Script_TouchControls_Interact(lua_State* L) {
    lua_pushboolean(L, PortInteractNearest());
    return 1;
}

int32_t Script_TouchControls_TargetNonHostile(lua_State* L) {
    PortTargetNonHostile(lua_toboolean(L, 1) != 0);
    return 0;
}

const char* const s_latchNames[] = { "SHIFT", "CTRL", "ALT" };

int32_t Script_TouchControls_SetLatch(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    uint32_t modifiers = 0;

    for (uint32_t i = 0; name && i < 3; i++) {
        if (!strcmp(name, s_latchNames[i])) {
            modifiers = 1u << i;
        }
    }

    AndroidInputSetLatch(modifiers);
    return 0;
}

int32_t Script_TouchControls_GetLatch(lua_State* L) {
    uint32_t modifiers = AndroidInputGetLatch();

    for (uint32_t i = 0; i < 3; i++) {
        if (modifiers & (1u << i)) {
            lua_pushstring(L, s_latchNames[i]);
            return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

int32_t Script_TouchControls_SetFrameRate(lua_State* L) {
    AndroidGlSetFrameRate(static_cast<int32_t>(lua_tonumber(L, 1)));
    return 0;
}

int32_t Script_TouchControls_SetCamera(lua_State* L) {
    AndroidInputSetCamera(static_cast<float>(lua_tonumber(L, 1)), lua_toboolean(L, 2) != 0);
    return 0;
}

int32_t Script_TouchControls_SetAutorunKey(lua_State* L) {
    const char* key = lua_tostring(L, 1);
    uint32_t modifiers = 0;
    int32_t code = key ? ParseKey(key, &modifiers) : KEY_NONE;
    AndroidInputSetAutorunKey(code, modifiers);
    return 0;
}

int32_t Script_TouchControls_LogPlayer(lua_State* L) {
    CGPlayer_C* player = ClntObjMgrGetActivePlayerObj();

    if (!player) {
        fprintf(stderr, "TouchControls: no player\n");
        return 0;
    }

    fprintf(stderr, "TouchControls: player stand %u (field %u) sheathe %u mount %d clientFlags 0x%x animState 0x%x cast %d channel %u attacking %d local %d unitFlags 0x%x moveFlags 0x%x\n",
        player->GetClientStandState(), player->m_unit->m_unitFieldBytes1[0], player->m_sheatheState, player->m_mountDisplayId,
        player->m_clientFlags, player->m_animationState, player->m_castSpellId, player->m_unit->m_unitChannelSpell,
        player->m_combat.m_attacking ? 1 : 0, player->IsLocalClientControlled() ? 1 : 0, player->m_unit->m_unitFieldFlags,
        player->m_movement->m_flags);
    return 0;
}

int32_t Script_TouchControls_Log(lua_State* L) {
    if (const char* text = lua_tostring(L, 1)) {
        fprintf(stderr, "TouchControls: %s\n", text);
    }

    return 0;
}

const struct {
    const char* m_name;
    lua_CFunction m_method;
} s_functions[] = {
    { "TouchControls_SetControl", &Script_TouchControls_SetControl },
    { "TouchControls_RemoveControl", &Script_TouchControls_RemoveControl },
    { "TouchControls_ClearControls", &Script_TouchControls_ClearControls },
    { "TouchControls_GetJoystick", &Script_TouchControls_GetJoystick },
    { "TouchControls_IsHeld", &Script_TouchControls_IsHeld },
    { "TouchControls_WasPressed", &Script_TouchControls_WasPressed },
    { "TouchControls_GetScreen", &Script_TouchControls_GetScreen },
    { "TouchControls_Interact", &Script_TouchControls_Interact },
    { "TouchControls_TargetNonHostile", &Script_TouchControls_TargetNonHostile },
    { "TouchControls_SetFrameRate", &Script_TouchControls_SetFrameRate },
    { "TouchControls_SetLatch", &Script_TouchControls_SetLatch },
    { "TouchControls_GetLatch", &Script_TouchControls_GetLatch },
    { "TouchControls_SetCamera", &Script_TouchControls_SetCamera },
    { "TouchControls_SetAutorunKey", &Script_TouchControls_SetAutorunKey },
    { "TouchControls_Log", &Script_TouchControls_Log },
    { "TouchControls_LogPlayer", &Script_TouchControls_LogPlayer },
};

}

void TouchControlsInitialize() {
    AndroidInputSetPointQuery(PointUnderMouse);
    AndroidInputSetWheelQuery(WheelUnderMouse);
    AndroidInputSetUiAtPointQuery(UiAtPoint);
    AndroidInputSetUiOverControlsQuery(UiOverControls);
    AndroidInputSetMultiLineQuery(FocusIsMultiLine);
}

void TouchControlsRegisterScriptFunctions() {
    for (const auto& function : s_functions) {
        FrameScript_RegisterFunction(function.m_name, function.m_method);
    }
}

void TouchControlsUnregisterScriptFunctions() {
    // The add-on goes with the UI (logging out, /reload): its controls go too.
    AndroidInputClearControls();

    for (const auto& function : s_functions) {
        FrameScript_UnregisterFunction(function.m_name);
    }
}
