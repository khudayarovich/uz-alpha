// UzAlpha: identifiers, names and brand strings. Rename the product here.
#pragma once

#include <windows.h>
#include "version.h"

#define UZA_BRAND            L"UzAlpha"
#define UZA_TAGLINE          L"Yangi özbek alifbosi klaviaturasi"
#define UZA_DEVELOPER        L"Farrukh Yuldashev"
#define UZA_EMAIL            L"fashuzfy98@gmail.com"
#define UZA_VERSION_W        L"" UZA_VER_STR

// Where releases are published; the app looks here for updates.
#define UZA_GITHUB_OWNER     L"khudayarovich"
#define UZA_GITHUB_REPO      L"uz-alpha"
#define UZA_RELEASES_BASE    L"https://github.com/" UZA_GITHUB_OWNER L"/" UZA_GITHUB_REPO L"/releases"

// Names shown in the Windows keyboard list (Win+Space).
#define UZA_AUTO_NAME        L"Özbekça (lotin, avtomatik)"
#define UZA_MANUAL_NAME      L"Özbekça (lotin, qölda)"

// Uzbek (Latin, Uzbekistan)
#define UZA_LANGID           ((LANGID)0x0443)
#define UZA_LANGID_STR       L"0443"

// Files
#define UZA_TIP_DLL          L"UzAlphaTip.dll"
// Version 1.0.0 shipped Manual as a keyboard layout DLL; later versions only
// need the name to clean it up.
#define UZA_LEGACY_KBD_DLL   L"kbduzalp.dll"
#define UZA_APP_EXE          L"UzAlpha.exe"
#define UZA_INSTALL_DIRNAME  L"UzAlpha"

// The Auto text service (TSF text input processor).
// {D6C739FF-D990-418B-8DDE-DA75436B5F08}
static const GUID CLSID_UzAlphaTip =
    {0xd6c739ff, 0xd990, 0x418b, {0x8d, 0xde, 0xda, 0x75, 0x43, 0x6b, 0x5f, 0x08}};
#define UZA_CLSID_STR        L"{D6C739FF-D990-418B-8DDE-DA75436B5F08}"

// Its two language profiles: one entry each in the Windows keyboard list.
// Automatic (UZA): {81E0DDC7-68E8-4A0F-8CF3-4CBCCE454F13}
static const GUID GUID_UzAlphaProfile =
    {0x81e0ddc7, 0x68e8, 0x4a0f, {0x8c, 0xf3, 0x4c, 0xbc, 0xce, 0x45, 0x4f, 0x13}};
#define UZA_PROFILE_STR      L"{81E0DDC7-68E8-4A0F-8CF3-4CBCCE454F13}"
// Manual (UZM): {B514FA1D-9D1B-431C-A0E9-BFC72B190B43}
static const GUID GUID_UzAlphaManualProfile =
    {0xb514fa1d, 0x9d1b, 0x431c, {0xa0, 0xe9, 0xbf, 0xc7, 0x2b, 0x19, 0x0b, 0x43}};
#define UZA_MANUAL_PROFILE_STR L"{B514FA1D-9D1B-431C-A0E9-BFC72B190B43}"

// Icon resource ids inside UzAlphaTip.dll, and their index for RegisterProfile.
#define UZA_ICON_AUTO        1
#define UZA_ICON_MANUAL      2
#define UZA_ICON_AUTO_INDEX   0
#define UZA_ICON_MANUAL_INDEX 1

// Display attribute used for the pending letter (drawn with no underline).
// {5AC6CDD8-9899-48D2-B7F9-7E94C43C6627}
static const GUID GUID_UzAlphaDisplayAttr =
    {0x5ac6cdd8, 0x9899, 0x48d2, {0xb7, 0xf9, 0x7e, 0x94, 0xc4, 0x3c, 0x66, 0x27}};

// TSF global compartment: thread id of the focused thread that has the Auto
// text service active, or 0. Written by the text service, read by the tray app.
// {02407899-FE2E-4230-8C57-981D2D28A2AA}
static const GUID GUID_UzAlphaActiveCompartment =
    {0x02407899, 0xfe2e, 0x4230, {0x8c, 0x57, 0x98, 0x1d, 0x2d, 0x28, 0xa2, 0xaa}};

// TSF global compartment: which keyboard that thread is typing with.
// {F88FF309-EA7C-4C40-B034-268DB7930370}
static const GUID GUID_UzAlphaModeCompartment =
    {0xf88ff309, 0xea7c, 0x4c40, {0xb0, 0x34, 0x26, 0x8d, 0xb7, 0x93, 0x03, 0x70}};
#define UZA_MODE_AUTO        1
#define UZA_MODE_MANUAL      2

// Tray app window class (also used to find a running instance).
#define UZA_WNDCLASS         L"UzAlphaMainWindow"
#define UZA_MUTEX            L"Local\\UzAlpha.SingleInstance"
