#include "translit.h"

namespace uza {
namespace {

const wchar_t kTurnedComma = 0x02BB;  // ʻ  the mark in oʻ / gʻ
const wchar_t kTutuq = 0x02BC;        // ʼ  tutuq belgisi

const wchar_t kSmallS = 0x015F, kCapS = 0x015E;  // ş Ş
const wchar_t kSmallC = 0x00E7, kCapC = 0x00C7;  // ç Ç
const wchar_t kSmallO = 0x00F6, kCapO = 0x00D6;  // ö Ö
const wchar_t kSmallG = 0x011F, kCapG = 0x011E;  // ğ Ğ

bool isUpper(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || c == kCapS || c == kCapC || c == kCapO || c == kCapG;
}

bool isLetter(wchar_t c) {
    if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) return true;
    // Latin-1 Supplement and Latin Extended-A/B letters.
    return c >= 0x00C0 && c <= 0x024F && c != 0x00D7 && c != 0x00F7;
}

// Characters people use for the mark in oʻ / gʻ.
bool isOgMark(wchar_t c) {
    return c == L'\'' || c == kTurnedComma || c == kTutuq || c == 0x2018 || c == 0x2019 || c == L'`';
}

// Characters people use for the tutuq belgisi.
bool isTutuqMark(wchar_t c) {
    return c == L'\'' || c == kTurnedComma || c == kTutuq || c == 0x2018 || c == 0x2019;
}

// For Ş -> "SH" vs "Sh": is the letter at `i` inside an upper-case word?
bool upperContext(const std::wstring& s, size_t i) {
    if (i + 1 < s.size() && isLetter(s[i + 1])) return isUpper(s[i + 1]);
    if (i > 0 && isLetter(s[i - 1])) return isUpper(s[i - 1]);
    return false;
}

}  // namespace

std::wstring toNewAlphabet(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const wchar_t c = in[i];
        const wchar_t n = i + 1 < in.size() ? in[i + 1] : 0;
        const bool h = n == L'h' || n == L'H';

        if (c == L's' && h) { out += kSmallS; ++i; continue; }
        if (c == L'S' && h) { out += kCapS; ++i; continue; }
        if (c == L'c' && h) { out += kSmallC; ++i; continue; }
        if (c == L'C' && h) { out += kCapC; ++i; continue; }
        if (c == L'o' && isOgMark(n)) { out += kSmallO; ++i; continue; }
        if (c == L'O' && isOgMark(n)) { out += kCapO; ++i; continue; }
        if (c == L'g' && isOgMark(n)) { out += kSmallG; ++i; continue; }
        if (c == L'G' && isOgMark(n)) { out += kCapG; ++i; continue; }

        if (isTutuqMark(c) && !out.empty() && isLetter(out.back())) {
            out += kTutuq;
            continue;
        }
        out += c;
    }
    return out;
}

std::wstring toOldAlphabet(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + in.size() / 8);
    for (size_t i = 0; i < in.size(); ++i) {
        const wchar_t c = in[i];
        switch (c) {
            case kSmallS: out += L"sh"; break;
            case kCapS: out += upperContext(in, i) ? L"SH" : L"Sh"; break;
            case kSmallC: out += L"ch"; break;
            case kCapC: out += upperContext(in, i) ? L"CH" : L"Ch"; break;
            case kSmallO: out += L'o'; out += kTurnedComma; break;
            case kCapO: out += L'O'; out += kTurnedComma; break;
            case kSmallG: out += L'g'; out += kTurnedComma; break;
            case kCapG: out += L'G'; out += kTurnedComma; break;
            default: out += c; break;
        }
    }
    return out;
}

}  // namespace uza
