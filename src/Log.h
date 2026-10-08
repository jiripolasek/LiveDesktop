#pragma once
#include <string>

namespace ld
{
    // Initializes logging to LiveDesktop.log next to the exe (falls back to %TEMP%) and the console.
    void LogInit();
    void Log(_Printf_format_string_ const wchar_t* fmt, ...);
    std::wstring const& LogPath();
}
