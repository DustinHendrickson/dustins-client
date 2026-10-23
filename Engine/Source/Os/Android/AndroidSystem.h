#ifndef ENGINE_SOURCE_OS_ANDROID_ANDROIDSYSTEM_H
#define ENGINE_SOURCE_OS_ANDROID_ANDROIDSYSTEM_H

// Android services for the game: the activity (Tools/Android/java/.../WowActivity.java) through JNI, its window, and
// its events (Os/Android/AndroidActivity.cpp, Input.cpp). WoW/Source/Android/main.cpp sets them up.

#include <cstdint>
#include <string>

struct ANativeActivity;
struct ANativeWindow;

// main.cpp: the activity, once it exists, and the window, as it comes and goes.
void AndroidSetActivity(ANativeActivity* activity);
void AndroidSetWindow(ANativeWindow* window);

// The window to draw in, waiting for it (the device is created after the activity has one). Null if the app is
// finishing.
ANativeWindow* AndroidWaitForWindow();

// The GLES device: the window now (null while the app is in the background), and a count that changes with it. Once
// the device draws to a window, its going away waits (up to two seconds) until the device has released it: called
// with the count it last saw, after destroying its surface.
ANativeWindow* AndroidGetWindow(uint32_t* generation);
void AndroidReleaseWindow(uint32_t generation);
void AndroidSetWindowInUse(bool inUse);

// The app is closing: wakes anything waiting for a window or a dialog.
void AndroidSetFinishing();
bool AndroidIsFinishing();

// Opens a URL or file in the app Android picks for it (a browser for http links). False if nothing could.
bool AndroidOpenURL(const char* url);

// The screen in pixels, landscape (the activity's window once it exists, the display's metrics before).
void AndroidGetScreenSize(int32_t* width, int32_t* height);

// The screen's density in dots per inch, and the safe area's margins in pixels (the notch, the home gesture bar).
float AndroidGetDensityDpi();
void AndroidGetSafeInsets(int32_t* left, int32_t* top, int32_t* right, int32_t* bottom);

// All files access, for the game folder on shared storage, and the page that grants it.
bool AndroidHasStorageAccess();
void AndroidRequestStorageAccess();
std::string AndroidGetStorageRoot();

// A message box with up to three buttons, the first the default: the index of the one pressed, -1 if dismissed.
// Blocks the calling thread (not the activity's).
int32_t AndroidShowMessage(const char* title, const char* message, const char* const* buttons, int32_t buttonCount);

// Android's keyboard over a text box: false if cancelled. Blocks the calling thread.
bool AndroidEditText(const char* title, const char* text, bool password, std::string* result);

// The system clipboard's text (false if it has none), and setting it.
bool AndroidGetClipboard(std::string* text);
void AndroidSetClipboard(const char* text);

#endif
