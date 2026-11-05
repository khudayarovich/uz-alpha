// Ctrl+Alt+Shift+Y / Ctrl+Alt+Shift+E: convert the text selected in whatever
// application is in front to the new / old alphabet.
//
// There is no universal way to read and replace a selection, except the one
// every application supports: copy and paste. So: remember the clipboard,
// press Ctrl+C for the user, convert the copied text, press Ctrl+V, then put
// the user's own clipboard content back.
#include "app.h"

#include <vector>

#include "../engine/translit.h"

namespace {

struct ClipItem {
    UINT format;
    std::vector<BYTE> data;
};

std::vector<ClipItem> g_saved;
bool g_haveSaved = false;

bool OpenClipboardRetry(HWND hwnd) {
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(hwnd)) return true;
        Sleep(10);
    }
    return false;
}

// Copies everything on the clipboard that is plain memory. Handles that only
// make sense inside their owner (bitmaps, metafiles, palettes) are skipped;
// Windows re-creates those from the formats that are kept.
std::vector<ClipItem> SaveClipboard(HWND hwnd) {
    std::vector<ClipItem> items;
    if (!OpenClipboardRetry(hwnd)) return items;
    size_t total = 0;
    for (UINT f = EnumClipboardFormats(0); f != 0; f = EnumClipboardFormats(f)) {
        if (f == CF_BITMAP || f == CF_METAFILEPICT || f == CF_ENHMETAFILE || f == CF_PALETTE ||
            f == CF_OWNERDISPLAY || f == CF_DSPBITMAP || f == CF_DSPMETAFILEPICT || f == CF_DSPENHMETAFILE ||
            (f >= CF_PRIVATEFIRST && f <= CF_PRIVATELAST) || (f >= CF_GDIOBJFIRST && f <= CF_GDIOBJLAST)) {
            continue;
        }
        const HANDLE h = GetClipboardData(f);
        if (!h) continue;
        const SIZE_T size = GlobalSize(h);
        if (!size || total + size > 64u * 1024 * 1024) continue;
        const void* p = GlobalLock(h);
        if (!p) continue;
        ClipItem item;
        item.format = f;
        item.data.assign((const BYTE*)p, (const BYTE*)p + size);
        GlobalUnlock(h);
        total += size;
        items.push_back(std::move(item));
    }
    CloseClipboard();
    return items;
}

void PutData(UINT format, const void* data, size_t size) {
    const HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!g) return;
    if (void* p = GlobalLock(g)) {
        memcpy(p, data, size);
        GlobalUnlock(g);
        if (SetClipboardData(format, g)) return;  // the clipboard owns it now
    }
    GlobalFree(g);
}

void RestoreClipboard(HWND hwnd, const std::vector<ClipItem>& items) {
    if (!OpenClipboardRetry(hwnd)) return;
    EmptyClipboard();
    for (const ClipItem& item : items) PutData(item.format, item.data.data(), item.data.size());
    CloseClipboard();
}

bool ReadClipboardText(HWND hwnd, std::wstring* text) {
    if (!OpenClipboardRetry(hwnd)) return false;
    bool ok = false;
    if (const HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) {
            *text = p;
            GlobalUnlock(h);
            ok = true;
        }
    }
    CloseClipboard();
    return ok;
}

void WriteClipboardText(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboardRetry(hwnd)) return;
    EmptyClipboard();
    PutData(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    // This is a temporary value: keep it out of clipboard history and sync.
    const DWORD zero = 0;
    PutData(RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"), &zero, sizeof zero);
    PutData(RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"), &zero, sizeof zero);
    PutData(RegisterClipboardFormatW(L"CanUploadToCloudClipboard"), &zero, sizeof zero);
    CloseClipboard();
}

void SendCtrl(WORD vk) {
    INPUT in[4] = {};
    for (INPUT& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = vk;
    in[2].ki.wVk = vk;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(ARRAYSIZE(in), in, sizeof(INPUT));
}

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// The user is still holding Ctrl+Alt+Shift when the hotkey fires; a Ctrl+C
// sent now would arrive as Ctrl+Alt+Shift+C. Wait for the keys to come up.
bool WaitForKeysUp() {
    for (int i = 0; i < 200; ++i) {
        if (!Down(VK_CONTROL) && !Down(VK_MENU) && !Down(VK_SHIFT) && !Down(VK_LWIN) && !Down(VK_RWIN) &&
            !Down('Y') && !Down('E')) {
            return true;
        }
        Sleep(10);
    }
    return false;
}

// In a terminal Ctrl+C interrupts the running program instead of copying.
bool IsTerminal(HWND hwnd) {
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    static const wchar_t* const kTerminals[] = {
        L"ConsoleWindowClass", L"CASCADIA_HOSTING_WINDOW_CLASS", L"mintty", L"PuTTY", L"VirtualConsoleClass",
    };
    for (const wchar_t* t : kTerminals) {
        if (lstrcmpiW(cls, t) == 0) return true;
    }
    return false;
}

}  // namespace

bool RegisterConvertHotkeys(HWND hwnd) {
    const UINT mods = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
    const bool a = RegisterHotKey(hwnd, UZA_HOTKEY_TO_NEW, mods, 'Y') != FALSE;
    const bool b = RegisterHotKey(hwnd, UZA_HOTKEY_TO_OLD, mods, 'E') != FALSE;
    if (!a || !b) Log(L"hotkeys: new=%d old=%d (error %lu)", a, b, GetLastError());
    return a && b;
}

void UnregisterConvertHotkeys(HWND hwnd) {
    UnregisterHotKey(hwnd, UZA_HOTKEY_TO_NEW);
    UnregisterHotKey(hwnd, UZA_HOTKEY_TO_OLD);
}

void RestoreClipboardNow(HWND hwnd) {
    KillTimer(hwnd, UZA_TIMER_CLIPBOARD);
    if (!g_haveSaved) return;
    RestoreClipboard(hwnd, g_saved);
    g_saved.clear();
    g_haveSaved = false;
}

void ConvertSelection(HWND hwnd, int hotkeyId) {
    const HWND target = GetForegroundWindow();
    if (!target || target == hwnd || IsTerminal(target)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    if (g_haveSaved) RestoreClipboardNow(hwnd);  // a previous conversion is still settling
    if (!WaitForKeysUp()) return;

    std::vector<ClipItem> saved = SaveClipboard(hwnd);

    const DWORD before = GetClipboardSequenceNumber();
    SendCtrl('C');
    bool copied = false;
    for (int i = 0; i < 100; ++i) {
        Sleep(10);
        if (GetClipboardSequenceNumber() != before) {
            copied = true;
            break;
        }
    }
    if (!copied) {
        MessageBeep(MB_ICONWARNING);  // nothing selected, or the app does not copy
        return;
    }
    Sleep(40);  // let the application finish putting all its formats up

    std::wstring text;
    if (!ReadClipboardText(hwnd, &text) || text.empty()) {
        RestoreClipboard(hwnd, saved);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    const std::wstring converted =
        hotkeyId == UZA_HOTKEY_TO_NEW ? uza::toNewAlphabet(text) : uza::toOldAlphabet(text);
    if (converted == text) {
        RestoreClipboard(hwnd, saved);
        return;
    }

    WriteClipboardText(hwnd, converted);
    SendCtrl('V');

    // The application reads the clipboard a moment after the key press; give
    // it time before the user's own content goes back.
    g_saved = std::move(saved);
    g_haveSaved = true;
    SetTimer(hwnd, UZA_TIMER_CLIPBOARD, 900, nullptr);
}
