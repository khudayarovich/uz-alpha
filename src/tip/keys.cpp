// Turns a WM_KEYDOWN (virtual key + lParam) into what the engine needs.
#include "textservice.h"

namespace {

// US-English characters by scan code (set 1), unshifted and shifted.
struct Row {
    BYTE first;
    const wchar_t* lower;
    const wchar_t* upper;
};
const Row kRows[] = {
    {0x02, L"1234567890-=", L"!@#$%^&*()_+"},
    {0x10, L"qwertyuiop[]", L"QWERTYUIOP{}"},
    {0x1E, L"asdfghjkl;'`", L"ASDFGHJKL:\"~"},
    {0x2B, L"\\", L"|"},
    {0x2C, L"zxcvbnm,./", L"ZXCVBNM<>?"},
    {0x39, L" ", L" "},
    {0x56, L"\\", L"|"},
};

wchar_t UsChar(UINT scan, bool shift, bool caps) {
    for (const Row& r : kRows) {
        const UINT n = (UINT)lstrlenW(r.lower);
        if (scan < r.first || scan >= r.first + n) continue;
        const wchar_t lo = r.lower[scan - r.first];
        const bool letter = lo >= L'a' && lo <= L'z';
        const bool upper = letter ? (shift != caps) : shift;
        return upper ? r.upper[scan - r.first] : lo;
    }
    return 0;
}

// What the active keyboard layout types for this key (0 if nothing printable).
wchar_t LayoutChar(UINT vk, UINT scan, bool shift, bool caps) {
    BYTE state[256] = {};
    if (shift) state[VK_SHIFT] = 0x80;
    if (caps) state[VK_CAPITAL] = 0x01;
    WCHAR buf[8] = {};
    // Flag bit 2: do not change the kernel keyboard state (Windows 10 1607+).
    const int n = ToUnicodeEx(vk, scan, state, buf, ARRAYSIZE(buf), 0x4, GetKeyboardLayout(0));
    if (n == 1 && buf[0] >= 0x20 && buf[0] != 0x7F) return buf[0];
    return 0;
}

bool Down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

}  // namespace

KeyInfo ClassifyKey(WPARAM wp, LPARAM lp) {
    KeyInfo ki;
    const UINT vk = (UINT)wp;

    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
        case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
            ki.key = uza::Key::ModifierOnly();
            return ki;
    }

    if (Down(VK_CONTROL) || Down(VK_MENU) || Down(VK_LWIN) || Down(VK_RWIN) ||
        vk == VK_PACKET || vk == VK_PROCESSKEY) {
        ki.key = uza::Key::Other();
        return ki;
    }

    if (vk == VK_BACK) {
        ki.key = uza::Key::Backspace();
        return ki;
    }

    // Numeric keypad: a plain non-letter character, never remapped.
    if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) {
        ki.key = uza::Key::Char(L'0');
        return ki;
    }

    const UINT scan = (UINT)((lp >> 16) & 0xFF);
    const bool extended = (lp & (1 << 24)) != 0;
    const bool shift = Down(VK_SHIFT);
    const bool caps = (GetKeyState(VK_CAPITAL) & 1) != 0;

    const wchar_t us = extended ? 0 : UsChar(scan, shift, caps);
    const wchar_t layout = LayoutChar(vk, scan, shift, caps);

    if (!us && !layout) {
        ki.key = uza::Key::Other();  // arrows, Enter, Tab, F-keys...
        return ki;
    }
    ki.usChar = us;
    ki.key = uza::Key::Char(us ? us : layout);
    ki.overrideLayout = us && layout != us;
    return ki;
}
