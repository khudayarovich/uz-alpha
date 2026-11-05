// Ctrl+Alt+Shift+Y / Ctrl+Alt+Shift+E: convert the text selected in whatever
// application is in front to the new / old alphabet.
//
// There is no universal way to read and replace a selection, except the one
// every application supports: copy and paste. So: remember the clipboard,
// press Ctrl+C for the user, convert the copied text, press Ctrl+V, then put
// the user's own clipboard content back.
//
// Two rules keep this from disturbing the program being typed in:
//
// 1. Nothing here waits. A program that copies has to tell the clipboard's
//    owner first (WM_DESTROYCLIPBOARD, sent and waited for), and after a
//    conversion the owner is UzAlpha. Waiting for the copy with Sleep() made
//    that program hang until the wait gave up, and the conversion was lost.
//    Every step is therefore a timer tick, and the message loop keeps running.
// 2. The clipboard is owned by a window on a thread of its own that does
//    nothing but answer messages, so even when the main thread is busy with
//    something else (an install, an update) nobody is kept waiting.
#include "app.h"

#include <vector>

#include "../engine/translit.h"

namespace {

struct ClipItem {
    UINT format;
    std::vector<BYTE> data;
};

enum class Stage {
    Idle,
    KeysUp,   // waiting for the user to let go of the shortcut
    Copy,     // Ctrl+C sent: waiting for the clipboard to change
    Settle,   // it changed: waiting for the application to finish writing
    Restore,  // Ctrl+V sent: the user's own clipboard goes back after a pause
};

struct Job {
    Stage stage = Stage::Idle;
    int hotkey = 0;
    HWND target = nullptr;
    wchar_t targetClass[64] = {};
    DWORD began = 0;  // when the shortcut arrived
    DWORD stageBegan = 0;
    DWORD sequence = 0;           // the clipboard's sequence number last seen
    std::vector<ClipItem> saved;  // the user's own clipboard content
    bool haveSaved = false;
};

Job g_job;

const UINT kTickMs = 10;
const DWORD kKeysUpLimitMs = 2500;  // how long the shortcut may be held
const DWORD kCopyLimitMs = 1000;    // how long the application may take to copy
const UINT kSettleMs = 40;          // quiet time after the clipboard last changed
const DWORD kSettleLimitMs = 800;
const DWORD kRestoreAfterMs = 900;  // the application reads the clipboard a moment after Ctrl+V

// The window that owns the clipboard for us (rule 2 above).
HWND g_clipWnd = nullptr;
HANDLE g_clipThread = nullptr;
DWORD g_clipThreadId = 0;

DWORD WINAPI ClipboardThread(void* ready) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"UzAlphaClipboard";
    RegisterClassExW(&wc);
    g_clipWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    SetEvent((HANDLE)ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    if (g_clipWnd) DestroyWindow(g_clipWnd);
    g_clipWnd = nullptr;
    return 0;
}

void StartClipboardWindow() {
    if (g_clipThread) return;
    const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) return;
    g_clipThread = CreateThread(nullptr, 0, ClipboardThread, ready, 0, &g_clipThreadId);
    if (g_clipThread) WaitForSingleObject(ready, 2000);
    CloseHandle(ready);
    if (!g_clipWnd) Log(L"convert: no clipboard window, the main window stands in");
}

void StopClipboardWindow() {
    if (!g_clipThread) return;
    PostThreadMessageW(g_clipThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_clipThread, 1000);
    CloseHandle(g_clipThread);
    g_clipThread = nullptr;
}

// The window to open the clipboard with.
HWND Owner(HWND fallback) { return g_clipWnd ? g_clipWnd : fallback; }

// A few quick tries only: whoever has the clipboard open is done in a moment,
// and a long wait here is exactly what rule 1 forbids.
bool OpenClipboardRetry(HWND hwnd) {
    for (int i = 0; i < 6; ++i) {
        if (OpenClipboard(Owner(hwnd))) return true;
        Sleep(5);
    }
    return false;
}

// Formats that only mean something inside the program that put them there
// (OLE objects and links). Asking for them makes that program build an
// embedded object, which can take seconds, and the copy would be useless.
bool IsOleOnlyFormat(UINT format) {
    if (format < 0xC000) return false;
    wchar_t name[64] = {};
    if (!GetClipboardFormatNameW(format, name, ARRAYSIZE(name))) return false;
    static const wchar_t* const kSkip[] = {
        L"DataObject",  L"Ole Private Data",       L"Embed Source",      L"Embedded Object", L"Link Source",
        L"Native",      L"Link Source Descriptor", L"Object Descriptor", L"OwnerLink",       L"ObjectLink",
    };
    for (const wchar_t* skip : kSkip) {
        if (lstrcmpiW(name, skip) == 0) return true;
    }
    return false;
}

// Copies everything on the clipboard that is plain memory. Handles that only
// make sense inside their owner (bitmaps, metafiles, palettes) are skipped;
// Windows re-creates those from the formats that are kept. Returns false when
// the clipboard could not be opened, so that nothing is "restored" over it.
bool SaveClipboard(HWND hwnd, std::vector<ClipItem>* out) {
    std::vector<ClipItem>& items = *out;
    items.clear();
    if (!OpenClipboardRetry(hwnd)) return false;
    size_t total = 0;
    for (UINT f = EnumClipboardFormats(0); f != 0; f = EnumClipboardFormats(f)) {
        if (f == CF_BITMAP || f == CF_METAFILEPICT || f == CF_ENHMETAFILE || f == CF_PALETTE ||
            f == CF_OWNERDISPLAY || f == CF_DSPBITMAP || f == CF_DSPMETAFILEPICT || f == CF_DSPENHMETAFILE ||
            (f >= CF_PRIVATEFIRST && f <= CF_PRIVATELAST) || (f >= CF_GDIOBJFIRST && f <= CF_GDIOBJLAST) ||
            IsOleOnlyFormat(f)) {
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
    return true;
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

enum class Read { Text, NoText, Busy };

// One try: when the application still has the clipboard open, the caller
// comes back on the next tick instead of waiting here.
Read ReadClipboardText(HWND hwnd, std::wstring* text) {
    if (!OpenClipboard(Owner(hwnd))) return Read::Busy;
    Read result = Read::NoText;
    if (const HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) {
            *text = p;
            GlobalUnlock(h);
            result = Read::Text;
        }
    }
    CloseClipboard();
    return result;
}

bool WriteClipboardText(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboardRetry(hwnd)) return false;
    EmptyClipboard();
    PutData(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    // This is a temporary value: keep it out of clipboard history and sync.
    const DWORD zero = 0;
    PutData(RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"), &zero, sizeof zero);
    PutData(RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"), &zero, sizeof zero);
    PutData(RegisterClipboardFormatW(L"CanUploadToCloudClipboard"), &zero, sizeof zero);
    CloseClipboard();
    return true;
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
// sent now would arrive as Ctrl+Alt+Shift+C. So the keys have to come up first.
bool ShortcutKeysDown() {
    return Down(VK_CONTROL) || Down(VK_MENU) || Down(VK_SHIFT) || Down(VK_LWIN) || Down(VK_RWIN) || Down('Y') ||
           Down('E');
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

const wchar_t* Direction() { return g_job.hotkey == UZA_HOTKEY_TO_NEW ? L"new" : L"old"; }

void Enter(HWND hwnd, Stage stage, UINT tickMs) {
    g_job.stage = stage;
    g_job.stageBegan = GetTickCount();
    SetTimer(hwnd, UZA_TIMER_CONVERT, tickMs, nullptr);
}

// Ends the conversion. The text itself is never written to the log.
void End(HWND hwnd, const wchar_t* outcome, bool restoreClipboard, bool beep) {
    KillTimer(hwnd, UZA_TIMER_CONVERT);
    if (restoreClipboard && g_job.haveSaved) RestoreClipboard(hwnd, g_job.saved);
    if (outcome) {
        Log(L"convert to %s in \"%s\": %s (%lu ms)", Direction(), g_job.targetClass, outcome,
            GetTickCount() - g_job.began);
    }
    g_job = Job();
    if (beep) MessageBeep(MB_ICONWARNING);
}

// The clipboard holds what the application copied: convert it and paste.
void PasteConverted(HWND hwnd) {
    std::wstring text;
    const Read read = ReadClipboardText(hwnd, &text);
    if (read == Read::Busy) {
        if (GetTickCount() - g_job.stageBegan > kSettleLimitMs) End(hwnd, L"the clipboard stayed busy", true, true);
        return;  // look again on the next tick
    }
    if (read == Read::NoText || text.empty()) {
        End(hwnd, L"the copy holds no text", true, true);
        return;
    }
    const std::wstring converted =
        g_job.hotkey == UZA_HOTKEY_TO_NEW ? uza::toNewAlphabet(text) : uza::toOldAlphabet(text);
    if (converted == text) {
        End(hwnd, L"nothing to change", true, false);
        return;
    }
    if (GetForegroundWindow() != g_job.target) {
        End(hwnd, L"another window came forward", true, false);
        return;
    }
    if (!WriteClipboardText(hwnd, converted)) {
        End(hwnd, L"the clipboard could not be written", true, true);
        return;
    }
    SendCtrl('V');
    Log(L"convert to %s in \"%s\": %u characters (%lu ms)", Direction(), g_job.targetClass, (unsigned)text.size(),
        GetTickCount() - g_job.began);
    Enter(hwnd, Stage::Restore, 50);
}

}  // namespace

bool RegisterConvertHotkeys(HWND hwnd) {
    StartClipboardWindow();
    const UINT mods = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
    const bool a = RegisterHotKey(hwnd, UZA_HOTKEY_TO_NEW, mods, 'Y') != FALSE;
    const bool b = RegisterHotKey(hwnd, UZA_HOTKEY_TO_OLD, mods, 'E') != FALSE;
    if (!a || !b) Log(L"hotkeys: new=%d old=%d (error %lu)", a, b, GetLastError());
    return a && b;
}

void UnregisterConvertHotkeys(HWND hwnd) {
    UnregisterHotKey(hwnd, UZA_HOTKEY_TO_NEW);
    UnregisterHotKey(hwnd, UZA_HOTKEY_TO_OLD);
    StopClipboardWindow();
}

void RestoreClipboardNow(HWND hwnd) {
    if (g_job.stage != Stage::Idle) End(hwnd, nullptr, true, false);
}

void ConvertSelection(HWND hwnd, int hotkeyId) {
    // One at a time: a press that arrives while the last one is still being
    // carried out would copy a selection that is about to be replaced.
    if (g_job.stage == Stage::KeysUp || g_job.stage == Stage::Copy || g_job.stage == Stage::Settle) return;

    const HWND target = GetForegroundWindow();
    if (!target || target == hwnd || IsTerminal(target)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    // Pressed again right after a conversion: the clipboard still holds our
    // converted text, and what the user had there is already saved. Keep that.
    Job next;
    if (g_job.stage == Stage::Restore) {
        next.saved = std::move(g_job.saved);
        next.haveSaved = g_job.haveSaved;
    }
    g_job = std::move(next);
    g_job.hotkey = hotkeyId;
    g_job.target = target;
    GetClassNameW(target, g_job.targetClass, ARRAYSIZE(g_job.targetClass));
    g_job.began = GetTickCount();
    Enter(hwnd, Stage::KeysUp, kTickMs);
}

void ConvertTick(HWND hwnd) {
    const DWORD inStage = GetTickCount() - g_job.stageBegan;
    switch (g_job.stage) {
        case Stage::Idle:
            KillTimer(hwnd, UZA_TIMER_CONVERT);
            return;

        case Stage::KeysUp:
            if (ShortcutKeysDown()) {
                if (inStage > kKeysUpLimitMs) End(hwnd, L"the keys were not released", true, false);
                return;
            }
            if (GetForegroundWindow() != g_job.target) {
                End(hwnd, L"another window came forward", true, false);
                return;
            }
            if (!g_job.haveSaved) g_job.haveSaved = SaveClipboard(hwnd, &g_job.saved);
            g_job.sequence = GetClipboardSequenceNumber();
            SendCtrl('C');
            Enter(hwnd, Stage::Copy, kTickMs);
            return;

        case Stage::Copy:
            if (GetClipboardSequenceNumber() != g_job.sequence) {
                g_job.sequence = GetClipboardSequenceNumber();
                Enter(hwnd, Stage::Settle, kSettleMs);
            } else if (inStage > kCopyLimitMs) {
                // Nothing selected, or the application does not copy. If the
                // clipboard still holds our text from the press before, the
                // user's own content goes back.
                End(hwnd, L"nothing was copied", true, true);
            }
            return;

        case Stage::Settle: {
            // The application may still be adding formats: wait until the
            // clipboard has been quiet for one tick.
            const DWORD sequence = GetClipboardSequenceNumber();
            if (sequence != g_job.sequence && inStage < kSettleLimitMs) {
                g_job.sequence = sequence;
                return;
            }
            PasteConverted(hwnd);
            return;
        }

        case Stage::Restore:
            if (inStage >= kRestoreAfterMs) End(hwnd, nullptr, true, false);
            return;
    }
}
