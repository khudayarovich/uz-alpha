// End-to-end test of the Auto text service against a real TSF document, with
// no installation and no real keyboard input.
//
//   tip_harness.exe <UzAlphaTip.dll> <log file> [us|current]
//
// It hosts a TSF-aware RichEdit control, loads the text service DLL directly,
// activates it on this thread and feeds key events through the TSF keystroke
// manager exactly as an application's message loop would. Keys the service
// does not eat are applied to the control the way the application would.
// Must be a GUI-subsystem program: TSF does not stay active in console apps.
#include <windows.h>
#include <msctf.h>
#include <richedit.h>

#include <cstdio>
#include <string>

#include "../src/common/ids.h"

// Test-only identifiers for the process-local registration, so the harness
// never shares a class id or profile with an installed copy of the product.
// {D9F56124-9477-4313-9A6B-73F027AD692B}
static const GUID CLSID_HarnessTip = {0xd9f56124, 0x9477, 0x4313, {0x9a, 0x6b, 0x73, 0xf0, 0x27, 0xad, 0x69, 0x2b}};
// {0ADF5BB1-3BEB-4673-B0DC-736E7FDCDA0A}
static const GUID GUID_HarnessProfile = {0x0adf5bb1, 0x3beb, 0x4673, {0xb0, 0xdc, 0x73, 0x6e, 0x7f, 0xdc, 0xda, 0x0a}};

static FILE* g_log = nullptr;
static int g_failed = 0;
static int g_checks = 0;

static std::string esc(const std::wstring& s) {
    std::string o;
    char buf[16];
    for (wchar_t c : s) {
        if (c >= 0x20 && c < 0x7F) {
            o += (char)c;
        } else {
            snprintf(buf, sizeof buf, "\\u%04X", (unsigned)c);
            o += buf;
        }
    }
    return o;
}

static void pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

struct KeyDef {
    UINT vk;
    UINT scan;
    bool shift;
};

// US-English key for a character (\b = Backspace, \x01 = Right arrow).
static bool keyFor(wchar_t c, KeyDef& k) {
    static const struct {
        UINT first;
        const wchar_t* lower;
        const wchar_t* upper;
    } rows[] = {
        {0x02, L"1234567890-=", L"!@#$%^&*()_+"},
        {0x10, L"qwertyuiop[]", L"QWERTYUIOP{}"},
        {0x1E, L"asdfghjkl;'`", L"ASDFGHJKL:\"~"},
        {0x2C, L"zxcvbnm,./", L"ZXCVBNM<>?"},
        {0x39, L" ", L" "},
    };
    if (c == L'\b') { k = {VK_BACK, 0x0E, false}; return true; }
    if (c == 1) { k = {VK_RIGHT, 0x4D, false}; return true; }
    if (c == 2) { k = {VK_LEFT, 0x4B, false}; return true; }
    for (auto& r : rows) {
        for (UINT i = 0; r.lower[i]; ++i) {
            const bool up = r.upper[i] == c && r.lower[i] != c;
            if (r.lower[i] != c && !up) continue;
            k.scan = r.first + i;
            k.shift = up;
            const wchar_t lo = r.lower[i];
            if (lo >= L'a' && lo <= L'z') k.vk = lo - L'a' + 'A';
            else if (lo >= L'0' && lo <= L'9') k.vk = lo;
            else if (lo == L' ') k.vk = VK_SPACE;
            else {
                switch (lo) {
                    case L'-': k.vk = VK_OEM_MINUS; break;
                    case L'=': k.vk = VK_OEM_PLUS; break;
                    case L'[': k.vk = VK_OEM_4; break;
                    case L']': k.vk = VK_OEM_6; break;
                    case L';': k.vk = VK_OEM_1; break;
                    case L'\'': k.vk = VK_OEM_7; break;
                    case L'`': k.vk = VK_OEM_3; break;
                    case L',': k.vk = VK_OEM_COMMA; break;
                    case L'.': k.vk = VK_OEM_PERIOD; break;
                    case L'/': k.vk = VK_OEM_2; break;
                    default: return false;
                }
            }
            return true;
        }
    }
    return false;
}

// Wraps the DLL's class factory to keep hold of the instance TSF creates.
struct SpyFactory : IClassFactory {
    IClassFactory* real = nullptr;
    IUnknown* last = nullptr;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        const HRESULT hr = real->CreateInstance(outer, riid, ppv);
        if (SUCCEEDED(hr) && *ppv) {
            if (last) last->Release();
            last = static_cast<IUnknown*>(*ppv);
            last->AddRef();
        }
        return hr;
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }
};

struct Harness {
    HWND top = nullptr;
    HWND edit = nullptr;
    ITfThreadMgr* tm = nullptr;
    // The service's own key sink. TSF's keystroke manager only dispatches to
    // it while this thread owns the system keyboard focus; calling the sink
    // directly lets the test run in the background without stealing focus.
    ITfKeyEventSink* keys = nullptr;
    ITfTextInputProcessorEx* tip = nullptr;

    ITfContext* context() {
        ITfDocumentMgr* dm = nullptr;
        ITfContext* ctx = nullptr;
        if (SUCCEEDED(tm->GetFocus(&dm)) && dm) {
            dm->GetTop(&ctx);
            dm->Release();
        }
        return ctx;
    }

    void setShift(bool down) {
        BYTE state[256];
        GetKeyboardState(state);
        state[VK_SHIFT] = down ? 0x80 : 0;
        state[VK_LSHIFT] = down ? 0x80 : 0;
        state[VK_CONTROL] = state[VK_MENU] = state[VK_LWIN] = state[VK_RWIN] = 0;
        state[VK_CAPITAL] = 0;
        SetKeyboardState(state);
    }

    void press(wchar_t c) {
        KeyDef k{};
        if (!keyFor(c, k)) {
            fprintf(g_log, "  no key for U+%04X\n", (unsigned)c);
            return;
        }
        setShift(k.shift);
        const bool arrow = k.vk == VK_RIGHT || k.vk == VK_LEFT;
        const LPARAM lp = 1 | ((LPARAM)k.scan << 16) | (arrow ? (1 << 24) : 0);

        ITfContext* ctx = context();
        BOOL eaten = FALSE;
        keys->OnTestKeyDown(ctx, k.vk, lp, &eaten);
        if (eaten) {
            eaten = FALSE;
            keys->OnKeyDown(ctx, k.vk, lp, &eaten);
        }
        if (ctx) ctx->Release();
        if (!eaten) {
            // The application handles the key.
            if (k.vk == VK_BACK) {
                SendMessageW(edit, WM_KEYDOWN, VK_BACK, lp);
            } else if (arrow) {
                SendMessageW(edit, WM_KEYDOWN, k.vk, lp);
            } else {
                SendMessageW(edit, WM_CHAR, c, lp);
            }
        }
        setShift(false);
        pump();
    }

    std::wstring text() {
        wchar_t buf[512] = {};
        GetWindowTextW(edit, buf, 511);
        return buf;
    }

    void clear() {
        // Move focus away and back so the service resets, then empty the box.
        SetWindowTextW(edit, L"");
        SetFocus(top);
        pump();
        SetFocus(edit);
        pump();
    }
};

static void expect(Harness& h, const wchar_t* keys, const wchar_t* want) {
    ++g_checks;
    h.clear();
    for (const wchar_t* p = keys; *p; ++p) h.press(*p);
    const std::wstring got = h.text();
    const bool ok = got == want;
    if (!ok) ++g_failed;
    fprintf(g_log, "%s  \"%s\" -> \"%s\"%s%s%s\n", ok ? "ok  " : "FAIL", esc(keys).c_str(),
            esc(got).c_str(), ok ? "" : "  (want \"", ok ? "" : esc(want).c_str(), ok ? "" : "\")");
    fflush(g_log);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 3) return 2;
    _wfopen_s(&g_log, argv[2], L"w");
    if (!g_log) return 2;
    // Which keyboard layout sits underneath the text service during the test:
    // us (default), ru, or uz (the kiritish.uz layout, where ; ' [ ] type
    // letters). On ru and uz the service must still produce US characters.
    const wchar_t* mode = argc >= 4 ? argv[3] : L"us";
    LANGID wantLang = 0x0409;
    if (lstrcmpiW(mode, L"ru") == 0) wantLang = 0x0419;
    if (lstrcmpiW(mode, L"uz") == 0) wantLang = 0x0443;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadLibraryW(L"Msftedit.dll");

    Harness h;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"UzAlphaTipHarness";
    RegisterClassW(&wc);
    h.top = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"UzAlpha TIP harness",
                            WS_POPUP, -2000, -2000, 400, 200, nullptr, nullptr, inst, nullptr);
    h.edit = CreateWindowExW(0, L"RICHEDIT50W", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE, 0, 0, 380, 160,
                             h.top, nullptr, inst, nullptr);
    if (!h.top || !h.edit) {
        fprintf(g_log, "cannot create windows (%lu)\n", GetLastError());
        return 2;
    }
    SendMessageW(h.edit, EM_SETEDITSTYLE, SES_USECTF, SES_USECTF);

    {
        // Only layouts the user already has; nothing is loaded or added.
        HKL list[64];
        const int n = GetKeyboardLayoutList(ARRAYSIZE(list), list);
        HKL pick = nullptr;
        for (int i = 0; i < n && !pick; ++i) {
            if (LOWORD((DWORD_PTR)list[i]) == wantLang) pick = list[i];
        }
        if (!pick) {
            fprintf(g_log, "no %ls keyboard layout in this user's list: skipped\n", mode);
            fprintf(g_log, "0 checks, 0 failed (skipped)\n");
            fclose(g_log);
            return 0;
        }
        ActivateKeyboardLayout(pick, 0);
    }
    fprintf(g_log, "thread keyboard layout: %08lx\n", (unsigned long)(ULONG_PTR)GetKeyboardLayout(0));

    HRESULT hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITfThreadMgr),
                                  (void**)&h.tm);
    TfClientId appId = TF_CLIENTID_NULL;
    if (SUCCEEDED(hr)) hr = h.tm->Activate(&appId);
    if (FAILED(hr)) {
        fprintf(g_log, "cannot activate TSF (%08lx)\n", hr);
        return 2;
    }

    ShowWindow(h.top, SW_SHOWNOACTIVATE);
    SetActiveWindow(h.top);
    SetFocus(h.edit);
    pump();

    ITfDocumentMgr* dm = nullptr;
    h.tm->GetFocus(&dm);
    fprintf(g_log, "focus document manager: %s\n", dm ? "yes" : "NO");
    if (dm) dm->Release();

    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) {
        fprintf(g_log, "cannot load %s (%lu)\n", esc(argv[1]).c_str(), GetLastError());
        return 2;
    }
    typedef HRESULT(STDAPICALLTYPE * GetClassObject)(REFCLSID, REFIID, void**);
    GetClassObject getClass = (GetClassObject)GetProcAddress(dll, "DllGetClassObject");
    IClassFactory* factory = nullptr;
    // The DLL itself only answers to its real class id.
    hr = getClass ? getClass(CLSID_UzAlphaTip, IID_IClassFactory, (void**)&factory) : E_FAIL;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateInstance(nullptr, __uuidof(ITfTextInputProcessorEx), (void**)&h.tip);
    }
    if (FAILED(hr)) {
        fprintf(g_log, "cannot create the text service (%08lx)\n", hr);
        return 2;
    }

    // The service is not installed, so make it known to this process only:
    // a process-local COM class object and a process-local TSF profile. TSF
    // then creates and activates the service itself, as it would after a
    // real installation, without touching the registry.
    h.tip->Release();  // TSF makes its own instance
    h.tip = nullptr;
    SpyFactory spy;
    spy.real = factory;
    DWORD classCookie = 0;
    hr = CoRegisterClassObject(CLSID_HarnessTip, &spy, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &classCookie);
    fprintf(g_log, "CoRegisterClassObject: %08lx\n", hr);

    ITfInputProcessorProfileMgr* profiles = nullptr;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          __uuidof(ITfInputProcessorProfileMgr), (void**)&profiles);
    const LANGID lang = LOWORD((DWORD_PTR)GetKeyboardLayout(0));
    if (SUCCEEDED(hr)) {
        const wchar_t name[] = L"UzAlpha test profile";
        hr = profiles->RegisterProfile(CLSID_HarnessTip, lang, GUID_HarnessProfile, name,
                                       ARRAYSIZE(name) - 1, nullptr, 0, 0, nullptr, 0, TRUE,
                                       TF_RP_LOCALPROCESS);
        fprintf(g_log, "RegisterProfile (process-local, lang %04x): %08lx\n", lang, hr);
    }
    if (SUCCEEDED(hr)) {
        hr = profiles->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, lang, CLSID_HarnessTip,
                                       GUID_HarnessProfile, nullptr,
                                       TF_IPPMF_FORPROCESS | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE);
        fprintf(g_log, "ActivateProfile: %08lx\n", hr);
    }
    pump();
    if (SUCCEEDED(hr)) {
        TF_INPUTPROCESSORPROFILE active = {};
        const HRESULT hrActive = profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active);
        fprintf(g_log, "active keyboard profile: %08lx type=%lu ours=%s\n", hrActive, active.dwProfileType,
                IsEqualCLSID(active.clsid, CLSID_HarnessTip) ? "yes" : "no");
    }
    if (profiles) {
        IEnumTfInputProcessorProfiles* en = nullptr;
        if (SUCCEEDED(profiles->EnumProfiles(lang, &en)) && en) {
            TF_INPUTPROCESSORPROFILE p = {};
            ULONG got = 0;
            while (en->Next(1, &p, &got) == S_OK && got == 1) {
                fprintf(g_log, "  profile type=%lu ours=%s keyboardcat=%s flags=%08lx caps=%08lx hkl=%08lx\n",
                        p.dwProfileType, IsEqualCLSID(p.clsid, CLSID_HarnessTip) ? "yes" : "no",
                        IsEqualGUID(p.catid, GUID_TFCAT_TIP_KEYBOARD) ? "yes" : "no", p.dwFlags, p.dwCaps,
                        (unsigned long)(ULONG_PTR)p.hkl);
            }
            en->Release();
        }
    }
    if (SUCCEEDED(hr)) {
        hr = spy.last ? spy.last->QueryInterface(__uuidof(ITfKeyEventSink), (void**)&h.keys) : E_FAIL;
    }
    if (FAILED(hr)) {
        fprintf(g_log, "TSF did not create the text service (%08lx)\n", hr);
        fclose(g_log);
        return 2;
    }
    {
        ITfKeystrokeMgr* km = nullptr;
        CLSID fg = {};
        if (SUCCEEDED(h.tm->QueryInterface(__uuidof(ITfKeystrokeMgr), (void**)&km))) {
            km->GetForeground(&fg);
            km->Release();
        }
        fprintf(g_log, "text service created by TSF; foreground key sink is ours: %s\n",
                IsEqualCLSID(fg, CLSID_HarnessTip) ? "yes" : "no");
    }

    // The same expectations as tests/engine_test.cpp, through real TSF.
    expect(h, L"shahar", L"şahar");
    expect(h, L"choy", L"çoy");
    expect(h, L"o'zbek", L"özbek");
    expect(h, L"g'isht", L"ğişt");
    expect(h, L"o`zbek to`g`ri", L"özbek töğri");
    expect(h, L"o`\b a`", L"o` a`");
    expect(h, L"bosh ", L"boş ");
    expect(h, L"tog'", L"toğ");
    expect(h, L"to'g'ri", L"töğri");
    expect(h, L"mashhur", L"maşhur");
    expect(h, L"Shahar", L"Şahar");
    expect(h, L"SHAHAR", L"ŞAHAR");
    expect(h, L"O'zbek", L"Özbek");
    expect(h, L"s", L"s");
    expect(h, L"sa", L"sa");
    expect(h, L"ss", L"ss");
    expect(h, L"ma'no", L"maʼno");
    expect(h, L"mo''jiza", L"möʼjiza");
    expect(h, L"is'hoq", L"isʼhoq");
    expect(h, L"'", L"'");
    expect(h, L"1'", L"1'");
    expect(h, L"sh\b", L"sh");
    expect(h, L"Sh\b", L"Sh");
    expect(h, L"o'\b", L"o'");
    expect(h, L"ch\b\b", L"c");
    expect(h, L"s\b", L"");
    expect(h, L"as\b", L"a");
    expect(h, L"ab\b", L"a");
    expect(h, L"sha\b", L"ş");
    expect(h, L"salom; [dunyo] 'x'", L"salom; [dunyo] 'xʼ");
    // The apostrophe rule follows the real text before the caret, not the
    // last key typed: go back (Left arrow = \x02) into "mano" and add it.
    expect(h, L"mano \x02\x02\x02'", L"maʼno ");
    expect(h, L"ma \x02\x02\x02'", L"'ma ");
    expect(h, L"Yangi o'zbek alifbosi uchun", L"Yangi özbek alifbosi uçun");

    fprintf(g_log, "%d checks, %d failed\n", g_checks, g_failed);

    profiles->DeactivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, lang, CLSID_HarnessTip, GUID_HarnessProfile,
                                nullptr, TF_IPPMF_FORPROCESS);
    profiles->UnregisterProfile(CLSID_HarnessTip, lang, GUID_HarnessProfile, TF_URP_LOCALPROCESS);
    profiles->Release();
    pump();
    h.keys->Release();
    if (spy.last) spy.last->Release();
    CoRevokeClassObject(classCookie);
    factory->Release();
    DestroyWindow(h.top);
    pump();
    h.tm->Deactivate();
    h.tm->Release();
    fclose(g_log);
    CoUninitialize();
    return g_failed ? 1 : 0;
}
