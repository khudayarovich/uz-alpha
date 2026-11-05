// Which keyboard is active, and switching between Avtomatik and Qölda.
// Both are language profiles of the UzAlpha text service.
#include "app.h"

#include <msctf.h>

namespace {

ITfThreadMgr* g_threadMgr = nullptr;
TfClientId g_clientId = TF_CLIENTID_NULL;
ITfInputProcessorProfileMgr* g_profiles = nullptr;
// Set by the text service: the thread it is active and focused on, and which
// of the two keyboards that thread is typing with.
ITfCompartment* g_activeFlag = nullptr;
ITfCompartment* g_modeFlag = nullptr;

LONG ReadFlag(ITfCompartment* flag) {
    if (!flag) return 0;
    VARIANT v;
    VariantInit(&v);
    LONG value = 0;
    if (SUCCEEDED(flag->GetValue(&v)) && v.vt == VT_I4) value = v.lVal;
    VariantClear(&v);
    return value;
}

// The thread the text service reports itself active on, or 0.
LONG ActiveThread() {
    const LONG tid = ReadFlag(g_activeFlag);
    if (!tid) return 0;
    // A program that was killed never cleared its flag: ignore dead threads.
    const HANDLE thread = OpenThread(SYNCHRONIZE, FALSE, (DWORD)tid);
    if (!thread) return GetLastError() == ERROR_ACCESS_DENIED ? tid : 0;  // alive but protected
    const bool alive = WaitForSingleObject(thread, 0) == WAIT_TIMEOUT;
    CloseHandle(thread);
    return alive ? tid : 0;
}

const GUID& ProfileOf(Keyboard k) { return k == Keyboard::Manual ? GUID_UzAlphaManualProfile : GUID_UzAlphaProfile; }

HRESULT Activate(Keyboard k, DWORD flags) {
    return g_profiles->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, UZA_LANGID, CLSID_UzAlphaTip, ProfileOf(k),
                                       nullptr, flags);
}

}  // namespace

bool InputInit() {
    // Our own TSF presence: needed to switch profiles for the whole session
    // and to read the text service's flags.
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITfThreadMgr),
                                   (void**)&g_threadMgr)) &&
        g_threadMgr) {
        if (FAILED(g_threadMgr->Activate(&g_clientId))) {
            g_threadMgr->Release();
            g_threadMgr = nullptr;
        }
    }
    if (g_threadMgr) {
        ITfCompartmentMgr* mgr = nullptr;
        if (SUCCEEDED(g_threadMgr->GetGlobalCompartment(&mgr)) && mgr) {
            mgr->GetCompartment(GUID_UzAlphaActiveCompartment, &g_activeFlag);
            mgr->GetCompartment(GUID_UzAlphaModeCompartment, &g_modeFlag);
            mgr->Release();
        }
    }
    CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                     __uuidof(ITfInputProcessorProfileMgr), (void**)&g_profiles);
    Log(L"input init: thread manager %s, profiles %s, flags %s", g_threadMgr ? L"ok" : L"no",
        g_profiles ? L"ok" : L"no", (g_activeFlag && g_modeFlag) ? L"ok" : L"no");
    return g_profiles != nullptr;
}

void InputShutdown() {
    if (g_activeFlag) g_activeFlag->Release();
    if (g_modeFlag) g_modeFlag->Release();
    if (g_profiles) g_profiles->Release();
    if (g_threadMgr) {
        g_threadMgr->Deactivate();
        g_threadMgr->Release();
    }
    g_activeFlag = g_modeFlag = nullptr;
    g_profiles = nullptr;
    g_threadMgr = nullptr;
}

Keyboard DetectKeyboard() {
    const HWND fg = GetForegroundWindow();
    const DWORD tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD hkl = (DWORD)(ULONG_PTR)GetKeyboardLayout(tid);

    // Any other language: UzAlpha is simply not involved.
    if (LOWORD(hkl) != UZA_LANGID) return Keyboard::Other;
    // The text service says where it is working, and in which mode.
    if (ActiveThread() == 0) return Keyboard::Other;
    return ReadFlag(g_modeFlag) == UZA_MODE_MANUAL ? Keyboard::Manual : Keyboard::Auto;
}

LANGID ForegroundLanguage() {
    const HWND fg = GetForegroundWindow();
    const DWORD tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    return LOWORD((DWORD)(ULONG_PTR)GetKeyboardLayout(tid));
}

// Only the modifier keys are looked at, never what is being typed.
bool LanguageKeysDown() {
    // "Language Hotkey": 1 = Left Alt+Shift (default), 2 = Ctrl+Shift, 3 = none.
    static int choice = 0;
    static DWORD readAt = 0;
    if (!choice || GetTickCount() - readAt > 10000) {
        wchar_t buf[8] = L"1";
        DWORD size = sizeof buf;
        RegGetValueW(HKEY_CURRENT_USER, L"Keyboard Layout\\Toggle", L"Language Hotkey", RRF_RT_REG_SZ, nullptr, buf, &size);
        choice = buf[0] == L'2' ? 2 : buf[0] == L'3' || buf[0] == L'4' ? 3 : 1;
        readAt = GetTickCount();
    }
    const bool alt = GetAsyncKeyState(VK_MENU) < 0, shift = GetAsyncKeyState(VK_SHIFT) < 0;
    const bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
    const bool win = GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0;
    if (win) return false;
    if (choice == 1) return alt && shift && !ctrl;
    if (choice == 2) return ctrl && shift && !alt;
    return false;
}

bool ProfilesEnabled() {
    if (!g_profiles) return false;
    int enabled = 0;
    IEnumTfInputProcessorProfiles* en = nullptr;
    if (SUCCEEDED(g_profiles->EnumProfiles(UZA_LANGID, &en)) && en) {
        TF_INPUTPROCESSORPROFILE p = {};
        ULONG got = 0;
        while (en->Next(1, &p, &got) == S_OK && got == 1) {
            if (p.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR && IsEqualCLSID(p.clsid, CLSID_UzAlphaTip) &&
                (p.dwFlags & TF_IPP_FLAG_ENABLED)) {
                ++enabled;
            }
        }
        en->Release();
    }
    return enabled >= 2;
}

void LogInputState(const wchar_t* why) {
    const HWND fg = GetForegroundWindow();
    const DWORD tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD hkl = (DWORD)(ULONG_PTR)GetKeyboardLayout(tid);
    TF_INPUTPROCESSORPROFILE active = {};
    const HRESULT hr = g_profiles ? g_profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active) : E_FAIL;
    Log(L"%s: foreground hkl=%08lx | our thread: profile type=%lu ours=%d manual=%d lang=%04x (hr %08lx) | "
        L"active thread=%ld mode=%ld | enabled=%d | service loaded here=%d",
        why, hkl, active.dwProfileType, (int)IsEqualCLSID(active.clsid, CLSID_UzAlphaTip),
        (int)IsEqualGUID(active.guidProfile, GUID_UzAlphaManualProfile), active.langid, hr, ActiveThread(),
        ReadFlag(g_modeFlag), (int)ProfilesEnabled(), (int)(GetModuleHandleW(UZA_TIP_DLL) != nullptr));
}

bool SwitchKeyboard(Keyboard k) {
    if (!g_profiles || k == Keyboard::Other) return false;

    // Windows will happily "activate" a profile it has marked disabled and
    // then fall back to a plain layout, so make sure of it first.
    if (!ProfilesEnabled()) {
        Log(L"the keyboards are not enabled for this user: enabling them");
        EnableForUser();
    }

    HRESULT hr = Activate(k, TF_IPPMF_FORSESSION);
    Log(L"switch to %s: %08lx", k == Keyboard::Auto ? L"auto" : L"manual", hr);
    if (hr == S_OK) return true;

    // The session is on another input language: move it to Uzbek first.
    ITfInputProcessorProfiles* legacy = nullptr;
    if (SUCCEEDED(g_profiles->QueryInterface(__uuidof(ITfInputProcessorProfiles), (void**)&legacy)) && legacy) {
        const HRESULT hrLang = legacy->ChangeCurrentLanguage(UZA_LANGID);
        legacy->Release();
        hr = Activate(k, TF_IPPMF_FORSESSION);
        Log(L"  after changing the language (%08lx): %08lx", hrLang, hr);
        if (hr == S_OK) return true;
    }

    hr = Activate(k, TF_IPPMF_FORSESSION | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE);
    Log(L"  ignoring the current language: %08lx", hr);
    return hr == S_OK;
}
