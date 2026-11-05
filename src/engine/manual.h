// The Manual keyboard: the kiritish.uz arrangement, where the new letters have
// keys of their own. Pure C++.
#pragma once

namespace uza {

// What the Manual keyboard types for a physical key, given by its PC scan code
// (set 1), or 0 if the key is not remapped and types its usual US character.
//
//   ;  -> ş      '  -> ç      [  -> ö      ]  -> ğ      (Shift or CapsLock: capitals)
//   `  -> ʼ      Shift+` -> "      \  -> ;      Shift+\ -> :
//
// With AltGr (right Alt, or Ctrl+Alt) the keys that lost their symbols get
// them back, plus typographic quotes and dashes:
//   K L -> “ ”   Shift+K L -> ‘ ’   -  -> —   Shift+- -> –
//   [ ] -> [ ]   Shift+[ ] -> { }   \  -> \   Shift+\ -> |   ' -> '   ` -> `   Shift+` -> ~
wchar_t manualChar(unsigned scan, bool shift, bool caps, bool altGr);

}  // namespace uza
