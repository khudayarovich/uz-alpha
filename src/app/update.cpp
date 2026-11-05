// Updates from GitHub releases.
//
// Every release publishes two files next to each other:
//   update.txt    version=1.0.1 / sha256=<hex> / file=UzAlpha.exe
//   UzAlpha.exe   the whole program (it is its own installer)
//
// A background thread reads update.txt of the latest release, and if it names
// a newer version downloads that UzAlpha.exe over HTTPS and checks its SHA-256
// against the published one. Nothing is installed until the user agrees:
// replacing files in System32 and Program Files needs the Windows prompt.
// Nothing about the user or the PC is sent; these are plain downloads.
#include "app.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <vector>

namespace {

const wchar_t kInfoUrl[] = UZA_RELEASES_BASE L"/latest/download/update.txt";
const wchar_t kSettingsKey[] = L"Software\\" UZA_INSTALL_DIRNAME;
const size_t kMaxInfoBytes = 4096;
const size_t kMaxExeBytes = 64u * 1024 * 1024;

volatile LONG g_checking = 0;

struct Request {
    HWND notify;
    bool manual;
};

// HTTPS GET. The body goes to `file` if given, else into `body`.
bool HttpGet(const std::wstring& url, std::vector<BYTE>* body, HANDLE file, size_t limit) {
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;

    bool ok = false;
    HINTERNET session = WinHttpOpen(UZA_BRAND L"/" UZA_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = nullptr, request = nullptr;
    if (session) {
        WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
        connect = WinHttpConnect(session, host, uc.nPort, 0);
    }
    if (connect) {
        // Redirects are followed (GitHub serves release files from another
        // host); WinHTTP refuses a redirect from HTTPS down to HTTP.
        request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            size_t total = 0;
            BYTE chunk[16384];
            ok = true;
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(request, chunk, sizeof chunk, &got)) {
                    ok = false;
                    break;
                }
                if (got == 0) break;
                total += got;
                if (total > limit) {
                    ok = false;
                    break;
                }
                if (file) {
                    DWORD written = 0;
                    if (!WriteFile(file, chunk, got, &written, nullptr) || written != got) {
                        ok = false;
                        break;
                    }
                } else {
                    body->insert(body->end(), chunk, chunk + got);
                }
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

}  // namespace

std::wstring FileSha256(const std::wstring& path) {
    std::wstring hex;
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return hex;

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        BYTE chunk[65536];
        DWORD got = 0;
        bool ok = true;
        while (ReadFile(f, chunk, sizeof chunk, &got, nullptr) && got > 0) {
            if (BCryptHashData(hash, chunk, got, 0) != 0) {
                ok = false;
                break;
            }
        }
        BYTE digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
            wchar_t two[3];
            for (BYTE b : digest) {
                swprintf_s(two, L"%02x", b);
                hex += two;
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}

namespace {

// "key=value" lines.
std::wstring InfoValue(const std::string& text, const char* key) {
    const std::string needle = std::string(key) + "=";
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.compare(0, needle.size(), needle) == 0) {
            const std::string value = line.substr(needle.size());
            return std::wstring(value.begin(), value.end());
        }
        pos = end + 1;
    }
    return L"";
}

// Accepts exactly "N.N.N"; the version ends up in a URL and a file name.
bool ParseVersion(const std::wstring& v, int out[3]) {
    int part = 0, digits = 0, value = 0;
    for (const wchar_t c : v) {
        if (c >= L'0' && c <= L'9') {
            value = value * 10 + (c - L'0');
            if (++digits > 4) return false;
        } else if (c == L'.' && digits > 0 && part < 2) {
            out[part++] = value;
            value = 0;
            digits = 0;
        } else {
            return false;
        }
    }
    if (part != 2 || digits == 0) return false;
    out[2] = value;
    return true;
}

bool IsHex64(const std::wstring& s) {
    if (s.size() != 64) return false;
    for (const wchar_t c : s) {
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    }
    return true;
}

std::wstring UpdateDir() {
    wchar_t dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring path = std::wstring(dir) + L"\\" UZA_INSTALL_DIRNAME;
    CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\update";
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

// Old downloads are of no use once a newer one (or none) is wanted.
void RemoveOtherDownloads(const std::wstring& dir, const std::wstring& keep) {
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring path = dir + L"\\" + fd.cFileName;
        if (lstrcmpiW(path.c_str(), keep.c_str()) != 0) DeleteFileW(path.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void Check(UpdateResult* result) {
    std::vector<BYTE> body;
    if (!HttpGet(kInfoUrl, &body, nullptr, kMaxInfoBytes)) {
        Log(L"update: cannot read %s", kInfoUrl);
        return;
    }
    const std::string info(body.begin(), body.end());
    const std::wstring version = InfoValue(info, "version");
    std::wstring sha = InfoValue(info, "sha256");
    CharLowerBuffW(&sha[0], (DWORD)sha.size());
    int latest[3] = {};
    if (!ParseVersion(version, latest) || !IsHex64(sha)) {
        Log(L"update: update.txt is not in the expected form");
        return;
    }

    const int current[3] = {UZA_VER_MAJOR, UZA_VER_MINOR, UZA_VER_PATCH};
    bool newer = false;
    for (int i = 0; i < 3; ++i) {
        if (latest[i] != current[i]) {
            newer = latest[i] > current[i];
            break;
        }
    }
    result->version = version;
    result->sha256 = sha;
    const std::wstring dir = UpdateDir();
    if (!newer) {
        result->state = UpdateResult::UpToDate;
        if (!dir.empty()) RemoveOtherDownloads(dir, L"");
        Log(L"update: %s is the latest release, nothing to do", version.c_str());
        return;
    }
    if (dir.empty()) return;

    const std::wstring target = dir + L"\\" UZA_BRAND L"-" + version + L".exe";
    RemoveOtherDownloads(dir, target);
    if (FileSha256(target) == sha) {  // downloaded on an earlier check
        result->path = target;
        result->state = UpdateResult::Ready;
        return;
    }

    const std::wstring url = std::wstring(UZA_RELEASES_BASE) + L"/download/v" + version + L"/" UZA_APP_EXE;
    const std::wstring part = target + L".part";
    const HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const bool got = HttpGet(url, nullptr, f, kMaxExeBytes);
    CloseHandle(f);
    if (!got) {
        DeleteFileW(part.c_str());
        Log(L"update: download of %s failed", url.c_str());
        return;
    }
    if (FileSha256(part) != sha) {
        DeleteFileW(part.c_str());
        Log(L"update: the downloaded file does not match the published SHA-256; discarded");
        return;
    }
    DeleteFileW(target.c_str());
    if (!MoveFileW(part.c_str(), target.c_str())) {
        DeleteFileW(part.c_str());
        return;
    }
    result->path = target;
    result->state = UpdateResult::Ready;
    Log(L"update: version %s downloaded and verified", version.c_str());
}

DWORD WINAPI Worker(void* param) {
    Request* req = (Request*)param;
    UpdateResult* result = new UpdateResult();
    result->manual = req->manual;
    Check(result);
    const HWND notify = req->notify;
    delete req;
    InterlockedExchange(&g_checking, 0);
    if (!PostMessageW(notify, WM_APP_UPDATE, 0, (LPARAM)result)) delete result;
    return 0;
}

}  // namespace

void StartUpdateCheck(HWND notify, bool manual) {
    if (InterlockedCompareExchange(&g_checking, 1, 0) != 0) return;  // one at a time
    Request* req = new Request{notify, manual};
    const HANDLE thread = CreateThread(nullptr, 0, Worker, req, 0, nullptr);
    if (!thread) {
        delete req;
        InterlockedExchange(&g_checking, 0);
        return;
    }
    CloseHandle(thread);
}

bool ApplyUpdate(HWND owner, const UpdateResult& update, std::wstring* error) {
    error->clear();
    // The file sat in the user's profile since it was checked: check it again
    // right before it is run elevated.
    if (update.state != UpdateResult::Ready || FileSha256(update.path) != update.sha256) {
        *error = L"Yuklab olingan yangilaniş fayli buzilgan. Keyinroq qayta urinib köring.";
        DeleteFileW(update.path.c_str());
        return false;
    }
    // Windows asks permission for this program, the copy the user already
    // installed and knows, not for an unfamiliar file in a download folder.
    // The elevated copy verifies the download again and lets it install itself.
    const std::wstring args = L"/apply-update \"" + update.path + L"\" " + update.sha256;
    DWORD code = 0;
    bool cancelled = false;
    if (!RunSelfElevated(owner, args, &code, &cancelled)) {
        if (!cancelled) *error = L"Örnatuvçini işga tuşirib bölmadi.";
        return false;
    }
    if (code != 0) {
        wchar_t buf[128];
        swprintf_s(buf, L"Yangilaşda xatolik yuz berdi (kod %lu).", code);
        *error = buf;
        return false;
    }
    return true;  // the new copy adds its keyboards to the user's list when it starts
}

// Per-user on/off settings (HKCU\Software\UzAlpha); all default to on.
bool GetSetting(const wchar_t* name) {
    DWORD value = 1, size = sizeof value;
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}

void SetSetting(const wchar_t* name, bool on) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    const DWORD value = on ? 1 : 0;
    RegSetValueExW(h, name, 0, REG_DWORD, (const BYTE*)&value, sizeof value);
    RegCloseKey(h);
}

bool GetAutoUpdate() { return GetSetting(L"AutoUpdate"); }
void SetAutoUpdate(bool on) { SetSetting(L"AutoUpdate", on); }
