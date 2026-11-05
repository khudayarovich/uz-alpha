// Installing and removing the two keyboards.
//
// Both keyboards are language profiles of one TSF text service (UzAlphaTip.dll).
// Machine-level work (files in Program Files, HKLM registry, TSF registration,
// shortcuts) runs in an elevated copy of this program started with
// /install-machine, /uninstall-machine or /apply-update. Per-user work (adding
// the keyboards to the user's own list, autostart) runs in the normal instance,
// so it lands in the right user profile even when another account approved the
// UAC prompt.
#include "app.h"

#include <msctf.h>
#include <shellapi.h>
#include <shlobj.h>

#include <share.h>

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
const wchar_t kShortcutName[] = L"\\" UZA_BRAND L".lnk";

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

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) path = p;
    if (p) CoTaskMemFree(p);
    return path;
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
// Version 1.0.0 installed Manual as a keyboard layout DLL (kbduzalp.dll). It
// is a mode of the text service now; these remove what 1.0.0 left behind.

std::wstring LayoutFile(const std::wstring& klid) {
    std::wstring file;
    GetRegString(HKEY_LOCAL_MACHINE, std::wstring(kLayoutsKey) + L"\\" + klid, L"Layout File", &file);
    return file;
}

void RemoveLegacyLayoutFromMachine() {
    HKEY h = nullptr;
    std::vector<std::wstring> ours;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kLayoutsKey, 0, KEY_READ, &h) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            wchar_t name[64];
            DWORD len = ARRAYSIZE(name);
            if (RegEnumKeyExW(h, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            if (lstrcmpiW(LayoutFile(name).c_str(), UZA_LEGACY_KBD_DLL) == 0) ours.push_back(name);
        }
        RegCloseKey(h);
    }
    for (const std::wstring& klid : ours) {
        DeleteRegTree(HKEY_LOCAL_MACHINE, kLayoutsKey, klid);
        Log(L"removed the old layout registration %s", klid.c_str());
    }
    RemoveFileNowOrAtRestart(SystemDir(false) + L"\\" UZA_LEGACY_KBD_DLL);
    RemoveFileNowOrAtRestart(SystemDir(true) + L"\\" UZA_LEGACY_KBD_DLL);
}

// ---------------------------------------------------------------------------
// The text service in TSF

const GUID* const kCategories[] = {
    &GUID_TFCAT_TIP_KEYBOARD,
    &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    &GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    &GUID_TFCAT_TIPCAP_COMLESS,
    &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,  // works in Store / full-screen apps
    &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,    // shown in the taskbar input switcher
};

struct ProfileDef {
    const GUID* guid;
    const wchar_t* guidStr;
    const wchar_t* name;
    ULONG iconIndex;
};
const ProfileDef kProfiles[] = {
    {&GUID_UzAlphaProfile, UZA_PROFILE_STR, UZA_AUTO_NAME, UZA_ICON_AUTO_INDEX},
    {&GUID_UzAlphaManualProfile, UZA_MANUAL_PROFILE_STR, UZA_MANUAL_NAME, UZA_ICON_MANUAL_INDEX},
};

HRESULT RegisterTextService(const std::wstring& iconFile) {
    ITfInputProcessorProfileMgr* profiles = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(ITfInputProcessorProfileMgr), (void**)&profiles);
    if (FAILED(hr)) return hr;
    for (const ProfileDef& p : kProfiles) {
        // bEnabledByDefault must be TRUE. Registered as FALSE and then enabled
        // per user, the profile showed up in the keyboard list but Windows 11
        // still reported it as disabled and every activation silently fell
        // back to a plain layout. TRUE only makes the profile available; it
        // appears in a user's list once that user adds it.
        //
        // The icon is just the letters UZA / UZM: Windows draws its own text
        // code (UZB) only for plain layouts, so this is how each keyboard gets
        // a code of its own in the switcher.
        hr = profiles->RegisterProfile(CLSID_UzAlphaTip, UZA_LANGID, *p.guid, p.name, (ULONG)lstrlenW(p.name),
                                       iconFile.c_str(), (ULONG)iconFile.size(), p.iconIndex,
                                       nullptr /* no substitute layout */, 0, TRUE /* enabled by default */, 0);
        if (FAILED(hr)) break;
        // An update over an older registration keeps its old values: set them outright.
        const std::wstring key = std::wstring(kTipParent) + L"\\" UZA_CLSID_STR L"\\LanguageProfile\\0x0000" UZA_LANGID_STR
                                 L"\\" + p.guidStr;
        SetRegDword(HKEY_LOCAL_MACHINE, key, L"Enable", 1);
        SetRegString(HKEY_LOCAL_MACHINE, key, L"Description", p.name);
        SetRegString(HKEY_LOCAL_MACHINE, key, L"IconFile", iconFile);
        SetRegDword(HKEY_LOCAL_MACHINE, key, L"IconIndex", p.iconIndex);
    }
    profiles->Release();
    if (FAILED(hr)) return hr;

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
        for (const ProfileDef& p : kProfiles) {
            profiles->UnregisterProfile(CLSID_UzAlphaTip, UZA_LANGID, *p.guid, 0);
        }
        profiles->Release();
    }
}

// ---------------------------------------------------------------------------
// Start menu and desktop shortcuts (for all users), so the app can be found
// with Windows Search.

bool CreateShortcut(const std::wstring& lnk, const std::wstring& target) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link)) ||
        !link) {
        return false;
    }
    link->SetPath(target.c_str());
    link->SetDescription(UZA_TAGLINE);
    link->SetIconLocation(target.c_str(), 0);
    const std::wstring dir = target.substr(0, target.find_last_of(L'\\'));
    link->SetWorkingDirectory(dir.c_str());
    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file)) && file) {
        ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

void CreateShortcuts(const std::wstring& exe) {
    for (const KNOWNFOLDERID* id : {&FOLDERID_CommonPrograms, &FOLDERID_PublicDesktop}) {
        const std::wstring folder = KnownFolder(*id);
        if (folder.empty()) continue;
        const bool ok = CreateShortcut(folder + kShortcutName, exe);
        Log(L"shortcut in %s: %s", folder.c_str(), ok ? L"ok" : L"FAILED");
    }
}

void RemoveShortcuts() {
    for (const KNOWNFOLDERID* id : {&FOLDERID_CommonPrograms, &FOLDERID_PublicDesktop}) {
        const std::wstring folder = KnownFolder(*id);
        if (!folder.empty()) DeleteFileW((folder + kShortcutName).c_str());
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

// Everything currently enabled for the user, as lower-case "langid:id" strings.
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

// "0443:{clsid}{profile}" - the form Windows itself uses for input ids.
// `prefixed` gives the 0x-prefixed form shown in the API docs.
std::wstring InputId(const ProfileDef& p, bool prefixed) {
    return (prefixed ? L"0x" : L"") + std::wstring(UZA_LANGID_STR) + L":" UZA_CLSID_STR + p.guidStr;
}

// One call per keyboard. Given both in one list, Windows added only the
// missing one's neighbour: on an update, where Automatic was already there,
// Manual never made it into the list.
bool InstallOrRemoveForUser(DWORD flags) {
    bool all = true;
    for (const ProfileDef& p : kProfiles) {
        if (!CallInstallLayoutOrTip(InputId(p, false), flags) && !CallInstallLayoutOrTip(InputId(p, true), flags)) {
            all = false;
        }
    }
    return all;
}

// Per-user switch for the profiles. InstallLayoutOrTip sets it as well;
// calling it directly also makes TSF refresh its view of them.
void EnableTextServiceProfiles(bool enable) {
    ITfInputProcessorProfiles* profiles = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(ITfInputProcessorProfiles), (void**)&profiles)) ||
        !profiles) {
        Log(L"EnableLanguageProfile: cannot reach TSF");
        return;
    }
    for (const ProfileDef& p : kProfiles) {
        const HRESULT hr = profiles->EnableLanguageProfile(CLSID_UzAlphaTip, UZA_LANGID, *p.guid, enable ? TRUE : FALSE);
        Log(L"EnableLanguageProfile(%s, %d) -> %08lx", p.name, (int)enable, hr);
    }
    profiles->Release();
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof elevation;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
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

    // Shared on purpose: _wfopen_s opens for writing exclusively, so a line was
    // silently lost whenever anything else had the log open, even just to read.
    FILE* f = _wfsopen(path.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    if (!f) return;
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
    const std::wstring base = KnownFolder(FOLDERID_ProgramFiles);
    return base.empty() ? base : base + L"\\" UZA_INSTALL_DIRNAME;
}

std::wstring InstalledExePath() { return InstallDir() + L"\\" UZA_APP_EXE; }

bool IsInstalled() {
    std::wstring dll;
    const std::wstring key = std::wstring(kClsidParent) + L"\\" UZA_CLSID_STR L"\\InprocServer32";
    if (!GetRegString(HKEY_LOCAL_MACHINE, key, nullptr, &dll, KEY_WOW64_64KEY)) return false;
    return GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool RunSelfElevated(HWND owner, const std::wstring& args, DWORD* exitCode, bool* cancelled) {
    *cancelled = false;
    // Already elevated (a scripted install from an administrator prompt, or a
    // build machine): do the machine-level work right here.
    if (IsElevated()) {
        if (args == L"/install-machine") {
            *exitCode = (DWORD)MachineInstall();
            return true;
        }
        if (args == L"/uninstall-machine") {
            *exitCode = (DWORD)MachineUninstall();
            return true;
        }
    }
    const std::wstring self = SelfPath();
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = self.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        const DWORD err = GetLastError();
        *cancelled = err == ERROR_CANCELLED;
        Log(L"elevation for \"%s\" failed (error %lu)", args.c_str(), err);
        return false;
    }
    // Wait for it, keeping our window painting.
    for (;;) {
        const DWORD w = MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT);
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
    Log(L"elevated \"%s\" finished with code %lu", args.c_str(), code);
    return true;
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

    RemoveLegacyLayoutFromMachine();

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT hr = RegisterTextService(tip64);
    Log(L"text service registration: %08lx", hr);
    if (SUCCEEDED(hr)) CreateShortcuts(exe);
    if (SUCCEEDED(hrCom)) CoUninitialize();
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
    RemoveLegacyLayoutFromMachine();
    DeleteRegTree(HKEY_LOCAL_MACHINE, kUninstallParent, UZA_INSTALL_DIRNAME);
    RemoveShortcuts();

    const std::wstring dir = InstallDir();
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

// Runs elevated, in the copy that is already installed and trusted:
// UzAlpha.exe /apply-update "<downloaded exe>" <sha256>
// Checks the download once more (it sat in the user's profile), copies it into
// Program Files and lets that copy install itself.
int ApplyUpdateElevated(const std::wstring& file, const std::wstring& sha256) {
    Log(L"apply update: %s", file.c_str());
    if (FileSha256(file) != sha256) return 31;
    const std::wstring dir = InstallDir();
    if (dir.empty()) return 30;
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring staged = dir + L"\\" UZA_BRAND L".new.exe";
    if (!ClearPath(staged) || !CopyFileW(file.c_str(), staged.c_str(), FALSE)) return 32;
    if (FileSha256(staged) != sha256) {
        DeleteFileW(staged.c_str());
        return 33;
    }

    std::wstring cmd = L"\"" + staged + L"\" /install-machine";
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(staged.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        Log(L"apply update: cannot start the new version (error %lu)", GetLastError());
        DeleteFileW(staged.c_str());
        return 34;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 35;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    RemoveFileNowOrAtRestart(staged);
    Log(L"apply update: the new version's installer finished with code %lu", code);
    return (int)code;
}

// Takes out of the user's list what an older UzAlpha put there and no longer
// provides: the Manual keyboard layout of version 1.0.0 (Manual is a text
// service profile now). Recognised by its DLL, or - once that registration is
// gone - by pointing at a layout that no longer exists.
void CleanUserList() {
    const std::wstring prefix = L"" UZA_LANGID_STR L":";
    for (const std::wstring& id : EnabledInputs()) {
        // "0443:a0010443": a custom layout under Uzbek
        if (id.size() != 13 || id.compare(0, 5, prefix) != 0 || id[5] != L'a') continue;
        const std::wstring klid = id.substr(5);
        const std::wstring file = LayoutFile(klid);
        if (file.empty() || lstrcmpiW(file.c_str(), UZA_LEGACY_KBD_DLL) == 0) {
            Log(L"removing the old layout %s from the user's list", id.c_str());
            CallInstallLayoutOrTip(id, ILOT_UNINSTALL);
        }
    }
}

bool EnableForUser() {
    if (!IsInstalled()) return false;
    CleanUserList();
    // InstallLayoutOrTip should only add, but make sure nothing else the user
    // had drops out of the list.
    const std::set<std::wstring> before = EnabledInputs();
    const bool ok = InstallOrRemoveForUser(0);
    EnableTextServiceProfiles(true);
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
    EnableTextServiceProfiles(false);
    InstallOrRemoveForUser(ILOT_UNINSTALL);
    CleanUserList();
}

bool KeyboardsInUserList() {
    const std::set<std::wstring> enabled = EnabledInputs();
    for (const ProfileDef& p : kProfiles) {
        std::wstring id = InputId(p, false);
        CharLowerW(&id[0]);
        if (!enabled.count(id)) return false;
    }
    return true;
}

bool GetAutostart() { return GetRegString(HKEY_CURRENT_USER, kRunKey, UZA_BRAND, nullptr); }

namespace {
// Always the installed copy, so a download folder can be cleaned up.
std::wstring AutostartCommand() {
    return L"\"" + (IsInstalled() ? InstalledExePath() : SelfPath()) + L"\" /tray";
}
}  // namespace

void SetAutostart(bool on) {
    if (on) {
        SetRegString(HKEY_CURRENT_USER, kRunKey, UZA_BRAND, AutostartCommand());
    } else {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
            RegDeleteValueW(h, UZA_BRAND);
            RegCloseKey(h);
        }
    }
}

void ChooseAutostart(bool on) {
    SetSetting(UZA_SETTING_AUTOSTART, on);
    SetAutostart(on);
}

// The Run entry is written at the first install only, so anything that removes
// it afterwards (a cleanup tool, a restored profile, an install made by an
// older version) would leave the app silently not starting any more. The choice
// is therefore kept apart from the entry, and the entry is checked at each start.
void EnsureAutostart() {
    if (!IsInstalled() || !GetSetting(UZA_SETTING_AUTOSTART)) return;
    std::wstring have;
    const bool present = GetRegString(HKEY_CURRENT_USER, kRunKey, UZA_BRAND, &have);
    if (present && lstrcmpiW(have.c_str(), AutostartCommand().c_str()) == 0) return;
    SetAutostart(true);
    Log(L"autostart: the entry was %s, written again", present ? L"pointing elsewhere" : L"missing");
}

bool InstallEverything(HWND owner, std::wstring* error) {
    error->clear();
    // A first install starts with Windows; an update keeps what the user chose.
    const bool fresh = !IsInstalled();
    DWORD code = 0;
    bool cancelled = false;
    if (!RunSelfElevated(owner, L"/install-machine", &code, &cancelled)) {
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
    if (fresh) {
        ChooseAutostart(true);
    } else {
        EnsureAutostart();
    }
    return true;
}

bool UninstallEverything(HWND owner, std::wstring* error) {
    error->clear();
    // While the machine-level registration still exists, take the keyboards
    // out of this user's list.
    const bool hadAutostart = GetAutostart();
    DisableForUser();
    SetAutostart(false);

    DWORD code = 0;
    bool cancelled = false;
    if (!RunSelfElevated(owner, L"/uninstall-machine", &code, &cancelled)) {
        if (!cancelled) *error = L"Öçiriş dasturini işga tuşirib bölmadi.";
        // Declined: put the user's keyboards back.
        EnableForUser();
        if (hadAutostart) SetAutostart(true);
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
