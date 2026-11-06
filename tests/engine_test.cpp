// Unit tests for the conversion engine and the whole-text transliteration.
// The harness drives the engine exactly the way the text service does:
// OnTestKeyDown only looks (peek) and claims every key that has work for the
// document; OnKeyDown (apply) does that work.
#include <cstdio>
#include <string>

#include "../src/engine/engine.h"
#include "../src/engine/manual.h"
#include "../src/engine/translit.h"

using namespace uza;

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

static void fail(const char* what, const std::wstring& input) {
    ++g_failed;
    printf("FAIL  %s  (input \"%s\")\n", what, esc(input).c_str());
}

// Keys in a test string: \b = Backspace, \x01 = some other key (arrow, Ctrl+X...),
// \x02 = a modifier pressed on its own (Shift).
static Key toKey(wchar_t c) {
    if (c == L'\b') return Key::Backspace();
    if (c == 1) return Key::Other();
    if (c == 2) return Key::ModifierOnly();
    return Key::Char(c);
}

static bool same(const Action& a, const Action& b) {
    return a.eat == b.eat && a.commit == b.commit && a.edit == b.edit &&
           std::wstring(a.text) == std::wstring(b.text);
}

struct Doc {
    std::wstring out;   // committed text
    std::wstring comp;  // text of the open composition
    bool open = false;
    bool bad = false;

    void perform(const Action& a) {
        if (a.commit) {
            if (!open) bad = true;
            out += comp;
            comp.clear();
            open = false;
        }
        switch (a.edit) {
            case Edit::None: break;
            case Edit::Start:
                if (open) bad = true;
                comp = a.text;
                open = true;
                break;
            case Edit::Replace:
                if (!open) bad = true;
                comp = a.text;
                break;
            case Edit::ReplaceAndEnd:
                if (!open) bad = true;
                out += a.text;
                comp.clear();
                open = false;
                break;
            case Edit::Insert:
                if (open) bad = true;
                out += a.text;
                break;
        }
    }
};

static std::wstring type(const std::wstring& keys) {
    Engine eng;
    Doc doc;
    for (wchar_t c : keys) {
        const Key k = toKey(c);
        const Action test = eng.peek(k);  // OnTestKeyDown: looks, changes nothing
        if (!same(eng.peek(k), test)) fail("peek is not stable", keys);

        if (test.eat || test.commit) {
            // Claimed: the document is only ever changed in OnKeyDown.
            const Action a = eng.apply(k);
            if (!same(a, test)) fail("peek and apply disagree", keys);
            doc.perform(a);
            if (!a.eat) {
                // The open letter is finished; now the application gets the key.
                // Only a key that types nothing may travel this way: a character
                // handed back here could overtake the letter ("server" -> "esrver").
                if (k.kind != KeyKind::Other) fail("a typing key was handed back while a letter was open", keys);
                if (doc.open) fail("key passed through while a composition is open", keys);
            }
        } else {
            const Action a = eng.apply(k);  // the engine only takes note of the key
            if (!a.nothing()) fail("apply acts on a key that peek passed", keys);
            const Action again = eng.apply(k);  // a host may ask about the same key twice
            if (!again.nothing()) fail("second apply is not a no-op", keys);
            // The application handles the key itself.
            if (doc.open && k.kind != KeyKind::ModifierOnly) {
                fail("key passed through while a composition is open", keys);
            }
            if (k.kind == KeyKind::Char) doc.out += k.ch;
            if (k.kind == KeyKind::Backspace && !doc.out.empty()) doc.out.pop_back();
        }
        if (eng.composing() != doc.open) fail("engine and document disagree on composition", keys);
    }
    if (doc.bad) fail("invalid edit sequence", keys);
    return doc.out + doc.comp;
}

static void expectTyped(const wchar_t* keys, const wchar_t* want) {
    ++g_checks;
    const std::wstring got = type(keys);
    if (got != want) {
        ++g_failed;
        printf("FAIL  typed \"%s\": got \"%s\", want \"%s\"\n", esc(keys).c_str(), esc(got).c_str(),
               esc(want).c_str());
    }
}

static void expectText(const char* name, const std::wstring& got, const wchar_t* want) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        printf("FAIL  %s: got \"%s\", want \"%s\"\n", name, esc(got).c_str(), esc(want).c_str());
    }
}

int main() {
    // --- the four digraphs -------------------------------------------------
    expectTyped(L"shahar", L"şahar");
    expectTyped(L"choy", L"çoy");
    expectTyped(L"o'zbek", L"özbek");
    expectTyped(L"g'isht", L"ğişt");
    expectTyped(L"bosh ", L"boş ");

    // --- the backtick works like the apostrophe after o and g ----------------
    expectTyped(L"o`zbek", L"özbek");
    expectTyped(L"g`isht", L"ğişt");
    expectTyped(L"O`zbek", L"Özbek");
    expectTyped(L"to`g`ri", L"töğri");
    expectTyped(L"o`\b", L"o`");
    expectTyped(L"a`b", L"a`b");  // elsewhere it stays a backtick
    expectTyped(L"s`", L"s`");
    // ...and so do the curly and turned marks, should a keyboard deliver them
    expectTyped(L"o‘zbek", L"özbek");
    expectTyped(L"g’isht", L"ğişt");
    expectTyped(L"Oʻzbek", L"Özbek");

    // --- chains: a pending letter commits when the next starter arrives ----
    expectTyped(L"tog'", L"toğ");
    expectTyped(L"to'g'ri", L"töğri");
    expectTyped(L"o'g'il", L"öğil");
    expectTyped(L"sch", L"sç");
    expectTyped(L"ss", L"ss");
    expectTyped(L"ssh", L"sş");
    expectTyped(L"mashhur", L"maşhur");

    // --- case follows the first letter --------------------------------------
    expectTyped(L"Shahar", L"Şahar");
    expectTyped(L"SHAHAR", L"ŞAHAR");
    expectTyped(L"sH", L"ş");
    expectTyped(L"Choy", L"Çoy");
    expectTyped(L"O'zbek", L"Özbek");
    expectTyped(L"G'isht", L"Ğişt");
    expectTyped(L"s\x02H", L"ş");  // Shift pressed between the two letters

    // --- pending letter on its own ------------------------------------------
    expectTyped(L"s", L"s");
    expectTyped(L"sa", L"sa");
    expectTyped(L"c", L"c");
    expectTyped(L"oh", L"oh");
    expectTyped(L"s'", L"sʼ");

    // --- tutuq belgisi -------------------------------------------------------
    expectTyped(L"ma'no", L"maʼno");
    expectTyped(L"a'", L"aʼ");
    expectTyped(L"a''", L"aʼ'");
    expectTyped(L"o''", L"öʼ");
    expectTyped(L"mo''jiza", L"möʼjiza");
    expectTyped(L"is'hoq", L"isʼhoq");
    expectTyped(L"'", L"'");
    expectTyped(L" 'a", L" 'a");
    expectTyped(L"1'", L"1'");

    // --- Backspace ------------------------------------------------------------
    expectTyped(L"sh\b", L"sh");
    expectTyped(L"Sh\b", L"Sh");
    expectTyped(L"SH\b", L"SH");
    expectTyped(L"ch\b", L"ch");
    expectTyped(L"o'\b", L"o'");
    expectTyped(L"g'\b", L"g'");
    expectTyped(L"sh\bh", L"shh");
    expectTyped(L"ch\b\b", L"c");
    expectTyped(L"s\b", L"");
    expectTyped(L"as\b", L"a");
    expectTyped(L"ab\b", L"a");
    expectTyped(L"sha\b", L"ş");  // only an immediate Backspace undoes
    expectTyped(L"a\b'", L"'");        // context unknown after a plain Backspace

    // --- other keys break a pending digraph -----------------------------------
    expectTyped(L"s\x01h", L"sh");
    expectTyped(L"o\x01'", L"o'");
    expectTyped(L"sh\x01\b", L"");  // after another key Backspace is a plain delete

    // --- the Manual keyboard: keys by scan code (shift, caps, altGr) -------------
    struct ManualCase {
        unsigned scan;
        bool shift, caps, altGr;
        wchar_t want;
    };
    const ManualCase manualCases[] = {
        {0x27, false, false, false, 0x015F}, {0x27, true, false, false, 0x015E},   // ; ş Ş
        {0x27, false, true, false, 0x015E},  {0x27, true, true, false, 0x015F},    // CapsLock like a letter
        {0x28, false, false, false, 0x00E7}, {0x28, true, false, false, 0x00C7},   // ' ç Ç
        {0x1A, false, false, false, 0x00F6}, {0x1A, true, false, false, 0x00D6},   // [ ö Ö
        {0x1B, false, false, false, 0x011F}, {0x1B, true, false, false, 0x011E},   // ] ğ Ğ
        {0x29, false, false, false, 0x02BC}, {0x29, true, false, false, L'"'},     // ` ʼ "
        {0x29, false, true, false, 0x02BC},                                        // CapsLock does not touch it
        {0x2B, false, false, false, L';'},   {0x2B, true, false, false, L':'},     // \ ; :
        {0x56, false, false, false, L';'},                                         // the extra ISO key
        {0x1E, false, false, false, 0},      {0x02, true, false, false, 0},        // a, ! are not remapped
        {0x25, false, false, true, 0x201C},  {0x25, true, false, true, 0x2018},    // AltGr K “ ‘
        {0x26, false, false, true, 0x201D},  {0x26, true, false, true, 0x2019},    // AltGr L ” ’
        {0x0C, false, false, true, 0x2014},  {0x0C, true, false, true, 0x2013},    // AltGr - — –
        {0x1A, false, false, true, L'['},    {0x1B, true, false, true, L'}'},      // AltGr [ and Shift ]
        {0x2B, false, false, true, L'\\'},   {0x2B, true, false, true, L'|'},
        {0x28, false, false, true, L'\''},   {0x29, false, false, true, L'`'},     {0x29, true, false, true, L'~'},
        {0x1E, false, false, true, 0},                                             // AltGr a: nothing
    };
    for (const ManualCase& c : manualCases) {
        ++g_checks;
        const wchar_t got = manualChar(c.scan, c.shift, c.caps, c.altGr);
        if (got != c.want) {
            ++g_failed;
            printf("FAIL  manual key %02x shift=%d caps=%d altgr=%d: got U+%04X, want U+%04X\n", c.scan, c.shift,
                   c.caps, c.altGr, (unsigned)got, (unsigned)c.want);
        }
    }

    // --- whole-text conversion --------------------------------------------------
    expectText("new 1", toNewAlphabet(L"O'zbekiston"), L"Özbekiston");
    expectText("new 2", toNewAlphabet(L"Oʻzbek"), L"Özbek");
    expectText("new 3", toNewAlphabet(L"g‘isht"), L"ğişt");
    expectText("new 4", toNewAlphabet(L"SHAHAR Shahar shahar"), L"ŞAHAR Şahar şahar");
    expectText("new 5", toNewAlphabet(L"Choy ichish"), L"Çoy içiş");
    expectText("new 6", toNewAlphabet(L"ma'no"), L"maʼno");
    expectText("new 7", toNewAlphabet(L"Is'hoq"), L"Isʼhoq");
    expectText("new 8", toNewAlphabet(L"toʻgʻri"), L"töğri");
    expectText("new 9", toNewAlphabet(L"'salom' 5'"), L"'salomʼ 5'");
    expectText("new 10", toNewAlphabet(L"şahar"), L"şahar");

    expectText("old 1", toOldAlphabet(L"Özbekiston"), L"Oʻzbekiston");
    expectText("old 2", toOldAlphabet(L"şahar"), L"shahar");
    expectText("old 3", toOldAlphabet(L"ŞAHAR"), L"SHAHAR");
    expectText("old 4", toOldAlphabet(L"Şahar"), L"Shahar");
    expectText("old 5", toOldAlphabet(L"BOŞ"), L"BOSH");
    expectText("old 6", toOldAlphabet(L"çoy Çoy ÇOY"), L"choy Choy CHOY");
    expectText("old 7", toOldAlphabet(L"töğri"), L"toʻgʻri");
    expectText("old 8", toOldAlphabet(L"maʼno"), L"maʼno");
    expectText("old 9", toOldAlphabet(L"Ş"), L"Sh");

    const wchar_t* samples[] = {
        L"Özbekiston Respublikasi", L"Yangi özbek alifbosi uçun",
        L"ŞAHAR VA QIŞLOQ",    L"töğri maʼno",
    };
    for (const wchar_t* s : samples) {
        expectText("round trip", toNewAlphabet(toOldAlphabet(s)), s);
    }

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
