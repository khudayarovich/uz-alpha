// UzAlpha: tray icon, main window, and the entry point that also serves as the
// installer (/install-machine, /uninstall-machine, /uninstall).
#include "app.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>

// GDI+ headers use min/max, which NOMINMAX removed.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

using namespace Gdiplus;

namespace {

// Private messages.
const UINT WM_APP_TRAY = WM_APP + 1;  // tray icon callback
const UINT WM_APP_SHOW = WM_APP + 2;  // another instance asks us to show the window
const UINT WM_APP_QUIT = WM_APP + 3;  // the uninstaller asks us to exit

const UINT_PTR kStatusTimer = 1;
const UINT_PTR kAfterSwitchTimer = 2;  // one-shot: record what the switch really did
const UINT_PTR kUpdateTimer = 3;       // look for a new release
const UINT_PTR kLanguageKeyTimer = 4;  // watch the Alt+Shift keys
const UINT kFirstUpdateCheckMs = 20 * 1000;
const UINT kUpdateIntervalMs = 6 * 60 * 60 * 1000;
const UINT kLanguageKeyPollMs = 30;
const DWORD kLanguageKeyWindowMs = 900;

// Layout, in 96-dpi pixels.
const int kClientW = 420;
const int kClientH = 688;
const int kHeaderH = 108;
const int kMargin = 24;

// Colours.
const Color kAccent(255, 37, 99, 235);
const Color kAccentDark(255, 29, 78, 216);
const Color kGreen(255, 5, 150, 105);
const Color kInk(255, 17, 24, 39);
const Color kInk2(255, 55, 65, 81);
const Color kMuted(255, 107, 114, 128);
const Color kFaint(255, 156, 163, 175);
const Color kLine(255, 229, 231, 235);
const Color kCard(255, 249, 250, 251);
const Color kChip(255, 243, 244, 246);
const Color kWhite(255, 255, 255, 255);
const Color kDanger(255, 185, 28, 28);

HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
UINT g_dpi = 96;
HFONT g_uiFont = nullptr;
HBRUSH g_whiteBrush = nullptr;

Keyboard g_active = Keyboard::Other;
bool g_installed = false;
// Installed, but this is a different copy of the program than the installed
// one (a newer download): offer to update instead of to remove.
bool g_updateMode = false;
// A newer release, downloaded and verified, waiting for the user's go-ahead.
UpdateResult* g_update = nullptr;
bool g_hotkeysOk = true;
bool g_relaunchInstalled = false;

// The Windows language key (Alt+Shift) only moves between languages, so on its
// own it never reaches the second Uzbek keyboard. These track one press of it:
// where the user was when the keys went down, and when they came up.
bool g_langKeysHeld = false;
DWORD g_langKeysReleasedAt = 0;
Keyboard g_langKeysFrom = Keyboard::Other;
LANGID g_langKeysFromLang = 0;
int g_tick = 0;

NOTIFYICONDATAW g_nid = {};
HICON g_trayIcons[3] = {};
UINT g_taskbarCreated = 0;
RECT g_emailRect = {};

int S(int v) { return MulDiv(v, (int)g_dpi, 96); }
float F(float v) { return v * (float)g_dpi / 96.0f; }

// ---------------------------------------------------------------------------
// Drawing helpers

void RoundedPath(GraphicsPath& path, RectF r, REAL radius) {
    const REAL d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

void FillRounded(Graphics& g, RectF r, REAL radius, Color fill) {
    GraphicsPath path;
    RoundedPath(path, r, radius);
    SolidBrush brush(fill);
    g.FillPath(&brush, &path);
}

void StrokeRounded(Graphics& g, RectF r, REAL radius, Color stroke, REAL width) {
    GraphicsPath path;
    RoundedPath(path, r, radius);
    Pen pen(stroke, width);
    g.DrawPath(&pen, &path);
}

enum Weight { Regular, Semibold, Bold };

// Text at a top-left point; returns its width in pixels.
REAL Text(Graphics& g, const wchar_t* s, REAL px, Weight weight, Color color, REAL x, REAL y) {
    FontFamily semibold(L"Segoe UI Semibold");
    FontFamily regular(L"Segoe UI");
    const bool haveSemibold = semibold.IsAvailable() != FALSE;
    const FontFamily* family = (weight == Semibold && haveSemibold) ? &semibold : &regular;
    const INT style = (weight == Bold || (weight == Semibold && !haveSemibold)) ? FontStyleBold : FontStyleRegular;
    Font font(family, px, style, UnitPixel);
    SolidBrush brush(color);
    const StringFormat* fmt = StringFormat::GenericTypographic();
    g.DrawString(s, -1, &font, PointF(x, y), fmt, &brush);
    RectF box;
    g.MeasureString(s, -1, &font, PointF(x, y), fmt, &box);
    return box.Width;
}

void TextCentered(Graphics& g, const wchar_t* s, REAL px, Weight weight, Color color, RectF r) {
    FontFamily semibold(L"Segoe UI Semibold");
    FontFamily regular(L"Segoe UI");
    const bool haveSemibold = semibold.IsAvailable() != FALSE;
    const FontFamily* family = (weight == Semibold && haveSemibold) ? &semibold : &regular;
    const INT style = (weight == Bold || (weight == Semibold && !haveSemibold)) ? FontStyleBold : FontStyleRegular;
    Font font(family, px, style, UnitPixel);
    SolidBrush brush(color);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(s, -1, &font, r, &fmt, &brush);
}

// The brand tile: a rounded square with "Ş".
void DrawTile(Graphics& g, RectF r, Color fill, Color letter) {
    FillRounded(g, r, r.Width * 0.22f, fill);
    RectF textRect(r.X, r.Y - r.Height * 0.07f, r.Width, r.Height);
    TextCentered(g, L"Ş", r.Width * 0.56f, Bold, letter, textRect);
}

HICON MakeTrayIcon(Color fill) {
    const int size = std::max(16, GetSystemMetrics(SM_CXSMICON));
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        DrawTile(g, RectF(0, 0, (REAL)size, (REAL)size), fill, kWhite);
    }
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

// ---------------------------------------------------------------------------
// Tray icon

const wchar_t* TrayTip() {
    if (!g_installed) return UZA_BRAND L" — klaviaturalar örnatilmagan";
    switch (g_active) {
        case Keyboard::Auto: return UZA_BRAND L" — Avtomatik (faol)";
        case Keyboard::Manual: return UZA_BRAND L" — Qölda (faol)";
        default: return UZA_BRAND L" — faol emas";
    }
}

HICON TrayIconForState() {
    if (g_installed && g_active == Keyboard::Auto) return g_trayIcons[1];
    if (g_installed && g_active == Keyboard::Manual) return g_trayIcons[2];
    return g_trayIcons[0];
}

void TrayAdd() {
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = TrayIconForState();
    lstrcpynW(g_nid.szTip, TrayTip(), ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

void TrayUpdate() {
    g_nid.hIcon = TrayIconForState();
    lstrcpynW(g_nid.szTip, TrayTip(), ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void TrayRemove() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

// ---------------------------------------------------------------------------
// Window layout

RECT ButtonRect(int id) {
    switch (id) {
        case IDC_AUTO: return {S(kMargin), S(184), S(kMargin + 180), S(184 + 46)};
        case IDC_MANUAL: return {S(kMargin + 192), S(184), S(kMargin + 372), S(184 + 46)};
        case IDC_INSTALL: return {S(kClientW - kMargin - 124), S(482), S(kClientW - kMargin), S(482 + 36)};
        case IDC_AUTOSTART: return {S(kMargin), S(530), S(kClientW - kMargin), S(530 + 24)};
        case IDC_AUTOUPDATE: return {S(kMargin), S(558), S(kClientW - kMargin), S(558 + 24)};
        case IDC_CYCLE: return {S(kMargin), S(586), S(kClientW - kMargin), S(586 + 24)};
    }
    return {};
}

void LayoutChildren() {
    if (g_uiFont) DeleteObject(g_uiFont);
    g_uiFont = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (int id : {IDC_AUTO, IDC_MANUAL, IDC_INSTALL, IDC_AUTOSTART, IDC_AUTOUPDATE, IDC_CYCLE}) {
        const HWND child = GetDlgItem(g_hwnd, id);
        if (!child) continue;
        const RECT r = ButtonRect(id);
        SetWindowPos(child, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(child, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    }
}

void SizeWindowToClient() {
    RECT r = {0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongPtrW(g_hwnd, GWL_STYLE), FALSE,
                             (DWORD)GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE), g_dpi);
    SetWindowPos(g_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Near the tray (bottom-right) when opened from it, otherwise centred.
void PlaceWindow(bool nearTray) {
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    int x, y;
    if (nearTray) {
        x = mi.rcWork.right - w - S(12);
        y = mi.rcWork.bottom - h - S(12);
    } else {
        x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
        y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    }
    SetWindowPos(g_hwnd, nullptr, x, std::max((int)mi.rcWork.top, y), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ShowMain(bool nearTray) {
    if (!IsWindowVisible(g_hwnd)) PlaceWindow(nearTray);
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwnd);
}

// ---------------------------------------------------------------------------
// State

void RefreshControls() {
    EnableWindow(GetDlgItem(g_hwnd, IDC_AUTO), g_installed);
    EnableWindow(GetDlgItem(g_hwnd, IDC_MANUAL), g_installed);
    EnableWindow(GetDlgItem(g_hwnd, IDC_AUTOSTART), g_installed);
    Button_SetCheck(GetDlgItem(g_hwnd, IDC_AUTOSTART), g_installed && GetAutostart() ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(g_hwnd, IDC_AUTOUPDATE), GetAutoUpdate() ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(g_hwnd, IDC_CYCLE), GetSetting(UZA_SETTING_CYCLE) ? BST_CHECKED : BST_UNCHECKED);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    for (int id : {IDC_AUTO, IDC_MANUAL, IDC_INSTALL}) InvalidateRect(GetDlgItem(g_hwnd, id), nullptr, FALSE);
    TrayUpdate();
}

bool IsOtherCopy() { return lstrcmpiW(SelfPath().c_str(), InstalledExePath().c_str()) != 0; }

// Called when the active keyboard has just changed. If the Windows language
// key caused it, add the stops Windows leaves out, so that pressing it goes
//   other languages -> Avtomatik -> Qölda -> other languages
// Leaving Avtomatik lands on Qölda first; arriving in Uzbek starts at Avtomatik.
void StepWithLanguageKey(Keyboard now) {
    const bool recent = g_langKeysHeld ||
                        (g_langKeysReleasedAt && GetTickCount() - g_langKeysReleasedAt < kLanguageKeyWindowMs);
    if (!recent || !GetSetting(UZA_SETTING_CYCLE)) return;

    Keyboard next = Keyboard::Other;
    if (g_langKeysFrom == Keyboard::Auto && ForegroundLanguage() != UZA_LANGID) {
        next = Keyboard::Manual;
    } else if (g_langKeysFromLang != UZA_LANGID && now == Keyboard::Manual) {
        next = Keyboard::Auto;
    }
    if (next == Keyboard::Other) return;

    // One correction per key press. No window is activated: the user stays
    // in the program they are typing in.
    g_langKeysReleasedAt = 0;
    g_langKeysHeld = false;
    g_langKeysFrom = Keyboard::Other;
    Log(L"language key: stepping to %s", next == Keyboard::Manual ? L"manual" : L"auto");
    SwitchKeyboard(next);
}

void RefreshState(bool force) {
    const bool installed = IsInstalled();
    const Keyboard active = installed ? DetectKeyboard() : Keyboard::Other;
    const bool update = installed && IsOtherCopy();
    if (!force && installed == g_installed && active == g_active && update == g_updateMode) return;
    if (active != g_active) {
        LogInputState(L"keyboard changed");
        StepWithLanguageKey(active);
    }
    g_installed = installed;
    g_active = active;
    g_updateMode = update;
    RefreshControls();
}

// ---------------------------------------------------------------------------
// Painting

void PaintWindow(HDC hdc) {
    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.Clear(kWhite);

    const REAL W = F((float)kClientW);
    const REAL M = F((float)kMargin);

    // Header band.
    {
        RectF band(0, 0, W, F((float)kHeaderH));
        LinearGradientBrush grad(band, Color(255, 59, 130, 246), kAccentDark, LinearGradientModeForwardDiagonal);
        g.FillRectangle(&grad, band);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        DrawTile(g, RectF(M, F(26), F(56), F(56)), kWhite, kAccentDark);
        Text(g, UZA_BRAND, F(27), Semibold, kWhite, F(96), F(27));
        Text(g, UZA_TAGLINE, F(13.5f), Regular, Color(225, 255, 255, 255), F(97), F(63));
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    }

    // Current keyboard.
    Text(g, L"HOZIRGI KLAVIATURA", F(11), Semibold, kMuted, M, F(124));
    {
        const wchar_t* label;
        Color dot = kFaint;
        if (!g_installed) {
            label = L"Klaviaturalar hali örnatilmagan";
        } else if (g_active == Keyboard::Auto) {
            label = UZA_AUTO_NAME;
            dot = kAccent;
        } else if (g_active == Keyboard::Manual) {
            label = UZA_MANUAL_NAME;
            dot = kGreen;
        } else {
            label = L"Boşqa klaviatura — " UZA_BRAND L" faol emas";
        }
        SolidBrush dotBrush(dot);
        g.FillEllipse(&dotBrush, M, F(151), F(10), F(10));
        Text(g, label, F(16), Semibold, g_installed && g_active != Keyboard::Other ? kInk : kInk2, M + F(18), F(144));
    }

    // What each keyboard does.
    {
        RectF card(M, F(246), W - 2 * M, F(118));
        FillRounded(g, card, F(10), kCard);
        StrokeRounded(g, card, F(10), kLine, 1);
        const REAL x1 = M + F(14), x2 = M + F(92);
        Text(g, L"Avtomatik", F(13), Semibold, kInk, x1, F(258));
        Text(g, L"sh → ş     ch → ç     o' → ö     g' → ğ", F(13.5f), Regular, kInk, x2, F(258));
        Text(g, L"o` g` ham böladi  ·  Backspace — bekor qiladi", F(12), Regular, kMuted, x2, F(279));
        Pen sep(kLine, 1);
        g.DrawLine(&sep, M + F(14), F(305), W - M - F(14), F(305));
        Text(g, L"Qölda", F(13), Semibold, kInk, x1, F(316));
        Text(g, L";  → ş     '  → ç     [  → ö     ]  → ğ     `  → ʼ", F(13.5f), Regular, kInk, x2, F(316));
        Text(g, L"kiritish.uz terilmasi bilan bir xil", F(12), Regular, kMuted, x2, F(337));
    }

    // Selected-text conversion.
    {
        RectF card(M, F(376), W - 2 * M, F(90));
        FillRounded(g, card, F(10), kCard);
        StrokeRounded(g, card, F(10), kLine, 1);
        const REAL x1 = M + F(14);
        Text(g, L"Tanlangan matnni ögiriş", F(13), Semibold, kInk, x1, F(387));
        if (!g_hotkeysOk) {
            Text(g, L"tugmalar boşqa dasturda band", F(12), Regular, kDanger, x1 + F(170), F(388));
        }
        const wchar_t* keys[2] = {L"Ctrl + Alt + Shift + Y", L"Ctrl + Alt + Shift + E"};
        const wchar_t* what[2] = {L"yangi alifboga  (sh → ş)", L"eski alifboga  (ş → sh)"};
        for (int i = 0; i < 2; ++i) {
            const REAL y = F(410.0f + 25.0f * (float)i);
            RectF chip(x1, y, F(150), F(21));
            FillRounded(g, chip, F(5), kWhite);
            StrokeRounded(g, chip, F(5), kLine, 1);
            TextCentered(g, keys[i], F(12), Semibold, kInk2, chip);
            Text(g, what[i], F(13), Regular, kInk, x1 + F(164), y + F(2));
        }
    }

    // Install row.
    {
        std::wstring row;
        if (g_update) {
            row = L"Yangi versiya " + g_update->version + L" tayyor";
        } else if (g_updateMode) {
            row = L"Yangi versiyani örnatiş";
        } else {
            row = g_installed ? L"Klaviaturalar örnatilgan" : L"Klaviaturalarni Windows’ga qöşiş";
        }
        Text(g, row.c_str(), F(14), g_update ? Semibold : Regular, g_update ? kAccentDark : kInk, M, F(491));
    }

    // Footer.
    {
        Pen sep(kLine, 1);
        g.DrawLine(&sep, M, F(626), W - M, F(626));
        Text(g, L"Dasturçi: " UZA_DEVELOPER, F(12.5f), Regular, kInk2, M, F(638));
        const REAL emailW = Text(g, UZA_EMAIL, F(12.5f), Regular, kAccent, M, F(658));
        g_emailRect = {(LONG)M, (LONG)F(656), (LONG)(M + emailW), (LONG)F(676)};
        Text(g, L"v" UZA_VERSION_W, F(12.5f), Regular, kFaint, W - M - F(40), F(638));
    }
}

void PaintButton(const DRAWITEMSTRUCT* dis) {
    const int w = dis->rcItem.right - dis->rcItem.left;
    const int h = dis->rcItem.bottom - dis->rcItem.top;
    Bitmap buffer(w, h, PixelFormat32bppPARGB);
    {
        Graphics g(&buffer);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.Clear(kWhite);

        const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
        const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        const bool focused = (dis->itemState & ODS_FOCUS) != 0 && (dis->itemState & ODS_NOFOCUSRECT) == 0;

        const wchar_t* label = L"";
        bool primary = false;
        Color primaryColor = kAccent;
        switch (dis->CtlID) {
            case IDC_AUTO:
                label = L"Avtomatik";
                primary = g_installed && g_active == Keyboard::Auto;
                break;
            case IDC_MANUAL:
                label = L"Qölda";
                primary = g_installed && g_active == Keyboard::Manual;
                primaryColor = kGreen;
                break;
            case IDC_INSTALL:
                label = (g_update || g_updateMode) ? L"Yangilaş" : g_installed ? L"Öçiriş" : L"Örnatiş";
                primary = !g_installed || g_updateMode || g_update;
                break;
        }

        RectF r(0.5f, 0.5f, (REAL)w - 1, (REAL)h - 1);
        const REAL radius = F(9);
        Color fill, text;
        if (disabled) {
            fill = kChip;
            text = kFaint;
        } else if (primary) {
            fill = pressed ? kAccentDark : primaryColor;
            text = kWhite;
        } else {
            fill = pressed ? kLine : kChip;
            text = dis->CtlID == IDC_INSTALL ? kDanger : kInk;
        }
        FillRounded(g, r, radius, fill);
        if (!primary) StrokeRounded(g, r, radius, kLine, 1);
        if (focused) {
            RectF ring(2.5f, 2.5f, (REAL)w - 5, (REAL)h - 5);
            StrokeRounded(g, ring, radius - 2, primary ? Color(200, 255, 255, 255) : kAccent, 1.5f);
        }
        const REAL px = dis->CtlID == IDC_INSTALL ? F(14) : F(15.5f);
        TextCentered(g, label, px, Semibold, text, RectF(0, -1, (REAL)w, (REAL)h));
    }
    Graphics out(dis->hDC);
    out.DrawImage(&buffer, (INT)dis->rcItem.left, (INT)dis->rcItem.top);
}

// ---------------------------------------------------------------------------
// Actions

void DoSwitch(Keyboard k) {
    if (!g_installed) return;
    // The switch applies to the whole session only when made by the window
    // that owns the keyboard focus.
    SetForegroundWindow(g_hwnd);
    if (!SwitchKeyboard(k)) {
        MessageBoxW(g_hwnd,
                    L"Klaviaturani almaştirib bölmadi.\n\n"
                    L"Win + Space tugmalari orqali özingiz tanlab köring.",
                    UZA_BRAND, MB_OK | MB_ICONWARNING);
    }
    RefreshState(true);
    SetTimer(g_hwnd, kAfterSwitchTimer, 1200, nullptr);
}

// Shown after an update. A program that is already running keeps the keyboard
// service it loaded until it is restarted, and nobody restarts Explorer: its
// rename box and the Start menu would go on with the old one for days.
const wchar_t kAfterUpdateNote[] =
    L"Oçiq turgan dasturlar yangi versiyani qayta işga tuşirilgandan söng işlatadi.\n\n"
    L"Fayl nomini özgartiriş kabi Windows oynalarida ham yangi versiya işlaşi uçun "
    L"tizimdan çiqib qayta kiring yoki kompyuterni qayta işga tuşiring.";

void DoInstall() {
    const bool wasUpdate = g_updateMode;
    std::wstring error;
    EnableWindow(GetDlgItem(g_hwnd, IDC_INSTALL), FALSE);
    const bool ok = InstallEverything(g_hwnd, &error);
    EnableWindow(GetDlgItem(g_hwnd, IDC_INSTALL), TRUE);
    RefreshState(true);
    if (!ok) {
        if (!error.empty()) MessageBoxW(g_hwnd, error.c_str(), UZA_BRAND, MB_OK | MB_ICONERROR);
        return;
    }
    const std::wstring done =
        wasUpdate ? std::wstring(UZA_BRAND L" yangilandi.\n\n") + kAfterUpdateNote
                  : std::wstring(L"Klaviaturalar örnatildi.\n\n"
                                 L"Win + Space tugmalari yoki şu oyna orqali «" UZA_AUTO_NAME L"» yoki «" UZA_MANUAL_NAME
                                 L"» klaviaturasini tanlang.");
    MessageBoxW(g_hwnd, done.c_str(), UZA_BRAND, MB_OK | MB_ICONINFORMATION);
    // Carry on from the installed copy, so the downloaded file can be deleted.
    if (lstrcmpiW(SelfPath().c_str(), InstalledExePath().c_str()) != 0) {
        g_relaunchInstalled = true;
        DestroyWindow(g_hwnd);
    }
}

// Installs the downloaded release, then carries on from the new copy.
void DoApplyUpdate() {
    if (!g_update) return;
    std::wstring error;
    EnableWindow(GetDlgItem(g_hwnd, IDC_INSTALL), FALSE);
    const bool ok = ApplyUpdate(g_hwnd, *g_update, &error);
    EnableWindow(GetDlgItem(g_hwnd, IDC_INSTALL), TRUE);
    if (!ok) {
        if (!error.empty()) {
            MessageBoxW(g_hwnd, error.c_str(), UZA_BRAND, MB_OK | MB_ICONERROR);
            delete g_update;  // a broken download: look again later
            g_update = nullptr;
        }
        RefreshState(true);
        return;
    }
    const std::wstring done = UZA_BRAND L" " + g_update->version + L" örnatildi.\n\n" + kAfterUpdateNote;
    MessageBoxW(g_hwnd, done.c_str(), UZA_BRAND, MB_OK | MB_ICONINFORMATION);
    g_relaunchInstalled = true;
    DestroyWindow(g_hwnd);
}

// Tells the user, from the tray, that an update is waiting.
void ShowUpdateBalloon() {
    if (!g_update) return;
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO | NIF_SHOWTIP;
    nid.dwInfoFlags = NIIF_INFO;
    lstrcpynW(nid.szInfoTitle, UZA_BRAND, ARRAYSIZE(nid.szInfoTitle));
    const std::wstring text = L"Yangi versiya " + g_update->version + L" yuklab olindi. Örnatiş uçun bosing.";
    lstrcpynW(nid.szInfo, text.c_str(), ARRAYSIZE(nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void OnUpdateResult(UpdateResult* result) {
    if (result->state == UpdateResult::Ready) {
        const bool isNew = !g_update || g_update->version != result->version;
        delete g_update;
        g_update = result;
        RefreshControls();
        if (result->manual) {
            ShowMain(true);
        } else if (isNew) {
            ShowUpdateBalloon();
        }
        return;
    }
    if (result->manual) {
        if (result->state == UpdateResult::UpToDate) {
            MessageBoxW(g_hwnd, L"Sizda eng sönggi versiya örnatilgan (" UZA_VERSION_W L").", UZA_BRAND,
                        MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(g_hwnd, L"Yangilanişni tekşirib bölmadi. Internet aloqasini tekşirib, qayta urinib köring.",
                        UZA_BRAND, MB_OK | MB_ICONWARNING);
        }
    }
    delete result;
}

void DoUninstall() {
    if (MessageBoxW(g_hwnd, UZA_BRAND L" klaviaturalari kompyuterdan öçirilsinmi?", UZA_BRAND,
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    std::wstring error;
    const bool ok = UninstallEverything(g_hwnd, &error);
    RefreshState(true);
    if (!ok) {
        if (!error.empty()) MessageBoxW(g_hwnd, error.c_str(), UZA_BRAND, MB_OK | MB_ICONERROR);
        return;
    }
    MessageBoxW(g_hwnd,
                UZA_BRAND L" öçirildi.\n\nQolgan fayllar kompyuter qayta işga tuşganda tozalanadi.",
                UZA_BRAND, MB_OK | MB_ICONINFORMATION);
    DestroyWindow(g_hwnd);
}

void ShowTrayMenu(int x, int y) {
    const HMENU menu = CreatePopupMenu();
    const UINT gray = g_installed ? 0 : MF_GRAYED;
    AppendMenuW(menu, MF_STRING | gray | (g_active == Keyboard::Auto ? MF_CHECKED : 0), IDM_AUTO, L"Avtomatik");
    AppendMenuW(menu, MF_STRING | gray | (g_active == Keyboard::Manual ? MF_CHECKED : 0), IDM_MANUAL, L"Qölda");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_CHECKUPDATE, L"Yangilanişni tekşiriş");
    AppendMenuW(menu, MF_STRING, IDM_OPEN, L"Oçiş");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Çiqiş");
    SetMenuDefaultItem(menu, IDM_OPEN, FALSE);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, x, y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        TrayAdd();  // Explorer restarted
        return 0;
    }
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            g_dpi = GetDpiForWindow(hwnd);
            const DWORD btn = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
            CreateWindowExW(0, L"BUTTON", L"Avtomatik", btn, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_AUTO, g_inst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Qölda", btn, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_MANUAL, g_inst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Örnatiş", btn, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_INSTALL, g_inst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Windows bilan birga işga tuşsin",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                            (HMENU)(INT_PTR)IDC_AUTOSTART, g_inst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Yangilanişlarni avtomatik tekşiriş",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                            (HMENU)(INT_PTR)IDC_AUTOUPDATE, g_inst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Alt+Shift: Avtomatik → Qölda → boşqa til",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                            (HMENU)(INT_PTR)IDC_CYCLE, g_inst, nullptr);
            SizeWindowToClient();
            LayoutChildren();

            g_trayIcons[0] = MakeTrayIcon(kFaint);
            g_trayIcons[1] = MakeTrayIcon(kAccent);
            g_trayIcons[2] = MakeTrayIcon(kGreen);
            g_installed = IsInstalled();
            g_updateMode = g_installed && IsOtherCopy();
            // Repair a user profile where the keyboards are installed on the
            // machine but not enabled for this user (a fresh update, or another
            // account on the PC), and drop what an older version left behind.
            if (g_installed) {
                if (ProfilesEnabled() && KeyboardsInUserList()) {
                    CleanUserList();
                } else {
                    EnableForUser();
                }
                EnsureAutostart();
            }
            g_active = g_installed ? DetectKeyboard() : Keyboard::Other;
            LogInputState(L"start");
            TrayAdd();
            g_hotkeysOk = RegisterConvertHotkeys(hwnd);
            RefreshControls();
            SetTimer(hwnd, kStatusTimer, 400, nullptr);
            SetTimer(hwnd, kUpdateTimer, kFirstUpdateCheckMs, nullptr);
            SetTimer(hwnd, kLanguageKeyTimer, kLanguageKeyPollMs, nullptr);
            return 0;
        }

        case WM_APP_UPDATE:
            OnUpdateResult((UpdateResult*)lp);
            return 0;

        case WM_TIMER:
            if (wp == kStatusTimer) {
                RefreshState(false);
            } else if (wp == kAfterSwitchTimer) {
                KillTimer(hwnd, kAfterSwitchTimer);
                LogInputState(L"after switch");
            } else if (wp == kLanguageKeyTimer) {
                if (!g_installed) return 0;
                const bool held = LanguageKeysDown();
                if (held && !g_langKeysHeld) {
                    RefreshState(false);  // know exactly where the user is now
                    g_langKeysHeld = true;
                    g_langKeysFrom = g_active;
                    g_langKeysFromLang = ForegroundLanguage();
                } else if (!held && g_langKeysHeld) {
                    g_langKeysHeld = false;
                    g_langKeysReleasedAt = GetTickCount() | 1;
                }
                // Right after a press, look for the change at once instead of
                // waiting for the slower status timer.
                if (g_langKeysReleasedAt && GetTickCount() - g_langKeysReleasedAt < kLanguageKeyWindowMs) {
                    RefreshState(false);
                }
            } else if (wp == kUpdateTimer) {
                SetTimer(hwnd, kUpdateTimer, kUpdateIntervalMs, nullptr);
                if (g_installed && GetAutoUpdate()) StartUpdateCheck(hwnd, false);
            } else if (wp == UZA_TIMER_CONVERT) {
                ConvertTick(hwnd);
            }
            return 0;

        case WM_HOTKEY:
            ConvertSelection(hwnd, (int)wp);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            const HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            const HDC mem = CreateCompatibleDC(hdc);
            const HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            const HGDIOBJ old = SelectObject(mem, bmp);
            PaintWindow(mem);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_PRINTCLIENT:
            PaintWindow((HDC)wp);
            return 0;

        case WM_DRAWITEM:
            PaintButton((const DRAWITEMSTRUCT*)lp);
            return TRUE;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)g_whiteBrush;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_AUTO: case IDM_AUTO: DoSwitch(Keyboard::Auto); break;
                case IDC_MANUAL: case IDM_MANUAL: DoSwitch(Keyboard::Manual); break;
                case IDC_INSTALL:
                    if (g_update) {
                        DoApplyUpdate();
                    } else if (g_installed && !g_updateMode) {
                        DoUninstall();
                    } else {
                        DoInstall();
                    }
                    break;
                case IDC_AUTOSTART:
                    ChooseAutostart(Button_GetCheck(GetDlgItem(hwnd, IDC_AUTOSTART)) == BST_CHECKED);
                    break;
                case IDC_AUTOUPDATE: {
                    const bool on = Button_GetCheck(GetDlgItem(hwnd, IDC_AUTOUPDATE)) == BST_CHECKED;
                    SetAutoUpdate(on);
                    if (on && g_installed) StartUpdateCheck(hwnd, false);
                    break;
                }
                case IDC_CYCLE:
                    SetSetting(UZA_SETTING_CYCLE, Button_GetCheck(GetDlgItem(hwnd, IDC_CYCLE)) == BST_CHECKED);
                    break;
                case IDM_CHECKUPDATE: StartUpdateCheck(hwnd, true); break;
                case IDM_OPEN: ShowMain(true); break;
                case IDM_EXIT: DestroyWindow(hwnd); break;
            }
            return 0;

        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    if (IsWindowVisible(hwnd) && GetForegroundWindow() == hwnd) {
                        ShowWindow(hwnd, SW_HIDE);
                    } else {
                        ShowMain(true);
                    }
                    break;
                case NIN_BALLOONUSERCLICK:  // "a new version is ready"
                    ShowMain(true);
                    break;
                case WM_CONTEXTMENU:
                    ShowTrayMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
                    break;
            }
            return 0;

        case WM_APP_SHOW:
            ShowMain(false);
            return 0;

        case WM_APP_QUIT:
            DestroyWindow(hwnd);
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (PtInRect(&g_emailRect, pt)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;

        case WM_LBUTTONUP: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&g_emailRect, pt)) {
                ShellExecuteW(hwnd, L"open", L"mailto:" UZA_EMAIL, nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }

        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            SizeWindowToClient();
            LayoutChildren();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);  // keep running in the tray
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kStatusTimer);
            KillTimer(hwnd, kUpdateTimer);
            KillTimer(hwnd, kLanguageKeyTimer);
            RestoreClipboardNow(hwnd);
            UnregisterConvertHotkeys(hwnd);
            TrayRemove();
            for (HICON& icon : g_trayIcons) {
                if (icon) DestroyIcon(icon);
                icon = nullptr;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// "Apps & features" → Uninstall. `quiet` (/uninstall /quiet) asks and says
// nothing, for scripted removal.
int InteractiveUninstall(bool quiet) {
    if (const HWND running = FindWindowW(UZA_WNDCLASS, nullptr)) {
        PostMessageW(running, WM_APP_QUIT, 0, 0);
        Sleep(500);
    }
    if (!quiet && MessageBoxW(nullptr, UZA_BRAND L" klaviaturalari kompyuterdan öçirilsinmi?", UZA_BRAND,
                              MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return 1;
    }
    std::wstring error;
    if (!UninstallEverything(nullptr, &error)) {
        if (!error.empty() && !quiet) MessageBoxW(nullptr, error.c_str(), UZA_BRAND, MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!quiet) {
        MessageBoxW(nullptr, UZA_BRAND L" öçirildi.\n\nQolgan fayllar kompyuter qayta işga tuşganda tozalanadi.",
                    UZA_BRAND, MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    g_inst = inst;
    const std::wstring args = cmdLine ? cmdLine : L"";

    // Elevated helper modes: no window, just the work and an exit code.
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i + 2 < argc; ++i) {
            if (lstrcmpiW(argv[i], L"/apply-update") == 0) return ApplyUpdateElevated(argv[i + 1], argv[i + 2]);
        }
        if (argv) LocalFree(argv);
    }
    if (args.find(L"/install-machine") != std::wstring::npos) return MachineInstall();
    if (args.find(L"/uninstall-machine") != std::wstring::npos) return MachineUninstall();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool quiet = args.find(L"/quiet") != std::wstring::npos;
    if (args.find(L"/uninstall") != std::wstring::npos) {
        const int rc = InteractiveUninstall(quiet);
        CoUninitialize();
        return rc;
    }
    // UzAlpha.exe /install [/quiet]: install without opening the window
    // (Windows still asks for permission unless already elevated).
    if (args.find(L"/install") != std::wstring::npos) {
        std::wstring error;
        const bool ok = InstallEverything(nullptr, &error);
        if (!ok && !error.empty() && !quiet) MessageBoxW(nullptr, error.c_str(), UZA_BRAND, MB_OK | MB_ICONERROR);
        CoUninitialize();
        return ok ? 0 : 1;
    }
    const bool startHidden = args.find(L"/tray") != std::wstring::npos;

    const HANDLE mutex = CreateMutexW(nullptr, TRUE, UZA_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        bool takeOver = false;
        if (const HWND other = FindWindowW(UZA_WNDCLASS, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            // Is the running one a different copy of the program (an older
            // installed version, and this is a new download)? Then replace it,
            // so this copy can offer the update.
            wchar_t otherPath[MAX_PATH] = {};
            DWORD len = MAX_PATH;
            if (const HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                QueryFullProcessImageNameW(proc, 0, otherPath, &len);
                CloseHandle(proc);
            }
            if (!startHidden && otherPath[0] && lstrcmpiW(otherPath, SelfPath().c_str()) != 0) {
                PostMessageW(other, WM_APP_QUIT, 0, 0);
                const DWORD w = mutex ? WaitForSingleObject(mutex, 5000) : WAIT_FAILED;
                takeOver = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
            } else {
                // Same program already running: bring its window up instead.
                AllowSetForegroundWindow(pid);
                if (!startHidden) PostMessageW(other, WM_APP_SHOW, 0, 0);
            }
        }
        if (!takeOver) {
            if (mutex) CloseHandle(mutex);
            CoUninitialize();
            return 0;
        }
    }

    GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    InputInit();

    g_whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_whiteBrush;
    wc.lpszClassName = UZA_WNDCLASS;
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    const HWND hwnd = CreateWindowExW(0, UZA_WNDCLASS, UZA_BRAND, style, CW_USEDEFAULT, CW_USEDEFAULT, 440, 680,
                                      nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    if (!startHidden) ShowMain(false);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg)) continue;  // Tab / Enter / Space between the buttons
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    InputShutdown();
    delete g_update;
    g_update = nullptr;
    if (g_uiFont) DeleteObject(g_uiFont);
    if (g_whiteBrush) DeleteObject(g_whiteBrush);
    GdiplusShutdown(gdiplusToken);
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    CoUninitialize();

    if (g_relaunchInstalled) {
        ShellExecuteW(nullptr, L"open", InstalledExePath().c_str(), L"/show", nullptr, SW_SHOWNORMAL);
    }
    return 0;
}
