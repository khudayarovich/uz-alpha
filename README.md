# UzAlpha

Yangi özbek alifbosi (ş ç ö ğ va tutuq belgisi ʼ) uçun Windows va macOS klaviaturalari.<br>
Keyboards for the new Uzbek Latin alphabet, for Windows and macOS.

**[⬇ Windows — UzAlpha.exe](https://github.com/khudayarovich/uz-alpha/releases/latest/download/UzAlpha.exe)** ·
**[⬇ macOS — UzAlpha-macOS.zip](https://github.com/khudayarovich/uz-alpha/releases/latest/download/UzAlpha-macOS.zip)** ·
[Barça versiyalar / all releases](https://github.com/khudayarovich/uz-alpha/releases)

---

## Özbekça

UzAlpha tizimning klaviaturalar röyxatiga ikkita klaviatura qöşadi:

| Klaviatura | Qanday yozadi |
|---|---|
| **UZA** — Özbekça (lotin, avtomatik) | Oddiy inglizça klaviatura, lekin yozayotganda özi almaştiradi: `sh` → ş, `ch` → ç, `o'` yoki `` o` `` → ö, `g'` yoki `` g` `` → ğ, harfdan keyingi `'` → ʼ. Almaştirişdan söng darhol Backspace bosilsa, oddiy harflar qaytadi (ş → sh). |
| **UZM** — Özbekça (lotin, qölda) | [kiritish.uz](https://kiritish.uz) terilmasi: `;` → ş, `'` → ç, `[` → ö, `]` → ğ, `` ` `` → ʼ. |

Windows’da tanlangan matnni ham ögiradi: **Ctrl+Alt+Shift+Y** — yangi alifboga, **Ctrl+Alt+Shift+E** — eski alifboga.

Boşqa klaviatura (inglizça, rusça…) tanlangan paytda UzAlpha heç narsani özgartirmaydi.

### Windows’ga örnatiş

1. [UzAlpha.exe](https://github.com/khudayarovich/uz-alpha/releases/latest/download/UzAlpha.exe) faylini yuklab oling va işga tuşiring.
2. «Windows protected your PC» oynasi çiqsa: **More info → Run anyway** (dastur hali raqamli imzolanmagan).
3. **Örnatiş** tugmasini bosing va Windows sörovini tasdiqlang.
4. Klaviaturani **Win + Space** orqali yoki UzAlpha oynasidagi **Avtomatik / Qölda** tugmalari bilan tanlang.

UzAlpha’ni Start menyusidan yoki iş stolidagi yorliqdan oçiş mumkin; özi soat yonidagi belgiçalar orasida turadi. Windows’ning tillar röyxatida klaviaturalar **UZA** va **UZM** belgisi bilan körinadi. Yangi versiya çiqsa, uni özi tekşirib yuklab oladi — **Yangilaş** tugmasini bosiş kifoya.
Öçiriş uçun: oynadagi **Öçiriş** tugmasi yoki Settings → Apps → UzAlpha.

### macOS’ga örnatiş

1. [UzAlpha-macOS.zip](https://github.com/khudayarovich/uz-alpha/releases/latest/download/UzAlpha-macOS.zip) faylini yuklab, oçing.
2. `install.command` faylini işga tuşiring (öng tugma → **Open**).
3. Tizimdan çiqib, qayta kiring.
4. System Settings → Keyboard → Input Sources → Edit → **+** orqali ikkala klaviaturani qöşing.

macOS versiyasida Backspace bilan bekor qiliş yöq, `'` esa har doim ʼ yozadi (oddiy `'` — Option+`'`).

---

## English

UzAlpha adds two keyboards to the system keyboard list:

| Keyboard | How it types |
|---|---|
| **UZA** — Özbekça (lotin, avtomatik), automatic | A normal English keyboard that converts as you type: `sh`→ş, `ch`→ç, `o'` or `` o` ``→ö, `g'` or `` g` ``→ğ, and `'` after a letter →ʼ. Backspace right after a conversion gives the plain letters back. |
| **UZM** — Özbekça (lotin, qölda), manual | The [kiritish.uz](https://kiritish.uz) layout: `;`→ş, `'`→ç, `[`→ö, `]`→ğ, `` ` ``→ʼ. |

On Windows it also converts **selected text** in any program: `Ctrl+Alt+Shift+Y` to the new alphabet, `Ctrl+Alt+Shift+E` back to the old one. Nothing is converted while another keyboard is selected.

Both keyboards are modes of one Windows text service. In the Windows keyboard switcher they carry the codes **UZA** and **UZM**. These are icons made of the letters only: Windows writes its own code (UZB) just for plain layouts, one per language.

### Install

**Windows 10/11 (x64).** Download and run `UzAlpha.exe`, press **Örnatiş** and approve the Windows prompt. The binaries are not code-signed yet, so SmartScreen warns on first run (More info → Run anyway). Windows on ARM is not supported yet. Remove it with **Öçiriş** in the window or from Settings → Apps.

`UzAlpha.exe /install /quiet` installs without opening the window; `UzAlpha.exe /uninstall /quiet` removes it.

**macOS.** Unzip `UzAlpha-macOS.zip`, run `install.command`, log out and in, then add both keyboards under System Settings → Keyboard → Input Sources. In this file-based version there is no Backspace-undo, `'` always types ʼ (plain `'` is on Option+`'`), and the first letter of a pair is shown highlighted until the next key.

### Updates

The Windows app looks at this repository's latest release a few times a day (it can be turned off in the window). If there is a newer version it downloads that `UzAlpha.exe`, checks it against the SHA-256 published with the release, and offers **Yangilaş**; installing still needs the Windows permission prompt. Nothing about you or your PC is sent — these are plain downloads from GitHub.

### Building

Needs Visual Studio Build Tools (C++ x64/x86 with the Windows SDK) and Node.js.

```
build.cmd          builds build\app\UzAlpha.exe (the DLLs are embedded in it)
build.cmd test     also runs the automated tests
```

| Folder | What |
|---|---|
| `src/engine` | The rules in plain C++: `engine` (automatic typing), `manual` (the manual keys), `translit` (whole text). |
| `src/tip` | Both keyboards: a Windows text service (TSF) with two profiles, built on the engine. |
| `src/app` | Tray icon, window, installer, updater, selected-text shortcut. |
| `mac` | The macOS keyboard bundle, generated by `tools/gen-mac-layouts.mjs`. |
| `tests` | Engine tests, a harness that drives the text service against a real document without installing it, and `tsf_diag`. |

A release is made by setting the version in `src/common/version.h` and pushing a tag `vX.Y.Z`; GitHub Actions builds and tests on Windows, type-tests the layouts on macOS, and publishes the files.

The app keeps a log of installs, switches and update checks in `%LOCALAPPDATA%\UzAlpha\uzalpha.log`.

### Credits and licence

Developed by Farrukh Yuldashev · fashuzfy98@gmail.com. MIT licence — see [LICENSE](LICENSE).

The manual key arrangement and the macOS layout base come from [kiritish.uz](https://github.com/itsbilolbek/kiritish.uz) by Bilolbek Normuminov, MIT licence — see `third_party/kiritish/LICENSE`.
