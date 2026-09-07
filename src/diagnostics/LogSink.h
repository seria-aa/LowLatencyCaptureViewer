#pragma once

#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace llcv::diagnostics {
using LogSink = void (*)(const wchar_t* message);

// Route module diagnostics through the same sink as the application log.
// No process-wide stderr redirection or global callback lifetime is needed.
inline void LogMessage(LogSink sink, const wchar_t* format, ...) {
    wchar_t message[4096]{};
    va_list arguments;
    va_start(arguments, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, arguments);
    va_end(arguments);
    if (sink) sink(message);
    else std::fputws(message, stderr);
}
}  // namespace llcv::diagnostics
