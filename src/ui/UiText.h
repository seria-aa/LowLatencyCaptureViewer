#pragma once

namespace llcv::ui_text {

// Returns the original pointer for Korean/unknown text, or a process-lifetime
// translation. No language/settings globals are read by this module.
const wchar_t* Translate(const wchar_t* korean, bool useEnglish);

} // namespace llcv::ui_text
