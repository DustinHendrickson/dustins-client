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

// OsIMEEnable: a text box gained or lost the keyboard focus.
void AndroidInputSetTextFocus(bool focus);

// OsClipboard: the game copying text while the keyboard reads a box's text; true if it was taken for that.
bool AndroidInputCaptureCopy(const char* text);

// 0 Control, 1 Shift, 2 Alt, as OsGuiIsModifierKeyDown takes them.
int32_t AndroidIsModifierDown(int32_t modifier);

#endif
