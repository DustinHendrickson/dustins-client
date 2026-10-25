#include <cstdint>
#include <cstdio>
#include <cstring>
#include "WoW/Source/Ui/Switch/SwitchControlsScript.h"
#include "WoW/Source/Ui/Mac/PortActions.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Os/Switch/SwitchInput.h"
#include "Engine/Source/lua-5.1/src/lua.h"

namespace {

int32_t Script_SwitchControls_GetMode(lua_State* L) {
    SwitchControllerState state = SwitchGetControllerState();
    lua_pushstring(L, state.cursorMode ? "cursor" : "play");
    lua_pushboolean(L, state.textFocus);
    lua_pushboolean(L, state.pointerActive);
    return 3;
}

int32_t Script_SwitchControls_SetMode(lua_State* L) {
    const char* mode = lua_tostring(L, 1);

    if (mode && (!strcmp(mode, "cursor") || !strcmp(mode, "play"))) {
        SwitchSetCursorMode(!strcmp(mode, "cursor"));
    }

    return 0;
}

// A line in Logs/Console.txt, for diagnosing the add-on on the console.
int32_t Script_SwitchControls_Log(lua_State* L) {
    const char* text = lua_tostring(L, 1);

    if (text) {
        fprintf(stderr, "SwitchControls: %s\n", text);
    }

    return 0;
}

int32_t Script_SwitchControls_SetOption(lua_State* L) {
    const char* name = lua_tostring(L, 1);

    if (name) {
        SwitchSetOption(name, lua_toboolean(L, 2) != 0);
    }

    return 0;
}

int32_t Script_SwitchControls_GetOption(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    bool value;

    if (!name || !SwitchGetOption(name, &value)) {
        lua_pushnil(L);
    } else {
        lua_pushboolean(L, value);
    }

    return 1;
}

int32_t Script_SwitchControls_DisplayChanged(lua_State* L) {
    lua_pushboolean(L, SwitchDisplayChanged());
    return 1;
}

int32_t Script_SwitchControls_SetPointer(lua_State* L) {
    SwitchSetPointer(static_cast<float>(lua_tonumber(L, 1)), static_cast<float>(lua_tonumber(L, 2)));
    return 0;
}

int32_t Script_SwitchControls_ClearTextFocus(lua_State* L) {
    SwitchClearTextFocus();
    return 0;
}

int32_t Script_SwitchControls_SetKeyCapture(lua_State* L) {
    SwitchSetKeyCapture(lua_toboolean(L, 1) != 0);
    return 0;
}

// interacted = SwitchControls_Interact(): what the controller's interact button does. The add-on's button calls it from a
// key binding; the game's own InteractUnit refuses add-on code, so this runs the right-click as if from the game.
int32_t Script_SwitchControls_Interact(lua_State* L) {
    lua_pushboolean(L, PortInteractNearest());
    return 1;
}

// SwitchControls_TargetNonHostile([reverse]): the next living friendly or neutral unit, cycling as Tab does through
// enemies (the game's own nearest-target search, with the Switch's target type 7). Add-on code may not target, so this
// runs as the game.
int32_t Script_SwitchControls_TargetNonHostile(lua_State* L) {
    PortTargetNonHostile(lua_toboolean(L, 1) != 0);
    return 0;
}

int32_t Script_SwitchControls_IsDocked(lua_State* L) {
    lua_pushboolean(L, SwitchIsDocked());
    return 1;
}

const struct {
    const char* m_name;
    lua_CFunction m_method;
} s_functions[] = {
    { "SwitchControls_GetMode", &Script_SwitchControls_GetMode },
    { "SwitchControls_SetMode", &Script_SwitchControls_SetMode },
    { "SwitchControls_IsDocked", &Script_SwitchControls_IsDocked },
    { "SwitchControls_Log", &Script_SwitchControls_Log },
    { "SwitchControls_GetOption", &Script_SwitchControls_GetOption },
    { "SwitchControls_SetOption", &Script_SwitchControls_SetOption },
    { "SwitchControls_SetKeyCapture", &Script_SwitchControls_SetKeyCapture },
    { "SwitchControls_ClearTextFocus", &Script_SwitchControls_ClearTextFocus },
    { "SwitchControls_SetPointer", &Script_SwitchControls_SetPointer },
    { "SwitchControls_DisplayChanged", &Script_SwitchControls_DisplayChanged },
    { "SwitchControls_Interact", &Script_SwitchControls_Interact },
    { "SwitchControls_TargetNonHostile", &Script_SwitchControls_TargetNonHostile },
};

// The login screens' video options (GlueXML, where add-ons don't load) get what the SwitchControls add-on does to the
// world's: the controls the Switch has no use for (windowed and maximized, the window lock, triple buffering, the
// hardware cursor, input lag, gamma) are greyed out and stay so when the panel enables others.
const char kGlueVideoOptionsScript[] = R"lua(
local panel = "VideoOptionsResolutionPanel"
local disable = BlizzardOptionsPanel_CheckButton_Disable or function(box) box:Disable() end
local unused = {}

for _, name in ipairs({ "TripleBuffer", "HardwareCursor", "FixInputLag", "Windowed", "Maximized", "DisableResize", "DesktopGamma" }) do
    local box = _G[panel .. name]

    if box then
        unused[box] = true
        disable(box)
        box.Enable = function() end
    end
end

if BlizzardOptionsPanel_CheckButton_Enable then
    hooksecurefunc("BlizzardOptionsPanel_CheckButton_Enable", function(box)
        if unused[box] then
            disable(box)
        end
    end)
end

local function DisableGamma()
    local slider = _G[panel .. "GammaSlider"]

    if slider and BlizzardOptionsPanel_Slider_Disable then
        BlizzardOptionsPanel_Slider_Disable(slider)
    end
end

if VideoOptionsResolutionPanel_RefreshGammaControls then
    hooksecurefunc("VideoOptionsResolutionPanel_RefreshGammaControls", DisableGamma)
end

DisableGamma()
)lua";

}

void SwitchControlsSetUpGlue() {
    FrameScript_Execute(kGlueVideoOptionsScript, "SwitchGlueVideoOptions", nullptr);
}

void SwitchControlsRegisterScriptFunctions() {
    for (const auto& function : s_functions) {
        FrameScript_RegisterFunction(function.m_name, function.m_method);
    }
}

void SwitchControlsUnregisterScriptFunctions() {
    for (const auto& function : s_functions) {
        FrameScript_UnregisterFunction(function.m_name);
    }
}
