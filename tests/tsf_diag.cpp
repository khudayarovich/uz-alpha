// Diagnostics for the installed Auto keyboard. Touches this process only.
//
//   tsf_diag.exe profile <log>            activate the installed Auto profile for
//                                         this process and report what TSF does
//   tsf_diag.exe setflag <log> <n> <ms>   set the "active" flag to n for ms milliseconds
//   tsf_diag.exe getflag <log>            read the "active" flag
//
// GUI subsystem: TSF does not stay active in console programs.
#include <windows.h>
#include <msctf.h>
#include <shellapi.h>

#include <cstdio>

#include "../src/common/ids.h"

static FILE* g_log = nullptr;

static void pump(DWORD ms) {
    const DWORD end = GetTickCount() + ms;
    do {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    } while (GetTickCount() < end);
}

static ITfCompartment* flag(ITfThreadMgr* tm) {
    ITfCompartmentMgr* mgr = nullptr;
    ITfCompartment* c = nullptr;
    const HRESULT hr = tm->GetGlobalCompartment(&mgr);
    if (SUCCEEDED(hr) && mgr) {
        const HRESULT hr2 = mgr->GetCompartment(GUID_UzAlphaActiveCompartment, &c);
        if (FAILED(hr2)) fprintf(g_log, "GetCompartment: %08lx\n", hr2);
        mgr->Release();
    } else {
        fprintf(g_log, "GetGlobalCompartment: %08lx\n", hr);
    }
    return c;
}

static void reportFlag(ITfThreadMgr* tm, const char* when) {
    ITfCompartment* c = flag(tm);
    if (!c) return;
    VARIANT v;
    VariantInit(&v);
    const HRESULT hr = c->GetValue(&v);
    fprintf(g_log, "flag %s: hr=%08lx vt=%u value=%ld\n", when, hr, (unsigned)v.vt, v.vt == VT_I4 ? v.lVal : -1);
    VariantClear(&v);
    c->Release();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 3) return 2;
    _wfopen_s(&g_log, argv[2], L"w");
    if (!g_log) return 2;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"UzAlphaTsfDiag";
    RegisterClassW(&wc);
    CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, -3000, -3000, 10, 10,
                    nullptr, nullptr, inst, nullptr);

    ITfThreadMgr* tm = nullptr;
    TfClientId id = TF_CLIENTID_NULL;
    HRESULT hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITfThreadMgr),
                                  (void**)&tm);
    if (SUCCEEDED(hr)) hr = tm->Activate(&id);
    fprintf(g_log, "thread manager: %08lx client id %lu, thread %lu\n", hr, (unsigned long)id, GetCurrentThreadId());
    if (FAILED(hr)) return 2;

    if (lstrcmpiW(argv[1], L"setflag") == 0 && argc >= 5) {
        const LONG value = _wtol(argv[3]);
        if (ITfCompartment* c = flag(tm)) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = value;
            const HRESULT hrSet = c->SetValue(id, &v);
            fprintf(g_log, "SetValue(%ld): %08lx\n", value, hrSet);
            fflush(g_log);
            reportFlag(tm, "after set");
            fflush(g_log);
            pump((DWORD)_wtol(argv[4]));
            v.lVal = 0;
            c->SetValue(id, &v);
            c->Release();
        }
    } else if (lstrcmpiW(argv[1], L"getflag") == 0) {
        reportFlag(tm, "read from another process");
    } else {
        fprintf(g_log, "layout before: %08lx\n", (unsigned long)(ULONG_PTR)GetKeyboardLayout(0));
        ITfInputProcessorProfileMgr* profiles = nullptr;
        hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                              __uuidof(ITfInputProcessorProfileMgr), (void**)&profiles);
        if (SUCCEEDED(hr)) {
            TF_INPUTPROCESSORPROFILE p = {};
            hr = profiles->GetProfile(TF_PROFILETYPE_INPUTPROCESSOR, UZA_LANGID, CLSID_UzAlphaTip,
                                      GUID_UzAlphaProfile, nullptr, &p);
            fprintf(g_log, "GetProfile: %08lx flags=%08lx caps=%08lx keyboardcat=%s substitute=%08lx\n", hr,
                    p.dwFlags, p.dwCaps, IsEqualGUID(p.catid, GUID_TFCAT_TIP_KEYBOARD) ? "yes" : "no",
                    (unsigned long)(ULONG_PTR)p.hklSubstitute);

            ITfInputProcessorProfiles* legacy = nullptr;
            profiles->QueryInterface(__uuidof(ITfInputProcessorProfiles), (void**)&legacy);
            if (legacy && argc >= 4 && lstrcmpiW(argv[3], L"enable") == 0) {
                // The per-user "enable" call; it also refreshes TSF's cached list.
                const HRESULT hrEnable = legacy->EnableLanguageProfile(CLSID_UzAlphaTip, UZA_LANGID,
                                                                       GUID_UzAlphaProfile, TRUE);
                fprintf(g_log, "EnableLanguageProfile(TRUE): %08lx\n", hrEnable);
                pump(300);
            }
            if (legacy) {
                BOOL enabled = FALSE;
                const HRESULT hrEn = legacy->IsEnabledLanguageProfile(CLSID_UzAlphaTip, UZA_LANGID,
                                                                      GUID_UzAlphaProfile, &enabled);
                LANGID cur = 0;
                legacy->GetCurrentLanguage(&cur);
                fprintf(g_log, "IsEnabledLanguageProfile: %08lx enabled=%d | current language %04x\n", hrEn,
                        (int)enabled, cur);
            }
            IEnumTfInputProcessorProfiles* en = nullptr;
            if (SUCCEEDED(profiles->EnumProfiles(UZA_LANGID, &en)) && en) {
                TF_INPUTPROCESSORPROFILE e = {};
                ULONG got = 0;
                while (en->Next(1, &e, &got) == S_OK && got == 1) {
                    fprintf(g_log, "  0443 profile: type=%lu ours=%s keyboardcat=%s flags=%08lx hkl=%08lx\n",
                            e.dwProfileType, IsEqualCLSID(e.clsid, CLSID_UzAlphaTip) ? "yes" : "no",
                            IsEqualGUID(e.catid, GUID_TFCAT_TIP_KEYBOARD) ? "yes" : "no", e.dwFlags,
                            (unsigned long)(ULONG_PTR)e.hkl);
                }
                en->Release();
            }

            const DWORD variants[] = {TF_IPPMF_FORPROCESS, TF_IPPMF_FORPROCESS | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE,
                                      TF_IPPMF_FORPROCESS | TF_IPPMF_ENABLEPROFILE, 0};
            for (int pass = 0; pass < 4; ++pass) {
                const DWORD flags = variants[pass];
                hr = profiles->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, UZA_LANGID, CLSID_UzAlphaTip,
                                               GUID_UzAlphaProfile, nullptr, flags);
                pump(600);
                TF_INPUTPROCESSORPROFILE active = {};
                const HRESULT hrActive = profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active);
                fprintf(g_log,
                        "ActivateProfile(flags %08lx): %08lx | layout %08lx | active profile hr=%08lx type=%lu ours=%s "
                        "lang=%04x | dll loaded: %s\n",
                        flags, hr, (unsigned long)(ULONG_PTR)GetKeyboardLayout(0), hrActive, active.dwProfileType,
                        IsEqualCLSID(active.clsid, CLSID_UzAlphaTip) ? "yes" : "no", active.langid,
                        GetModuleHandleW(UZA_TIP_DLL) ? "yes" : "no");
                if (legacy) {
                    LANGID lang = 0;
                    GUID prof = {};
                    const HRESULT hrAl = legacy->GetActiveLanguageProfile(CLSID_UzAlphaTip, &lang, &prof);
                    fprintf(g_log, "    GetActiveLanguageProfile(ours): %08lx lang=%04x\n", hrAl, lang);
                }
                fflush(g_log);
            }
            if (legacy) legacy->Release();
            // Can COM create the service at all in this process?
            IUnknown* unk = nullptr;
            const HRESULT hrCreate = CoCreateInstance(CLSID_UzAlphaTip, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown,
                                                      (void**)&unk);
            fprintf(g_log, "CoCreateInstance(text service): %08lx, dll loaded: %s\n", hrCreate,
                    GetModuleHandleW(UZA_TIP_DLL) ? "yes" : "no");
            if (unk) unk->Release();
            reportFlag(tm, "in this process");
            profiles->Release();
        }
    }

    tm->Deactivate();
    tm->Release();
    fclose(g_log);
    CoUninitialize();
    return 0;
}
