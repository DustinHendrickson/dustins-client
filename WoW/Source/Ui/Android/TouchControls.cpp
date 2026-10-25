// The game's side of Android's touch controls (TouchControls.h): what the native input layer needs from the UI to
// route fingers, and the Lua functions of the TouchControls add-on (Tools/Android/assets/AddOns/TouchControls):
//
//   TouchControls_SetControl(name, left, bottom, right, top, key)
//                                   an overlay control: a finger landing in the area (fractions of the screen from its
//                                   bottom left) holds key down, a binding's name for one ("SPACE", "TAB", "SHIFT-2",
//                                   "F13"), or is the joystick when key is "JOYSTICK". Setting a name again replaces it.
//   TouchControls_RemoveControl(name)
//   TouchControls_ClearControls()
//   held, originX, originY, x, y = TouchControls_GetJoystick()
//                                   while a finger holds the joystick: where it landed and how far it is pushed (-1 to
//                                   1, right and up positive)
//   held = TouchControls_IsHeld(name)    whether a finger is on that key control
//   dpi, left, bottom, right, top = TouchControls_GetScreen()
//                                   the screen's density, and the safe area's margins (the notch, the gesture bar) as
//                                   fractions of the screen
//   interacted = TouchControls_Interact()      loot, talk to or use the nearest thing in reach (Ui/Mac/PortActions)
//   TouchControls_TargetNonHostile([reverse])  the next friendly or neutral unit
//   TouchControls_Log(text)                    a line in Logs/Console.txt

#include "WoW/Source/Ui/Android/TouchControls.h"
#include <cstdio>
#include <cstring>
#include "Engine/Source/Event/Types.h"
#include "Engine/Source/Frame/CSimpleTop.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Os/Android/AndroidInput.h"
#include "Engine/Source/Os/Android/AndroidSystem.h"
#include "Engine/Source/lua-5.1/src/lua.h"
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
        { "'", KEY_APOSTROPHE }, { ",", KEY_COMMA }, { ".", KEY_PERIOD },
    };

    for (const auto& named : s_named) {
        if (!strcmp(named.name, name)) {
            return named.key;
        }
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

    uint32_t modifiers;
    int32_t code = ParseKey(key, &modifiers);

    if (code != KEY_NONE) {
        AndroidInputSetKeyControl(name, left, bottom, right, top, code, modifiers);
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
    return 5;
}

int32_t Script_TouchControls_Interact(lua_State* L) {
    lua_pushboolean(L, PortInteractNearest());
    return 1;
}

int32_t Script_TouchControls_TargetNonHostile(lua_State* L) {
    PortTargetNonHostile(lua_toboolean(L, 1) != 0);
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
    { "TouchControls_GetScreen", &Script_TouchControls_GetScreen },
    { "TouchControls_Interact", &Script_TouchControls_Interact },
    { "TouchControls_TargetNonHostile", &Script_TouchControls_TargetNonHostile },
    { "TouchControls_Log", &Script_TouchControls_Log },
};

}

void TouchControlsInitialize() {
    AndroidInputSetPointQuery(PointUnderMouse);
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
