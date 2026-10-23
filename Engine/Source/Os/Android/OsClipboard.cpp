// Android implementation of Os/Win64/OsClipboard.cpp, on the system clipboard. While the keyboard reads a text box's
// text (Input.cpp), the copy it asks the game for is kept from the clipboard. The returned string is allocated with
// SMemAlloc, as OsClipboardFreeString expects.

#include <cstdint>
#include <cstring>
#include <string>
#include "Engine/Source/Os/OsClipboard.h"
#include "Engine/Source/Os/Android/AndroidInput.h"
#include "Engine/Source/Os/Android/AndroidSystem.h"
#include "Engine/Source/Storm/H/storm.h"

char* IOsClipboardGetString(void* window) {
    std::string text;

    if (!AndroidGetClipboard(&text)) {
        return nullptr;
    }

    size_t size = text.size() + 1;
    char* string = static_cast<char*>(SMemAlloc(static_cast<uint32_t>(size), __FILE__, __LINE__, 0));
    memcpy(string, text.c_str(), size);

    return string;
}

void OsClipboardFreeString(char* string) {
    if (string) {
        SMemFree(string, __FILE__, __LINE__, 0);
    }
}

int32_t IOsClipboardPutString(const char* string, void* window) {
    if (!AndroidInputCaptureCopy(string)) {
        AndroidSetClipboard(string);
    }

    return 1;
}
