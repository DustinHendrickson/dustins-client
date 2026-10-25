#ifndef WOW_SOURCE_UI_ANDROID_TOUCHCONTROLS_H
#define WOW_SOURCE_UI_ANDROID_TOUCHCONTROLS_H

// The game's side of Android's touch controls (docs/ANDROID_PORT.md, "Touch and small screens"): what the native input
// layer (Engine/Source/Os/Android/Input.cpp) needs to know from the UI to route fingers, and the TouchControls
// add-on's Lua functions (listed in TouchControls.cpp).

// Called once at start (WoW/Source/Android/main.cpp).
void TouchControlsInitialize();

// The TouchControls add-on's Lua functions, with the game UI's (GameUI_Scripts.cpp).
void TouchControlsRegisterScriptFunctions();
void TouchControlsUnregisterScriptFunctions();

#endif
