#pragma once
#include "pch.h"

namespace ld
{
    using Clock = std::chrono::system_clock;
    using TimePoint = Clock::time_point;

    winrt::Windows::UI::Color Argb(uint8_t a, uint32_t rgb);
    winrt::Windows::Foundation::TimeSpan Seconds(float s);

    std::wstring Utf8ToWide(std::string_view s);
    std::string WideToUtf8(std::wstring_view s);
    std::wstring Trim(std::wstring_view s);
    std::wstring ToLower(std::wstring s);
    std::wstring Truncate(std::wstring const& s, size_t maxChars);
    uint32_t Hash(std::wstring_view s);

    // ISO-8601 "2026-10-09T09:53:38.964Z" / "+02:00" offsets. Values without a zone are treated as UTC.
    bool ParseIsoTime(std::string_view s, TimePoint& out);
    TimePoint FromLocal(SYSTEMTIME const& local);
    TimePoint FromFileTime(FILETIME const& ft);
    SYSTEMTIME ToLocal(TimePoint t);
    TimePoint StartOfLocalDay(TimePoint t);

    // "now", "45s", "12m", "2h 05m", "3d"
    std::wstring FormatAge(std::chrono::seconds d);
    // Local "14:30"
    std::wstring FormatClock(TimePoint t);

    std::wstring KnownFolder(REFKNOWNFOLDERID id);

    struct ProcessResult
    {
        DWORD exitCode = 0;
        std::string out;
        std::string err;
    };
    // Runs a console program with no window and captures stdout/stderr. Returns false if it could
    // not be started or timed out (the process is then terminated). `cancel` aborts early.
    bool RunProcess(std::wstring commandLine, DWORD timeoutMs, ProcessResult& result, std::atomic<bool> const* cancel = nullptr);

    void OpenUrl(std::wstring const& url);
    // Opens an existing local directory in File Explorer (no-op for anything else).
    void OpenFolder(std::wstring const& path);

    struct Rng
    {
        std::mt19937 gen{ std::random_device{}() };
        Rng() = default;
        explicit Rng(uint32_t seed) : gen(seed) {}
        float Next(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(gen); }
        size_t Index(size_t count) { return std::uniform_int_distribution<size_t>(0, count - 1)(gen); }
    };
}
