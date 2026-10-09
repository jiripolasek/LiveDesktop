#include "pch.h"
#include "Util.h"
#include "Log.h"

using namespace std::chrono;

namespace ld
{
    winrt::Windows::UI::Color Argb(uint8_t a, uint32_t rgb)
    {
        return winrt::Windows::UI::Color{ a, static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb) };
    }

    winrt::Windows::Foundation::TimeSpan Seconds(float s)
    {
        return duration_cast<winrt::Windows::Foundation::TimeSpan>(duration<float>(s));
    }

    std::wstring Utf8ToWide(std::string_view s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    std::string WideToUtf8(std::wstring_view s)
    {
        if (s.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string a(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), a.data(), n, nullptr, nullptr);
        return a;
    }

    std::wstring Trim(std::wstring_view s)
    {
        size_t b = 0, e = s.size();
        while (b < e && iswspace(s[b])) ++b;
        while (e > b && iswspace(s[e - 1])) --e;
        return std::wstring(s.substr(b, e - b));
    }

    std::wstring ToLower(std::wstring s)
    {
        for (auto& ch : s) ch = static_cast<wchar_t>(towlower(ch));
        return s;
    }

    std::wstring Truncate(std::wstring const& s, size_t maxChars)
    {
        if (s.size() <= maxChars) return s;
        return Trim(std::wstring_view(s).substr(0, maxChars - 1)) + L"\u2026";
    }

    uint32_t Hash(std::wstring_view s)
    {
        uint32_t h = 2166136261u;
        for (wchar_t ch : s) { h ^= static_cast<uint32_t>(ch); h *= 16777619u; }
        return h;
    }

    namespace
    {
        int64_t DaysFromCivil(int y, unsigned m, unsigned d)
        {
            y -= m <= 2;
            const int64_t era = (y >= 0 ? y : y - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + static_cast<int64_t>(doe) - 719468;
        }

        bool ReadInt(std::string_view s, size_t& i, size_t digits, int& out)
        {
            if (i + digits > s.size()) return false;
            out = 0;
            for (size_t k = 0; k < digits; ++k)
            {
                char ch = s[i + k];
                if (ch < '0' || ch > '9') return false;
                out = out * 10 + (ch - '0');
            }
            i += digits;
            return true;
        }
    }

    bool ParseIsoTime(std::string_view s, TimePoint& out)
    {
        size_t i = 0;
        int y, mo, d, h = 0, mi = 0, sec = 0;
        if (!ReadInt(s, i, 4, y)) return false;
        if (i < s.size() && s[i] == '-') ++i;
        if (!ReadInt(s, i, 2, mo)) return false;
        if (i < s.size() && s[i] == '-') ++i;
        if (!ReadInt(s, i, 2, d)) return false;
        if (i < s.size() && (s[i] == 'T' || s[i] == ' '))
        {
            ++i;
            if (!ReadInt(s, i, 2, h)) return false;
            if (i < s.size() && s[i] == ':') ++i;
            if (!ReadInt(s, i, 2, mi)) return false;
            if (i < s.size() && s[i] == ':') ++i;
            ReadInt(s, i, 2, sec);
        }
        double frac = 0.0;
        if (i < s.size() && s[i] == '.')
        {
            ++i;
            double scale = 0.1;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') { frac += (s[i] - '0') * scale; scale *= 0.1; ++i; }
        }
        int offsetMinutes = 0;
        if (i < s.size() && (s[i] == '+' || s[i] == '-'))
        {
            int sign = s[i] == '-' ? -1 : 1;
            ++i;
            int oh = 0, om = 0;
            if (ReadInt(s, i, 2, oh))
            {
                if (i < s.size() && s[i] == ':') ++i;
                ReadInt(s, i, 2, om);
            }
            offsetMinutes = sign * (oh * 60 + om);
        }
        int64_t days = DaysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
        int64_t secs = days * 86400 + h * 3600 + mi * 60 + sec - offsetMinutes * 60;
        out = TimePoint(duration_cast<Clock::duration>(seconds(secs) + duration<double>(frac)));
        return true;
    }

    TimePoint FromFileTime(FILETIME const& ft)
    {
        ULARGE_INTEGER u{ ft.dwLowDateTime, ft.dwHighDateTime };
        constexpr uint64_t kEpochDiff = 116444736000000000ull; // 1601 -> 1970 in 100ns
        int64_t ticks = static_cast<int64_t>(u.QuadPart) - static_cast<int64_t>(kEpochDiff);
        return TimePoint(duration_cast<Clock::duration>(duration<int64_t, std::ratio<1, 10000000>>(ticks)));
    }

    TimePoint FromLocal(SYSTEMTIME const& local)
    {
        SYSTEMTIME utc{};
        if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) utc = local;
        FILETIME ft{};
        SystemTimeToFileTime(&utc, &ft);
        return FromFileTime(ft);
    }

    SYSTEMTIME ToLocal(TimePoint t)
    {
        auto ticks = duration_cast<duration<int64_t, std::ratio<1, 10000000>>>(t.time_since_epoch()).count() + 116444736000000000ll;
        ULARGE_INTEGER u{};
        u.QuadPart = static_cast<uint64_t>(ticks);
        FILETIME ft{ u.LowPart, u.HighPart };
        SYSTEMTIME utc{}, local{};
        FileTimeToSystemTime(&ft, &utc);
        if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) local = utc;
        return local;
    }

    TimePoint StartOfLocalDay(TimePoint t)
    {
        SYSTEMTIME st = ToLocal(t);
        st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
        return FromLocal(st);
    }

    std::wstring FormatAge(seconds d)
    {
        auto s = std::max<int64_t>(0, d.count());
        wchar_t buf[32];
        if (s < 45) return L"now";
        if (s < 3600) swprintf_s(buf, L"%lldm", std::max<int64_t>(1, (s + 30) / 60));
        else if (s < 86400) swprintf_s(buf, L"%lldh %02lldm", s / 3600, (s % 3600) / 60);
        else swprintf_s(buf, L"%lldd", s / 86400);
        return buf;
    }

    std::wstring FormatClock(TimePoint t)
    {
        SYSTEMTIME st = ToLocal(t);
        wchar_t buf[16];
        swprintf_s(buf, L"%02u:%02u", st.wHour, st.wMinute);
        return buf;
    }

    std::wstring KnownFolder(REFKNOWNFOLDERID id)
    {
        PWSTR path = nullptr;
        std::wstring result;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path))) result = path;
        CoTaskMemFree(path);
        return result;
    }

    bool RunProcess(std::wstring commandLine, DWORD timeoutMs, ProcessResult& result, std::atomic<bool> const* cancel)
    {
        result = {};
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
        if (!CreatePipe(&outRead, &outWrite, &sa, 1 << 16)) return false;
        if (!CreatePipe(&errRead, &errWrite, &sa, 1 << 14))
        {
            CloseHandle(outRead); CloseHandle(outWrite);
            return false;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = outWrite;
        si.hStdError = errWrite;
        PROCESS_INFORMATION pi{};
        BOOL started = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi);
        CloseHandle(outWrite);
        CloseHandle(errWrite);
        if (!started)
        {
            CloseHandle(outRead); CloseHandle(errRead);
            return false;
        }

        auto drain = [](HANDLE pipe, std::string& sink)
        {
            DWORD avail = 0;
            while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0)
            {
                char buf[8192];
                DWORD read = 0;
                if (!ReadFile(pipe, buf, std::min<DWORD>(avail, sizeof(buf)), &read, nullptr) || read == 0) break;
                sink.append(buf, read);
            }
        };

        ULONGLONG deadline = GetTickCount64() + timeoutMs;
        bool ok = true;
        for (;;)
        {
            drain(outRead, result.out);
            drain(errRead, result.err);
            if (WaitForSingleObject(pi.hProcess, 25) == WAIT_OBJECT_0)
            {
                drain(outRead, result.out);
                drain(errRead, result.err);
                break;
            }
            if (GetTickCount64() > deadline || (cancel && cancel->load()))
            {
                TerminateProcess(pi.hProcess, 1);
                ok = false;
                break;
            }
        }
        GetExitCodeProcess(pi.hProcess, &result.exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(outRead);
        CloseHandle(errRead);
        return ok;
    }

    namespace
    {
        std::wstring ProcessImageName(DWORD pid)
        {
            std::wstring name;
            if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
            {
                wchar_t path[MAX_PATH]{};
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(process, 0, path, &size)) name = std::filesystem::path(path).filename().wstring();
                CloseHandle(process);
            }
            return name;
        }

        // Topmost (in z-order) visible, unowned, titled top-level window of a process with this image name.
        HWND FindAppWindow(std::wstring const& image)
        {
            struct Search { std::wstring const* image; HWND found; } search{ &image, nullptr };
            EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
                auto& s = *reinterpret_cast<Search*>(param);
                if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || GetWindowTextLengthW(hwnd) == 0) return TRUE;
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (_wcsicmp(ProcessImageName(pid).c_str(), s.image->c_str()) != 0) return TRUE;
                s.found = hwnd;
                return FALSE;
            }, reinterpret_cast<LPARAM>(&search));
            return search.found;
        }

        // The click that triggered the launch went to Explorer's icon layer, so we don't own the foreground and
        // the launched (or already running, single-instance) app can't take it either. Borrow the foreground
        // thread's input state to hand it over.
        bool ForceForeground(HWND target)
        {
            if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
            if (GetForegroundWindow() == target) return true;
            DWORD self = GetCurrentThreadId();
            DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
            bool attached = fgThread && fgThread != self && AttachThreadInput(self, fgThread, TRUE);
            BringWindowToTop(target);
            SetForegroundWindow(target);
            if (attached) AttachThreadInput(self, fgThread, FALSE);
            if (GetForegroundWindow() == target) return true;

            // Fallback: a synthetic Alt tap lifts the foreground lock for the injecting thread.
            INPUT keys[2]{};
            keys[0].type = keys[1].type = INPUT_KEYBOARD;
            keys[0].ki.wVk = keys[1].ki.wVk = VK_MENU;
            keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &keys[0], sizeof(INPUT));
            SetForegroundWindow(target);
            SendInput(1, &keys[1], sizeof(INPUT));
            return GetForegroundWindow() == target;
        }

        std::wstring HandlerCommand(std::wstring const& scheme)
        {
            wchar_t command[1024]{};
            DWORD size = sizeof(command);
            std::wstring key = scheme + L"\\shell\\open\\command";
            if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, command, &size) != ERROR_SUCCESS) return {};
            return command;
        }

        void LaunchAndFocus(std::wstring url)
        {
            HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            AllowSetForegroundWindow(ASFW_ANY);
            bool ghapp = url.rfind(L"ghapp://", 0) == 0;

            SHELLEXECUTEINFOW sei{ sizeof(sei) };
            sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
            sei.lpVerb = L"open";
            sei.lpFile = url.c_str();
            sei.nShow = SW_SHOWNORMAL;
            BOOL ok = ShellExecuteExW(&sei);
            DWORD error = ok ? ERROR_SUCCESS : GetLastError();
            DWORD pid = sei.hProcess ? GetProcessId(sei.hProcess) : 0;
            Log(L"Open: ShellExecuteEx(%s) ok=%d err=%lu hInstApp=%Id pid=%lu", url.c_str(), ok ? 1 : 0, error,
                reinterpret_cast<INT_PTR>(sei.hInstApp), pid);
            if (sei.hProcess)
            {
                AllowSetForegroundWindow(pid);
                CloseHandle(sei.hProcess);
            }

            if (!ok && ghapp)
            {
                // Fall back to the registered handler, run directly with the URL substituted for %1.
                std::wstring command = HandlerCommand(L"ghapp");
                if (auto at = command.find(L"%1"); at != std::wstring::npos) command.replace(at, 2, url);
                else if (!command.empty()) command += L" \"" + url + L"\"";
                STARTUPINFOW si{ sizeof(si) };
                PROCESS_INFORMATION pi{};
                BOOL created = !command.empty() && CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
                Log(L"Open: fallback CreateProcess(%s) ok=%d err=%lu", command.c_str(), created ? 1 : 0, created ? 0 : GetLastError());
                if (created)
                {
                    AllowSetForegroundWindow(pi.dwProcessId);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            }

            if (ghapp)
            {
                // The app is single-instance: the launched process hands the link to the running window and exits,
                // and the running window may restore/minimise itself while it handles it. Keep the window in front
                // for a few seconds, but only take focus back from the shell, never from another app.
                auto shellOwnsForeground = [] {
                    HWND fg = GetForegroundWindow();
                    if (!fg) return true;
                    wchar_t cls[64]{};
                    GetClassNameW(fg, cls, 64);
                    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Shell_TrayWnd") == 0 ||
                        wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
                };
                ULONGLONG start = GetTickCount64(), focusedAt = 0;
                HWND window = nullptr;
                int asserts = 0;
                while (GetTickCount64() - start < 8000)
                {
                    if (!window) window = FindAppWindow(L"github.exe");
                    if (window && !IsWindow(window)) window = nullptr;
                    if (window)
                    {
                        bool inFront = GetForegroundWindow() == window && !IsIconic(window);
                        if (focusedAt)
                        {
                            // A minimise or a switch to another app after we handed focus over is the user's call;
                            // only take focus back if the shell (desktop/taskbar) grabbed it.
                            if (GetTickCount64() - focusedAt > 1500) break;
                            if (!inFront)
                            {
                                if (IsIconic(window) || !shellOwnsForeground()) break;
                                ++asserts;
                                ForceForeground(window);
                            }
                        }
                        else
                        {
                            ++asserts;
                            if (inFront || ForceForeground(window)) focusedAt = GetTickCount64();
                        }
                    }
                    Sleep(100);
                }
                bool focused = window && GetForegroundWindow() == window;
                Log(L"Open: GitHub Copilot window %p %s after %llu ms (%d focus attempt(s))", window,
                    focused ? L"in foreground" : (focusedAt ? L"was focused, then left by user" : L"NOT focused"),
                    GetTickCount64() - start, asserts);
            }
            if (SUCCEEDED(co)) CoUninitialize();
        }
    }

    void OpenUrl(std::wstring const& url)
    {
        // Only hand well-formed URLs we built ourselves to the shell.
        if (url.rfind(L"https://", 0) != 0 && url.rfind(L"http://localhost:", 0) != 0 && url.rfind(L"ghapp://", 0) != 0)
        {
            Log(L"Open: refused '%s'", url.c_str());
            return;
        }
        // Off the UI thread: ShellExecute can block on the handler, and focusing the app polls.
        std::thread(LaunchAndFocus, url).detach();
    }

    void OpenFolder(std::wstring const& path)
    {
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return;
        ShellExecuteW(nullptr, L"explore", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
