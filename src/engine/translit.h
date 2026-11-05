// Whole-text conversion between the current ("old") Uzbek Latin alphabet
// (sh, ch, oʻ, gʻ) and the new one (ş, ç, ö, ğ). Used by the selected-text
// shortcut. Pure C++.
#pragma once

#include <string>

namespace uza {

// sh->ş, ch->ç, oʻ/o'->ö, gʻ/g'->ğ, apostrophe after a letter -> ʼ
std::wstring toNewAlphabet(const std::wstring& in);

// ş->sh, ç->ch, ö->oʻ, ğ->gʻ (case-aware: ŞAHAR -> SHAHAR, Şahar -> Shahar)
std::wstring toOldAlphabet(const std::wstring& in);

}  // namespace uza
