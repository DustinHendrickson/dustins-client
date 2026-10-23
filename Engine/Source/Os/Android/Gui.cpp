// Android implementation of the Os GUI functions (Os/Win64/Gui.cpp) the client uses: the Gx window registry, message
// boxes (an AlertDialog from the activity), and modifier state.

#include <cstdint>
#include "Engine/Source/Os/Gui.h"
#include "Engine/Source/Os/Android/AndroidInput.h"
#include "Engine/Source/Os/Android/AndroidSystem.h"

static void* s_GxDevWindow;

void OsGuiSetWindowTitle(void* window, const char* title) {
}

void* OsGuiGetWindow(int32_t type) {
    // The game window is the screen, and always the key and main window.
    return type >= 0 && type <= 2 ? s_GxDevWindow : nullptr;
}

void OsGuiSetGxWindow(void* window) {
    s_GxDevWindow = window;
}

int32_t OsGuiMessageBox(void* parentWindow, int32_t style, const char* message, const char* title) {
    // Results as on Windows: 0 for OK / Yes, 1 for No, 2 for Cancel. Dismissing the box (Back) answers the last.
    const char* buttons[3];
    int32_t results[3];
    int32_t count = 0;

    switch (style) {
        case 1:
            buttons[count] = "OK";
            results[count++] = 0;
            buttons[count] = "Cancel";
            results[count++] = 2;
            break;
        case 2:
            buttons[count] = "Yes";
            results[count++] = 0;
            buttons[count] = "No";
            results[count++] = 1;
            break;
        case 3:
            buttons[count] = "Yes";
            results[count++] = 0;
            buttons[count] = "No";
            results[count++] = 1;
            buttons[count] = "Cancel";
            results[count++] = 2;
            break;
        default:
            buttons[count] = "OK";
            results[count++] = 0;
            break;
    }

    int32_t index = AndroidShowMessage(title, message, buttons, count);

    return index >= 0 && index < count ? results[index] : results[count - 1];
}

int32_t OsGuiIsModifierKeyDown(int32_t key) {
    return AndroidIsModifierDown(key);
}

void OsGuiBeep() {
}

int32_t OsGuiProcessMessage(void* message) {
    return 0;
}
