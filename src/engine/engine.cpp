#include "engine.h"

namespace uza {
namespace {

bool isStarter(wchar_t c) {
    switch (c) {
        case L's': case L'S': case L'c': case L'C':
        case L'o': case L'O': case L'g': case L'G':
            return true;
    }
    return false;
}

// Does `c` complete the digraph that `first` started?
bool completes(wchar_t first, wchar_t c) {
    switch (first) {
        case L's': case L'S': case L'c': case L'C':
            return c == L'h' || c == L'H';
        case L'o': case L'O': case L'g': case L'G':
            // People write the mark in oʻ many ways: o' o` o‘ o’ oʻ oʼ.
            return c == kApostrophe || c == kGrave || c == 0x2018 || c == 0x2019 || c == 0x02BB || c == kTutuq;
    }
    return false;
}

wchar_t converted(wchar_t first) {
    switch (first) {
        case L's': return 0x015F;  // ş
        case L'S': return 0x015E;  // Ş
        case L'c': return 0x00E7;  // ç
        case L'C': return 0x00C7;  // Ç
        case L'o': return 0x00F6;  // ö
        case L'O': return 0x00D6;  // Ö
        case L'g': return 0x011F;  // ğ
        case L'G': return 0x011E;  // Ğ
    }
    return first;
}

bool isLetter(wchar_t c) {
    if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) return true;
    switch (c) {
        case 0x015F: case 0x015E: case 0x00E7: case 0x00C7:
        case 0x00F6: case 0x00D6: case 0x011F: case 0x011E:
            return true;
    }
    return false;
}

void setText(Action& a, wchar_t c0, wchar_t c1 = 0) {
    a.text[0] = c0;
    a.text[1] = c1;
    a.text[2] = 0;
}

}  // namespace

void Engine::reset() {
    state_ = State::Idle;
    first_ = second_ = last_ = 0;
}

Action Engine::peek(const Key& k) const {
    Engine scratch = *this;
    return decide(k, scratch);
}

Action Engine::apply(const Key& k) {
    Engine next = *this;
    Action a = decide(k, next);
    *this = next;
    return a;
}

Action Engine::decide(const Key& k, Engine& next) const {
    Action a;
    const bool open = state_ != State::Idle;

    switch (k.kind) {
        case KeyKind::ModifierOnly:
            // Shift between `s` and `H` must not break the digraph.
            return a;

        case KeyKind::Other:
            a.commit = open;
            next.state_ = State::Idle;
            next.first_ = next.second_ = 0;
            next.last_ = 0;  // the caret may have moved
            return a;

        case KeyKind::Backspace:
            if (state_ == State::Pending) {
                // Delete the pending letter.
                a.eat = true;
                a.edit = Edit::ReplaceAndEnd;
                setText(a, 0);
                next.last_ = 0;
            } else if (state_ == State::Converted) {
                // Undo the conversion: ş -> "sh".
                a.eat = true;
                a.edit = Edit::ReplaceAndEnd;
                setText(a, first_, second_);
                next.last_ = second_;
            } else {
                next.last_ = 0;
            }
            next.state_ = State::Idle;
            next.first_ = next.second_ = 0;
            return a;

        case KeyKind::Char:
            break;
    }

    const wchar_t c = k.ch;

    if (state_ == State::Pending && completes(first_, c)) {
        a.eat = true;
        a.edit = Edit::Replace;
        setText(a, converted(first_));
        next.state_ = State::Converted;
        next.second_ = c;
        next.last_ = converted(first_);
        return a;
    }

    if (isStarter(c)) {
        a.eat = true;
        a.commit = open;
        a.edit = Edit::Start;
        setText(a, c);
        next.state_ = State::Pending;
        next.first_ = c;
        next.second_ = 0;
        next.last_ = c;
        return a;
    }

    next.state_ = State::Idle;
    next.first_ = next.second_ = 0;

    if (c == kApostrophe && isLetter(last_)) {
        // Tutuq belgisi: ma'no -> maʼno.
        a.eat = true;
        a.commit = open;
        a.edit = Edit::Insert;
        setText(a, kTutuq);
        next.last_ = kTutuq;
        return a;
    }

    // Any other character: let the application have the key.
    a.commit = open;
    next.last_ = c;
    return a;
}

}  // namespace uza
