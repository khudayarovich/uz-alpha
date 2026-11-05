// Which keyboard is active, and switching between Auto and Manual.
#include "app.h"

#include <msctf.h>

namespace {

ITfThreadMgr* g_threadMgr = nullptr;
TfClientId g_clientId = TF_CLIENTID_NULL;
ITfInputProcessorProfileMgr* g_profiles = nullptr;
// Set by the Auto text service to the thread it is active and focused on.
ITfCompartment* g_activeFlag = nullptr;

DWORD ManualHkl() {
    DWORD hkl = 0;
    return FindManualLayout(nullptr, &hkl) ? hkl : 0;
}

// The thread the Auto text service reports itself active on, or 0.
LONG AutoThread() {
    if (!g_activeFlag) return 0;
    VARIANT v;
    VariantInit(&v);
    LONG tid = 0;
    if (SUCCEEDED(g_activeFlag->GetValue(&v)) && v.vt == VT_I4) tid = v.lVal;
    VariantClear(&v);
    if (!tid) return 0;

    // A program that was killed never cleared its flag: ignore dead threads.
    const HANDLE thread = OpenThread(SYNCHRONIZE, FALSE, (DWORD)tid);
    if (!thread) return GetLastError() == ERROR_ACCESS_DENIED ? tid : 0;  // alive but protected
    const bool alive = WaitForSingleObject(thread, 0) == WAIT_TIMEOUT;
    CloseHandle(thread);
    return alive ? tid : 0;
}

HRESULT Activate(Keyboard k, DWORD flags) {
    if (k == Keyboard::Auto) {
        return g_profiles->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, UZA_LANGID, CLSID_UzAlphaTip,
                                           GUID_UzAlphaProfile, nullptr, flags);
    }
    const DWORD hkl = ManualHkl();
    if (!hkl) return E_FAIL;
    // Layout handles are sign-extended 32-bit values.
    return g_profiles->ActivateProfile(TF_PROFILETYPE_KEYBOARDLAYOUT, UZA_LANGID, CLSID_NULL, GUID_NULL,
                                       (HKL)(LONG_PTR)(LONG)hkl, flags);
}

}  // namespace

bool InputInit() {
    // Our own TSF presence: needed to switch profiles for the whole session
    // and to read the text service's "active" flag.
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
            mgr->Release();
        }
    }
    CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                     __uuidof(ITfInputProcessorProfileMgr), (void**)&g_profiles);
    Log(L"input init: thread manager %s, profiles %s, flag %s", g_threadMgr ? L"ok" : L"no",
        g_profiles ? L"ok" : L"no", g_activeFlag ? L"ok" : L"no");
    return g_profiles != nullptr;
}

void InputShutdown() {
    if (g_activeFlag) g_activeFlag->Release();
    if (g_profiles) g_profiles->Release();
    if (g_threadMgr) {
        g_threadMgr->Deactivate();
        g_threadMgr->Release();
    }
    g_activeFlag = nullptr;
    g_profiles = nullptr;
    g_threadMgr = nullptr;
}

Keyboard DetectKeyboard() {
    const HWND fg = GetForegroundWindow();
    const DWORD tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD hkl = (DWORD)(ULONG_PTR)GetKeyboardLayout(tid);

    // Any other language: UzAlpha is simply not involved.
    if (LOWORD(hkl) != UZA_LANGID) return Keyboard::Other;

    const DWORD manual = ManualHkl();
    if (manual && hkl == manual) return Keyboard::Manual;

    // The text service says where it is working.
    if (AutoThread() != 0) return Keyboard::Auto;

    // Without that flag (a sandboxed app may not be able to set it): Uzbek
    // with a plain, non-custom layout handle can only be the text service.
    if (!g_activeFlag && (HIWORD(hkl) & 0xF000) != 0xF000) return Keyboard::Auto;
    return Keyboard::Other;
}

bool AutoProfileEnabled() {
    if (!g_profiles) return false;
    bool enabled = false;
    IEnumTfInputProcessorProfiles* en = nullptr;
    if (SUCCEEDED(g_profiles->EnumProfiles(UZA_LANGID, &en)) && en) {
        TF_INPUTPROCESSORPROFILE p = {};
        ULONG got = 0;
        while (en->Next(1, &p, &got) == S_OK && got == 1) {
            if (p.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR && IsEqualCLSID(p.clsid, CLSID_UzAlphaTip)) {
                enabled = (p.dwFlags & TF_IPP_FLAG_ENABLED) != 0;
                break;
            }
        }
        en->Release();
    }
    return enabled;
}

void LogInputState(const wchar_t* why) {
    const HWND fg = GetForegroundWindow();
    const DWORD tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD hkl = (DWORD)(ULONG_PTR)GetKeyboardLayout(tid);
    TF_INPUTPROCESSORPROFILE active = {};
    const HRESULT hr = g_profiles ? g_profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active) : E_FAIL;
    Log(L"%s: foreground hkl=%08lx | our thread: profile type=%lu ours=%d lang=%04x (hr %08lx) | flag=%ld | "
        L"auto enabled=%d | service loaded here=%d",
        why, hkl, active.dwProfileType, (int)IsEqualCLSID(active.clsid, CLSID_UzAlphaTip), active.langid, hr,
        AutoThread(), (int)AutoProfileEnabled(), (int)(GetModuleHandleW(UZA_TIP_DLL) != nullptr));
}

bool SwitchKeyboard(Keyboard k) {
    if (!g_profiles || k == Keyboard::Other) return false;

    // Windows will happily "activate" a profile it has marked disabled and
    // then fall back to a plain layout, so make sure of it first.
    if (k == Keyboard::Auto && !AutoProfileEnabled()) {
        Log(L"the Auto profile is not enabled for this user: enabling it");
        EnableForUser();
    }

    HRESULT hr = Activate(k, TF_IPPMF_FORSESSION);
    Log(L"switch to %s: %08lx", k == Keyboard::Auto ? L"auto" : L"manual", hr);
    if (hr == S_OK) return true;

    // Not in this user's keyboard list (yet)?
    if (EnableForUser()) {
        hr = Activate(k, TF_IPPMF_FORSESSION);
        Log(L"  after enabling for the user: %08lx", hr);
        if (hr == S_OK) return true;
    }

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
