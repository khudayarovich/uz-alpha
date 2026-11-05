// Installing and removing the two keyboards.
//
// Machine-level work (files in System32 / Program Files, HKLM registry, TSF
// registration) runs in an elevated copy of this program started with
// /install-machine or /uninstall-machine. Per-user work (adding the keyboards
// to the user's own list, autostart) runs in the normal instance, so it lands
// in the right user profile even when another account approved the UAC prompt.
#include "app.h"

#include <msctf.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <set>
#include <vector>

namespace {

const wchar_t kLayoutsKey[] = L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts";
const wchar_t kUninstallParent[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kClsidParent[] = L"SOFTWARE\\Classes\\CLSID";
const wchar_t kTipParent[] = L"SOFTWARE\\Microsoft\\CTF\\TIP";

// ---------------------------------------------------------------------------
// Registry helpers. `view` is 0, KEY_WOW64_64KEY or KEY_WOW64_32KEY.

bool SetRegString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value,
                  REGSAM view = 0, DWORD type = REG_SZ) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | view, nullptr, &h, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LONG r = RegSetValueExW(h, name, 0, type, (const BYTE*)value.c_str(),
                                  (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

bool SetRegDword(HKEY root, const std::wstring& key, const wchar_t* name, DWORD value) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG r = RegSetValueExW(h, name, 0, REG_DWORD, (const BYTE*)&value, sizeof value);
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

bool GetRegString(HKEY root, const std::wstring& key, const wchar_t* name, std::wstring* out, REGSAM view = 0) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE | view, &h) != ERROR_SUCCESS) return false;
    wchar_t buf[1024] = {};
    DWORD size = sizeof buf - sizeof(wchar_t);
    DWORD type = 0;
    const LONG r = RegQueryValueExW(h, name, nullptr, &type, (BYTE*)buf, &size);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    if (out) *out = buf;
    return true;
}

void DeleteRegTree(HKEY root, const wchar_t* parent, const std::wstring& child, REGSAM view = 0) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, parent, 0, DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE | view,
                      &h) != ERROR_SUCCESS) {
        return;
    }
    RegDeleteTreeW(h, child.c_str());
    RegCloseKey(h);
}

// ---------------------------------------------------------------------------
// Paths and files

std::wstring SystemDir(bool wow64) {
    wchar_t buf[MAX_PATH] = {};
    if (wow64) {
        GetSystemWow64DirectoryW(buf, MAX_PATH);
    } else {
        GetSystemDirectoryW(buf, MAX_PATH);
    }
    return buf;
}

std::wstring Tick() {
    wchar_t buf[32];
    swprintf_s(buf, L"%lu", GetTickCount());
    return buf;
}

// Gets an existing file out of the way. A DLL that is loaded somewhere cannot
// be deleted, but it can be renamed; the renamed copy goes at the next restart.
bool ClearPath(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    if (DeleteFileW(path.c_str())) return true;
    const std::wstring aside = path + L".old" + Tick();
    if (!MoveFileExW(path.c_str(), aside.c_str(), 0)) return false;
    MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return true;
}

// Writes an embedded resource to `path`. The file is created in place (never
// moved in from elsewhere) so it inherits the folder's permissions, which is
// what lets Store apps load the text service from Program Files.
bool ExtractResource(int id, const std::wstring& path) {
    const HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    const HGLOBAL mem = LoadResource(nullptr, res);
    const DWORD size = SizeofResource(nullptr, res);
    const void* data = mem ? LockResource(mem) : nullptr;
    if (!data || !size) return false;

    if (!ClearPath(path)) {
        Log(L"cannot replace %s (error %lu)", path.c_str(), GetLastError());
        return false;
    }
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log(L"cannot create %s (error %lu)", path.c_str(), GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(f, data, size, &written, nullptr) && written == size;
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

void RemoveFileNowOrAtRestart(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (DeleteFileW(path.c_str())) return;
    const std::wstring aside = path + L".del" + Tick();
    if (MoveFileExW(path.c_str(), aside.c_str(), 0)) {
        MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    } else {
        MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
}

void RemoveDirNowOrAtRestart(const std::wstring& path) {
    if (!RemoveDirectoryW(path.c_str())) MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

// ---------------------------------------------------------------------------
// The Manual keyboard layout in the registry

struct LayoutEntry {
    std::wstring klid;
    std::wstring file;
    std::wstring id;
};

std::vector<LayoutEntry> EnumLayouts() {
    std::vector<LayoutEntry> all;
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kLayoutsKey, 0, KEY_READ, &h) != ERROR_SUCCESS) return all;
    for (DWORD i = 0;; ++i) {
        wchar_t name[64];
        DWORD len = ARRAYSIZE(name);
        if (RegEnumKeyExW(h, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        LayoutEntry e;
        e.klid = name;
        const std::wstring key = std::wstring(kLayoutsKey) + L"\\" + name;
        GetRegString(HKEY_LOCAL_MACHINE, key, L"Layout File", &e.file);
        GetRegString(HKEY_LOCAL_MACHINE, key, L"Layout Id", &e.id);
        all.push_back(e);
    }
    RegCloseKey(h);
    return all;
}

// Registers kbduzalp.dll as a keyboard layout. The registry key name (KLID)
// and the "Layout Id" must not collide with any other layout on the machine
// (kiritish.uz's own layout, for one), so both are chosen by scanning.
bool RegisterLayout() {
    std::wstring klid, id;
    if (!FindManualLayout(&klid, nullptr)) {
        std::set<std::wstring> names;
        std::set<DWORD> ids;
        for (const LayoutEntry& e : EnumLayouts()) {
            std::wstring lower = e.klid;
            CharLowerW(&lower[0]);
            names.insert(lower);
            if (!e.id.empty()) ids.insert(wcstoul(e.id.c_str(), nullptr, 16));
        }
        DWORD newId = 0x00c1;
        while (ids.count(newId) && newId < 0x0fff) ++newId;
        wchar_t buf[16] = {};
        for (unsigned n = 1; n <= 0xfff; ++n) {
            swprintf_s(buf, L"a%03x%04x", n, (unsigned)UZA_LANGID);
            if (!names.count(buf)) break;
        }
        klid = buf;
        swprintf_s(buf, L"%04lx", newId);
        id = buf;
    }

    const std::wstring key = std::wstring(kLayoutsKey) + L"\\" + klid;
    bool ok = SetRegString(HKEY_LOCAL_MACHINE, key, L"Layout File", UZA_KBD_DLL);
    ok = ok && SetRegString(HKEY_LOCAL_MACHINE, key, L"Layout Text", UZA_MANUAL_NAME);
    if (!id.empty()) ok = ok && SetRegString(HKEY_LOCAL_MACHINE, key, L"Layout Id", id);
    ok = ok && SetRegString(HKEY_LOCAL_MACHINE, key, L"Layout Display Name",
                            L"@%SystemRoot%\\system32\\" UZA_KBD_DLL L",-1000", 0, REG_EXPAND_SZ);
    Log(L"layout registered as %s: %s", klid.c_str(), ok ? L"ok" : L"FAILED");
    return ok;
}

// ---------------------------------------------------------------------------
// The Auto text service in TSF

const GUID* const kCategories[] = {
    &GUID_TFCAT_TIP_KEYBOARD,
    &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    &GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    &GUID_TFCAT_TIPCAP_COMLESS,
    &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,  // works in Store / full-screen apps
    &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,    // shown in the taskbar input switcher
};

HRESULT RegisterTextService() {
    ITfInputProcessorProfileMgr* profiles = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(ITfInputProcessorProfileMgr), (void**)&profiles);
    if (FAILED(hr)) return hr;
    const wchar_t name[] = UZA_AUTO_NAME;
    // bEnabledByDefault must be TRUE. Registered as FALSE and then enabled per
    // user (InstallLayoutOrTip writes Enable=1 under HKCU), the profile showed
    // up in the keyboard list but Windows 11 still reported it as disabled and
    // every activation silently fell back to a plain layout. TRUE only makes
    // the profile available; it appears in a user's list once that user adds it.
    // No icon: the taskbar language indicator then shows the language code
    // for Auto, the same as for an ordinary keyboard layout, instead of a logo.
    hr = profiles->RegisterProfile(CLSID_UzAlphaTip, UZA_LANGID, GUID_UzAlphaProfile, name,
                                   ARRAYSIZE(name) - 1, nullptr, 0, 0,
                                   nullptr /* no substitute layout */, 0, TRUE /* enabled by default */, 0);
    profiles->Release();
    if (FAILED(hr)) return hr;
    // An update over an older registration keeps its old values: set them outright.
    const std::wstring profileKey = std::wstring(kTipParent) + L"\\" UZA_CLSID_STR L"\\LanguageProfile\\0x0000" UZA_LANGID_STR
                                    L"\\" UZA_PROFILE_STR;
    SetRegDword(HKEY_LOCAL_MACHINE, profileKey, L"Enable", 1);
    SetRegString(HKEY_LOCAL_MACHINE, profileKey, L"Description", UZA_AUTO_NAME);
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, profileKey.c_str(), 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
        RegDeleteValueW(h, L"IconFile");
        RegDeleteValueW(h, L"IconIndex");
        RegCloseKey(h);
    }

    ITfCategoryMgr* categories = nullptr;
    hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITfCategoryMgr),
                          (void**)&categories);
    if (FAILED(hr)) return hr;
    for (const GUID* cat : kCategories) {
        const HRESULT hrCat = categories->RegisterCategory(CLSID_UzAlphaTip, *cat, CLSID_UzAlphaTip);
        if (FAILED(hrCat)) hr = hrCat;
    }
    categories->Release();
    return hr;
}

void UnregisterTextService() {
    ITfCategoryMgr* categories = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(ITfCategoryMgr), (void**)&categories))) {
        for (const GUID* cat : kCategories) {
            categories->UnregisterCategory(CLSID_UzAlphaTip, *cat, CLSID_UzAlphaTip);
        }
        categories->Release();
    }
    ITfInputProcessorProfileMgr* profiles = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(ITfInputProcessorProfileMgr), (void**)&profiles))) {
        profiles->UnregisterProfile(CLSID_UzAlphaTip, UZA_LANGID, GUID_UzAlphaProfile, 0);
        profiles->Release();
    }
}

// ---------------------------------------------------------------------------
// The current user's keyboard list (input.dll)

struct LAYOUTORTIPPROFILE {
    DWORD dwProfileType;
    LANGID langid;
    CLSID clsid;
    GUID guidProfile;
    GUID catid;
    DWORD dwSubstituteLayout;
    DWORD dwFlags;
    WCHAR szId[MAX_PATH];
};
const DWORD ILOT_UNINSTALL = 0x00000001;

HMODULE InputDll() {
    static HMODULE dll = LoadLibraryExW(L"input.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return dll;
}

bool CallInstallLayoutOrTip(const std::wstring& list, DWORD flags) {
    typedef BOOL(WINAPI * Fn)(LPCWSTR, DWORD);
    const Fn fn = InputDll() ? (Fn)GetProcAddress(InputDll(), "InstallLayoutOrTip") : nullptr;
    if (!fn) return false;
    const BOOL ok = fn(list.c_str(), flags);
    Log(L"InstallLayoutOrTip(\"%s\", %lx) -> %d", list.c_str(), flags, ok);
    return ok != FALSE;
}

// Everything currently enabled for the user, as "langid:id" strings.
std::set<std::wstring> EnabledInputs() {
    std::set<std::wstring> ids;
    typedef UINT(WINAPI * Fn)(LPCWSTR, LPCWSTR, LPCWSTR, LAYOUTORTIPPROFILE*, UINT);
    const Fn fn = InputDll() ? (Fn)GetProcAddress(InputDll(), "EnumEnabledLayoutOrTip") : nullptr;
    if (!fn) return ids;
    std::vector<LAYOUTORTIPPROFILE> buf(64);
    const UINT n = fn(nullptr, nullptr, nullptr, buf.data(), (UINT)buf.size());
    for (UINT i = 0; i < n && i < buf.size(); ++i) {
        std::wstring id = buf[i].szId;
        if (id.empty()) continue;
        CharLowerW(&id[0]);
        ids.insert(id);
    }
    return ids;
}

// "0443:{clsid}{profile};0443:a0010443" - the form Windows itself uses for
// input ids. `prefixed` gives the 0x-prefixed form shown in the API docs.
// Per-user switch for the Auto profile. InstallLayoutOrTip sets it as well;
// calling it directly also makes TSF refresh its view of the profile.
void EnableTextServiceProfile(bool enable) {
    ITfInputProcessorProfiles* profiles = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(ITfInputProcessorProfiles), (void**)&profiles)) ||
        !profiles) {
        Log(L"EnableLanguageProfile: cannot reach TSF");
        return;
    }
    const HRESULT hr = profiles->EnableLanguageProfile(CLSID_UzAlphaTip, UZA_LANGID, GUID_UzAlphaProfile,
                                                       enable ? TRUE : FALSE);
    Log(L"EnableLanguageProfile(%d) -> %08lx", (int)enable, hr);
    profiles->Release();
}

std::wstring UserInputList(bool prefixed) {
    const std::wstring x = prefixed ? L"0x" : L"";
    std::wstring klid;
    std::wstring list = x + UZA_LANGID_STR L":" UZA_CLSID_STR UZA_PROFILE_STR;
    if (FindManualLayout(&klid, nullptr)) list += L";" + x + UZA_LANGID_STR L":" + x + klid;
    return list;
}

// ---------------------------------------------------------------------------
// Elevation

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof elevation;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// Runs a copy of this program elevated and waits for it, keeping our window
// painting.
bool RunElevated(HWND owner, const std::wstring& exe, const wchar_t* args, DWORD* exitCode, bool* cancelled) {
    *cancelled = false;
    // Already elevated (a scripted install from an administrator prompt, or a
    // build machine): do our own machine-level work right here.
    if (IsElevated() && lstrcmpiW(exe.c_str(), SelfPath().c_str()) == 0) {
        if (lstrcmpW(args, L"/install-machine") == 0) {
            *exitCode = (DWORD)MachineInstall();
            return true;
        }
        if (lstrcmpW(args, L"/uninstall-machine") == 0) {
            *exitCode = (DWORD)MachineUninstall();
            return true;
        }
    }
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        *cancelled = GetLastError() == ERROR_CANCELLED;
        Log(L"elevation for %s failed (error %lu)", args, GetLastError());
        return false;
    }
    for (;;) {
        const DWORD w = MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) break;
        if (w != WAIT_OBJECT_0 + 1) break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage((int)msg.wParam);
                CloseHandle(sei.hProcess);
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    *exitCode = code;
    Log(L"elevated %s finished with code %lu", args, code);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------

void Log(const wchar_t* fmt, ...) {
    wchar_t dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring path = std::wstring(dir) + L"\\" UZA_INSTALL_DIRNAME;
    CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\uzalpha.log";

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(f, L"%04u-%02u-%02u %02u:%02u:%02u  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, fmt);
    vfwprintf(f, fmt, args);
    va_end(args);
    fputwc(L'\n', f);
    fclose(f);
}

std::wstring SelfPath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

std::wstring InstallDir() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &base)) && base) {
        dir = std::wstring(base) + L"\\" UZA_INSTALL_DIRNAME;
    }
    if (base) CoTaskMemFree(base);
    return dir;
}

std::wstring InstalledExePath() { return InstallDir() + L"\\" UZA_APP_EXE; }

bool FindManualLayout(std::wstring* klid, DWORD* hkl) {
    for (const LayoutEntry& e : EnumLayouts()) {
        if (_wcsicmp(e.file.c_str(), UZA_KBD_DLL) != 0) continue;
        if (klid) *klid = e.klid;
        if (hkl) {
            // A layout with a "Layout Id" gets the handle F<id><langid>.
            const DWORD id = wcstoul(e.id.c_str(), nullptr, 16);
            *hkl = 0xF0000000u | (id << 16) | UZA_LANGID;
        }
        return true;
    }
    return false;
}

bool IsInstalled() {
    std::wstring dll;
    const std::wstring key = std::wstring(kClsidParent) + L"\\" UZA_CLSID_STR L"\\InprocServer32";
    if (!GetRegString(HKEY_LOCAL_MACHINE, key, nullptr, &dll, KEY_WOW64_64KEY)) return false;
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    return FindManualLayout(nullptr, nullptr);
}

int MachineInstall() {
    Log(L"machine install: start (version %s)", UZA_VERSION_W);
    const std::wstring dir = InstallDir();
    if (dir.empty()) return 10;
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\x64").c_str(), nullptr);
    CreateDirectoryW((dir + L"\\x86").c_str(), nullptr);

    const std::wstring tip64 = dir + L"\\x64\\" UZA_TIP_DLL;
    const std::wstring tip32 = dir + L"\\x86\\" UZA_TIP_DLL;
    if (!ExtractResource(IDR_TIP_X64, tip64)) return 11;
    if (!ExtractResource(IDR_TIP_X86, tip32)) return 12;
    if (!ExtractResource(IDR_KBD_X64, SystemDir(false) + L"\\" UZA_KBD_DLL)) return 13;
    if (!ExtractResource(IDR_KBD_WOW64, SystemDir(true) + L"\\" UZA_KBD_DLL)) return 14;

    const std::wstring self = SelfPath();
    const std::wstring exe = InstalledExePath();
    if (_wcsicmp(self.c_str(), exe.c_str()) != 0) {
        if (!ClearPath(exe) || !CopyFileW(self.c_str(), exe.c_str(), FALSE)) {
            Log(L"cannot copy the app to %s (error %lu)", exe.c_str(), GetLastError());
            return 15;
        }
    }

    // COM class of the text service: the 64-bit and the 32-bit registry views
    // point at the matching DLL.
    const std::wstring clsidKey = std::wstring(kClsidParent) + L"\\" UZA_CLSID_STR;
    bool ok = true;
    for (int pass = 0; pass < 2; ++pass) {
        const REGSAM view = pass == 0 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
        const std::wstring& dll = pass == 0 ? tip64 : tip32;
        ok = ok && SetRegString(HKEY_LOCAL_MACHINE, clsidKey, nullptr, UZA_BRAND L" Text Service", view);
        ok = ok && SetRegString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", nullptr, dll, view);
        ok = ok && SetRegString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", L"ThreadingModel",
                                L"Apartment", view);
    }
    if (!ok) return 16;

    if (!RegisterLayout()) return 17;

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT hr = RegisterTextService();
    if (SUCCEEDED(hrCom)) CoUninitialize();
    Log(L"text service registration: %08lx", hr);
    if (FAILED(hr)) return 18;

    // Apps & features entry.
    const std::wstring un = std::wstring(kUninstallParent) + L"\\" UZA_INSTALL_DIRNAME;
    SetRegString(HKEY_LOCAL_MACHINE, un, L"DisplayName", UZA_BRAND);
    SetRegString(HKEY_LOCAL_MACHINE, un, L"DisplayVersion", UZA_VERSION_W);
    SetRegString(HKEY_LOCAL_MACHINE, un, L"Publisher", UZA_DEVELOPER);
    SetRegString(HKEY_LOCAL_MACHINE, un, L"DisplayIcon", exe);
    SetRegString(HKEY_LOCAL_MACHINE, un, L"InstallLocation", dir);
    SetRegString(HKEY_LOCAL_MACHINE, un, L"UninstallString", L"\"" + exe + L"\" /uninstall");
    SetRegDword(HKEY_LOCAL_MACHINE, un, L"NoModify", 1);
    SetRegDword(HKEY_LOCAL_MACHINE, un, L"NoRepair", 1);
    SetRegDword(HKEY_LOCAL_MACHINE, un, L"EstimatedSize", 1024);

    Log(L"machine install: done");
    return 0;
}

int MachineUninstall() {
    Log(L"machine uninstall: start");
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    UnregisterTextService();
    if (SUCCEEDED(hrCom)) CoUninitialize();

    DeleteRegTree(HKEY_LOCAL_MACHINE, kTipParent, UZA_CLSID_STR);
    DeleteRegTree(HKEY_LOCAL_MACHINE, kClsidParent, UZA_CLSID_STR, KEY_WOW64_64KEY);
    DeleteRegTree(HKEY_LOCAL_MACHINE, kClsidParent, UZA_CLSID_STR, KEY_WOW64_32KEY);

    std::wstring klid;
    if (FindManualLayout(&klid, nullptr)) DeleteRegTree(HKEY_LOCAL_MACHINE, kLayoutsKey, klid);
    DeleteRegTree(HKEY_LOCAL_MACHINE, kUninstallParent, UZA_INSTALL_DIRNAME);

    const std::wstring dir = InstallDir();
    RemoveFileNowOrAtRestart(SystemDir(false) + L"\\" UZA_KBD_DLL);
    RemoveFileNowOrAtRestart(SystemDir(true) + L"\\" UZA_KBD_DLL);
    if (!dir.empty()) {
        RemoveFileNowOrAtRestart(dir + L"\\x64\\" UZA_TIP_DLL);
        RemoveFileNowOrAtRestart(dir + L"\\x86\\" UZA_TIP_DLL);
        RemoveFileNowOrAtRestart(InstalledExePath());
        RemoveDirNowOrAtRestart(dir + L"\\x64");
        RemoveDirNowOrAtRestart(dir + L"\\x86");
        RemoveDirNowOrAtRestart(dir);
    }
    Log(L"machine uninstall: done");
    return 0;
}

bool EnableForUser() {
    if (!IsInstalled()) return false;
    // InstallLayoutOrTip should only add, but make sure nothing the user
    // already had (the kiritish.uz layout, for one) drops out of the list.
    const std::set<std::wstring> before = EnabledInputs();
    const bool ok = CallInstallLayoutOrTip(UserInputList(false), 0) || CallInstallLayoutOrTip(UserInputList(true), 0);
    EnableTextServiceProfile(true);
    const std::set<std::wstring> after = EnabledInputs();
    for (const std::wstring& id : before) {
        if (!after.count(id)) {
            Log(L"input %s disappeared, adding it back", id.c_str());
            CallInstallLayoutOrTip(id, 0);
        }
    }
    return ok;
}

void DisableForUser() {
    EnableTextServiceProfile(false);
    if (!CallInstallLayoutOrTip(UserInputList(false), ILOT_UNINSTALL)) {
        CallInstallLayoutOrTip(UserInputList(true), ILOT_UNINSTALL);
    }
}

bool GetAutostart() { return GetRegString(HKEY_CURRENT_USER, kRunKey, UZA_BRAND, nullptr); }

void SetAutostart(bool on) {
    if (on) {
        // Always the installed copy, so a download folder can be cleaned up.
        const std::wstring exe = IsInstalled() ? InstalledExePath() : SelfPath();
        SetRegString(HKEY_CURRENT_USER, kRunKey, UZA_BRAND, L"\"" + exe + L"\" /tray");
    } else {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
            RegDeleteValueW(h, UZA_BRAND);
            RegCloseKey(h);
        }
    }
}

bool InstallFrom(HWND owner, const std::wstring& exe, std::wstring* error) {
    error->clear();
    DWORD code = 0;
    bool cancelled = false;
    if (!RunElevated(owner, exe, L"/install-machine", &code, &cancelled)) {
        if (!cancelled) *error = L"Örnatuvçini işga tuşirib bölmadi.";
        return false;
    }
    if (code != 0) {
        wchar_t buf[128];
        swprintf_s(buf, L"Örnatişda xatolik yuz berdi (kod %lu).", code);
        *error = buf;
        return false;
    }
    if (!EnableForUser()) Log(L"could not add the keyboards to the user's list");
    return true;
}

bool InstallEverything(HWND owner, std::wstring* error) {
    if (!InstallFrom(owner, SelfPath(), error)) return false;
    SetAutostart(true);
    return true;
}

bool UninstallEverything(HWND owner, std::wstring* error) {
    error->clear();
    // While the machine-level registration still exists, take the keyboards
    // out of this user's list.
    DisableForUser();
    SetAutostart(false);

    DWORD code = 0;
    bool cancelled = false;
    if (!RunElevated(owner, SelfPath(), L"/uninstall-machine", &code, &cancelled)) {
        if (!cancelled) *error = L"Öçiriş dasturini işga tuşirib bölmadi.";
        // Declined: put the user's keyboards back.
        EnableForUser();
        SetAutostart(true);
        return false;
    }
    if (code != 0) {
        wchar_t buf[128];
        swprintf_s(buf, L"Öçirişda xatolik yuz berdi (kod %lu).", code);
        *error = buf;
        return false;
    }
    return true;
}
