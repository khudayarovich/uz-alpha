#include "manual.h"

namespace uza {

wchar_t manualChar(unsigned scan, bool shift, bool caps, bool altGr) {
    if (altGr) {
        switch (scan) {
            case 0x25: return shift ? 0x2018 : 0x201C;  // K: ‘ “
            case 0x26: return shift ? 0x2019 : 0x201D;  // L: ’ ”
            case 0x0C: return shift ? 0x2013 : 0x2014;  // -: – —
            case 0x1A: return shift ? L'{' : L'[';
            case 0x1B: return shift ? L'}' : L']';
            case 0x2B:
            case 0x56: return shift ? L'|' : L'\\';
            case 0x28: return shift ? 0 : L'\'';
            case 0x29: return shift ? L'~' : L'`';
        }
        return 0;
    }

    // The four letters follow CapsLock like any other letter.
    const bool upper = shift != caps;
    switch (scan) {
        case 0x27: return upper ? 0x015E : 0x015F;  // ; : Ş ş
        case 0x28: return upper ? 0x00C7 : 0x00E7;  // ' : Ç ç
        case 0x1A: return upper ? 0x00D6 : 0x00F6;  // [ : Ö ö
        case 0x1B: return upper ? 0x011E : 0x011F;  // ] : Ğ ğ
        case 0x29: return shift ? L'"' : 0x02BC;    // ` : " ʼ
        case 0x2B:
        case 0x56: return shift ? L':' : L';';      // \ : : ;
    }
    return 0;
}

}  // namespace uza
