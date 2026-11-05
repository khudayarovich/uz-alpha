// UzAlpha tray app: shared declarations.
#pragma once

#include <windows.h>

#include <string>

#include "../common/ids.h"
#include "resource.h"

// --- log.cpp-less logging (install.cpp) ------------------------------------
// Appends a line to %LOCALAPPDATA%\UzAlpha\uzalpha.log.
void Log(const wchar_t* fmt, ...);

// --- install.cpp -------------------------------------------------------------
std::wstring SelfPath();
std::wstring InstallDir();
std::wstring InstalledExePath();

// Are the keyboards registered on this machine?
bool IsInstalled();

// Machine-level work; must run elevated. Return a process exit code (0 = ok).
int MachineInstall();
int MachineUninstall();
// UzAlpha.exe /apply-update "<downloaded exe>" <sha256>
int ApplyUpdateElevated(const std::wstring& file, const std::wstring& sha256);

// Starts this program again elevated with `args` and waits for it. Returns
// false if it could not be started; `cancelled` then says whether the user
// declined the Windows prompt.
bool RunSelfElevated(HWND owner, const std::wstring& args, DWORD* exitCode, bool* cancelled);

// Whole flows, started from the normal (unelevated) instance. `owner` gets the
// UAC prompt. On failure `error` holds a message for the user; a declined UAC
// prompt returns false with an empty message.
bool InstallEverything(HWND owner, std::wstring* error);
bool UninstallEverything(HWND owner, std::wstring* error);

// Put our two keyboards into / take them out of the current user's list.
bool EnableForUser();
void DisableForUser();
// Remove what an older UzAlpha left in the user's list.
void CleanUserList();
// Are both keyboards in the user's own keyboard list?
bool KeyboardsInUserList();

bool GetAutostart();
void SetAutostart(bool on);

// --- inputctl.cpp ------------------------------------------------------------
enum class Keyboard { Other, Auto, Manual };

bool InputInit();
void InputShutdown();
// Which keyboard the foreground application is typing with right now.
Keyboard DetectKeyboard();
// Make `k` (Auto or Manual) the active keyboard for the session. Must be
// called while one of our own windows has the focus.
bool SwitchKeyboard(Keyboard k);
// Does TSF consider both of our keyboards enabled for this user?
bool ProfilesEnabled();
// Writes what Windows reports about the active keyboard to the log.
void LogInputState(const wchar_t* why);

// --- update.cpp --------------------------------------------------------------
// Posted to the window when an update check finishes; lParam is an
// UpdateResult* that the receiver must delete.
#define WM_APP_UPDATE (WM_APP + 4)

struct UpdateResult {
    enum State { UpToDate, Ready, Failed } state = Failed;
    bool manual = false;    // the user asked for this check
    std::wstring version;   // e.g. "1.0.1"
    std::wstring path;      // downloaded, verified UzAlpha.exe (state == Ready)
    std::wstring sha256;    // lower-case hex, as published with the release
};

// Looks on GitHub for a newer release in the background; if there is one,
// downloads it and checks it against the published SHA-256.
void StartUpdateCheck(HWND notify, bool manual);
// Checks the downloaded file once more and installs it (UAC prompt).
bool ApplyUpdate(HWND owner, const UpdateResult& update, std::wstring* error);
// Lower-case hex SHA-256 of a file; empty on failure.
std::wstring FileSha256(const std::wstring& path);

bool GetAutoUpdate();
void SetAutoUpdate(bool on);

// --- convert.cpp -------------------------------------------------------------
#define UZA_HOTKEY_TO_NEW 1
#define UZA_HOTKEY_TO_OLD 2
#define UZA_TIMER_CLIPBOARD 42

// Registers Ctrl+Alt+Shift+Y / Ctrl+Alt+Shift+E. Returns false if another
// program already owns one of them.
bool RegisterConvertHotkeys(HWND hwnd);
void UnregisterConvertHotkeys(HWND hwnd);
// Converts the text selected in the foreground application.
void ConvertSelection(HWND hwnd, int hotkeyId);
// Timer callback: puts the user's own clipboard content back.
void RestoreClipboardNow(HWND hwnd);
