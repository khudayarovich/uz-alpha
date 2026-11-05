// Auto-mode conversion engine: sh->ş, ch->ç, o'->ö, g'->ğ (also o` and g`),
// '->ʼ after a letter.
// Pure C++ (no Windows dependencies) so it can be unit-tested on its own.
//
// The engine sees every key and answers with an Action that the host (the TSF
// text service, or the test harness) performs on the document. The first letter
// of a digraph lives in an open "composition"; the completing key replaces it
// with the new letter and keeps the composition open so that an immediate
// Backspace can turn it back into the literal letters.
#pragma once

namespace uza {

const wchar_t kApostrophe = L'\'';
const wchar_t kGrave = L'`';    // the other key people use for the mark in oʻ / gʻ
const wchar_t kTutuq = 0x02BC;  // ʼ MODIFIER LETTER APOSTROPHE

enum class KeyKind {
    Char,          // a printable character (see Key::ch)
    Backspace,
    Other,         // any other key, or a character key with Ctrl/Alt/Win held
    ModifierOnly,  // Shift, Ctrl, Alt, Win, CapsLock... on their own
};

struct Key {
    KeyKind kind;
    wchar_t ch;  // valid when kind == Char

    static Key Char(wchar_t c) { return {KeyKind::Char, c}; }
    static Key Backspace() { return {KeyKind::Backspace, 0}; }
    static Key Other() { return {KeyKind::Other, 0}; }
    static Key ModifierOnly() { return {KeyKind::ModifierOnly, 0}; }
};

enum class Edit {
    None,
    Start,          // open a composition holding `text`
    Replace,        // replace the open composition's text with `text`, keep it open
    ReplaceAndEnd,  // replace the open composition's text with `text` (may be empty), end it
    Insert,         // insert `text` as plain committed text (no composition is open)
};

struct Action {
    bool eat = false;     // the key is consumed; the application must not see it
    bool commit = false;  // first end the open composition, keeping its text
    Edit edit = Edit::None;
    wchar_t text[4] = {};

    bool nothing() const { return !eat && !commit && edit == Edit::None; }
};

class Engine {
public:
    // The decision for `k` without changing any state.
    Action peek(const Key& k) const;
    // The decision for `k`, and move to the next state.
    Action apply(const Key& k);
    // Forget everything (focus change, composition ended by the application).
    void reset();
    // Tell the engine which character really sits before the caret (0 = none).
    // Only used while idle, for the apostrophe rule; hosts that can read the
    // document call this, others rely on the engine's own memory.
    void setPrevious(wchar_t c) {
        if (state_ == State::Idle) last_ = c;
    }

    bool composing() const { return state_ != State::Idle; }

private:
    enum class State { Idle, Pending, Converted };

    Action decide(const Key& k, Engine& next) const;

    State state_ = State::Idle;
    wchar_t first_ = 0;   // Pending/Converted: the starter letter as typed
    wchar_t second_ = 0;  // Converted: the completing key as typed
    wchar_t last_ = 0;    // last character put into the document, 0 = unknown
};

}  // namespace uza
