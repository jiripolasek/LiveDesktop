#include "pch.h"
#include "Log.h"
#include <share.h>
#include <io.h>

namespace ld
{
    namespace
    {
        std::mutex g_mutex;
        FILE* g_file = nullptr;
        std::wstring g_path;
        HANDLE g_stdout = nullptr;
        bool g_isConsole = false;
    }

    void LogInit()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::filesystem::path path(exe);
        path.replace_filename(L"LiveDesktop.log");

        g_file = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
        if (!g_file)
        {
            wchar_t temp[MAX_PATH]{};
            GetTempPathW(MAX_PATH, temp);
            path = std::filesystem::path(temp) / L"LiveDesktop.log";
            g_file = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
        }
        g_path = path.wstring();

        g_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
        // A launcher can hand us a stale stdout handle value that now aliases our own log file.
        if (g_file && g_stdout == reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(g_file)))) g_stdout = nullptr;
        if (g_stdout && g_stdout != INVALID_HANDLE_VALUE && GetFileType(g_stdout) == FILE_TYPE_UNKNOWN) g_stdout = nullptr;
        DWORD mode = 0;
        g_isConsole = g_stdout && g_stdout != INVALID_HANDLE_VALUE && GetConsoleMode(g_stdout, &mode);
    }

    std::wstring const& LogPath() { return g_path; }

    void Log(const wchar_t* fmt, ...)
    {
        wchar_t msg[2048];
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(msg, _TRUNCATE, fmt, args);
        va_end(args);

        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t line[2200];
        int len = swprintf_s(line, L"[%02u:%02u:%02u.%03u] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        if (len < 0) return;

        std::lock_guard lock(g_mutex);
        if (g_file)
        {
            fputws(line, g_file);
            fflush(g_file);
        }
        if (g_isConsole)
        {
            DWORD written = 0;
            WriteConsoleW(g_stdout, line, static_cast<DWORD>(len), &written, nullptr);
        }
        else if (g_stdout && g_stdout != INVALID_HANDLE_VALUE)
        {
            char utf8[4400];
            int n = WideCharToMultiByte(CP_UTF8, 0, line, len, utf8, sizeof(utf8), nullptr, nullptr);
            DWORD written = 0;
            if (n > 0) WriteFile(g_stdout, utf8, static_cast<DWORD>(n), &written, nullptr);
        }
        OutputDebugStringW(line);
    }
}
