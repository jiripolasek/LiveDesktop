#include "pch.h"
#include "Util.h"

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

    void OpenUrl(std::wstring const& url)
    {
        // Only hand well-formed URLs we built ourselves to the shell.
        if (url.rfind(L"https://", 0) != 0 && url.rfind(L"http://localhost:", 0) != 0 && url.rfind(L"ghapp://", 0) != 0) return;
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
