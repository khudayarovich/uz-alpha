// Compares two keyboard layout DLLs table by table.
//   kbd_test.exe <ours.dll> <reference.dll>
// Used to prove the Manual layout is identical to the kiritish.uz layout.
// Build it for x86 with /DBUILD_WOW6432 to compare the SysWOW64 variants.
#define KBD_TYPE 4

#include <windows.h>
#include <kbd.h>

#include <cstdio>
#include <map>
#include <set>
#include <string>

// Pointers inside the tables are 64-bit wide even in the 32-bit (WOW64) build;
// cutting them down to the native size there is intended.
#pragma warning(disable : 4302 4311)
#define NP(T, p) ((T*)(ULONG_PTR)(p))

static std::string utf8(const wchar_t* w) {
    if (!w) return "(null)";
    char buf[256];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, nullptr, nullptr);
    return n > 0 ? buf : "?";
}

static bool dump(const wchar_t* path, std::set<std::string>& out) {
    HMODULE h = LoadLibraryW(path);
    if (!h) {
        printf("cannot load %s (error %lu)\n", utf8(path).c_str(), GetLastError());
        return false;
    }
    typedef PKBDTABLES (*Fn)(void);
    Fn fn = (Fn)GetProcAddress(h, (LPCSTR)1);
    if (!fn) {
        printf("%s has no export with ordinal 1\n", utf8(path).c_str());
        return false;
    }
    const KBDTABLES* t = NP(const KBDTABLES, fn());
    char line[512];

    snprintf(line, sizeof line, "flags %08lx", t->fLocaleFlags);
    out.insert(line);
    snprintf(line, sizeof line, "ligatures max=%u cb=%u ptr=%s", t->nLgMax, t->cbLgEntry,
             t->pLigature ? "set" : "null");
    out.insert(line);
    snprintf(line, sizeof line, "deadkeys %s", t->pDeadKey ? "set" : "null");
    out.insert(line);
    snprintf(line, sizeof line, "deadnames %s", t->pKeyNamesDead ? "set" : "null");
    out.insert(line);

    const MODIFIERS* mods = NP(const MODIFIERS, t->pCharModifiers);
    for (const VK_TO_BIT* b = NP(const VK_TO_BIT, mods->pVkToBit); b->Vk; ++b) {
        snprintf(line, sizeof line, "modbit vk=%02x bits=%02x", b->Vk, b->ModBits);
        out.insert(line);
    }
    for (unsigned i = 0; i <= mods->wMaxModBits; ++i) {
        snprintf(line, sizeof line, "modnum[%u]=%u", i, mods->ModNumber[i]);
        out.insert(line);
    }

    std::map<unsigned, std::string> chars;
    for (const VK_TO_WCHAR_TABLE* wt = NP(const VK_TO_WCHAR_TABLE, t->pVkToWcharTable);
         wt->pVkToWchars; ++wt) {
        const BYTE* p = NP(const BYTE, wt->pVkToWchars);
        for (;; p += wt->cbSize) {
            const VK_TO_WCHARS1* e = (const VK_TO_WCHARS1*)p;
            if (!e->VirtualKey) break;
            std::string s;
            snprintf(line, sizeof line, "char vk=%02x attr=%02x:", e->VirtualKey, e->Attributes);
            s = line;
            for (unsigned c = 0; c < 8; ++c) {
                const WCHAR ch = c < wt->nModifications ? e->wch[c] : WCH_NONE;
                snprintf(line, sizeof line, " %04x", ch);
                s += line;
            }
            chars.emplace(e->VirtualKey, s);  // the first table that defines a key wins
        }
    }
    for (auto& kv : chars) out.insert(kv.second);

    snprintf(line, sizeof line, "vsc count %u", t->bMaxVSCtoVK);
    out.insert(line);
    const USHORT* vsc = NP(const USHORT, t->pusVSCtoVK);
    for (unsigned i = 0; i < t->bMaxVSCtoVK; ++i) {
        snprintf(line, sizeof line, "vsc %02x -> %04x", i, vsc[i]);
        out.insert(line);
    }
    for (const VSC_VK* e = NP(const VSC_VK, t->pVSCtoVK_E0); e->Vsc; ++e) {
        snprintf(line, sizeof line, "e0 %02x -> %04x", e->Vsc, e->Vk);
        out.insert(line);
    }
    for (const VSC_VK* e = NP(const VSC_VK, t->pVSCtoVK_E1); e->Vsc; ++e) {
        snprintf(line, sizeof line, "e1 %02x -> %04x", e->Vsc, e->Vk);
        out.insert(line);
    }
    for (const VSC_LPWSTR* n = NP(const VSC_LPWSTR, t->pKeyNames); n->vsc; ++n) {
        snprintf(line, sizeof line, "name %02x = %s", n->vsc, utf8(NP(const wchar_t, n->pwsz)).c_str());
        out.insert(line);
    }
    for (const VSC_LPWSTR* n = NP(const VSC_LPWSTR, t->pKeyNamesExt); n->vsc; ++n) {
        snprintf(line, sizeof line, "extname %02x = %s", n->vsc, utf8(NP(const wchar_t, n->pwsz)).c_str());
        out.insert(line);
    }
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        printf("usage: kbd_test <ours.dll> <reference.dll>\n");
        return 2;
    }
    std::set<std::string> ours, ref;
    if (!dump(argv[1], ours) || !dump(argv[2], ref)) return 2;

    int diffs = 0;
    for (auto& l : ours) {
        if (!ref.count(l)) {
            printf("only in ours:      %s\n", l.c_str());
            ++diffs;
        }
    }
    for (auto& l : ref) {
        if (!ours.count(l)) {
            printf("only in reference: %s\n", l.c_str());
            ++diffs;
        }
    }
    printf("%zu entries compared, %d differences\n", ours.size(), diffs);
    return diffs ? 1 : 0;
}
