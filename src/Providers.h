#pragma once
#include "Provider.h"

namespace ld
{
    class SessionsProvider final : public SnapshotProvider<SessionsSnapshot>
    {
    public:
        explicit SessionsProvider(Config const& config);
        ~SessionsProvider() override;

    protected:
        bool PollReal() override;
        void PollMock() override;
        void OnModeChanged() override;
        std::chrono::milliseconds MockInterval() const override { return std::chrono::seconds(1); }

    private:
        struct Tracked;
        void Rescan();
        std::wstring m_root;
        double m_maxIdleHours;
        int m_maxOrbs;
        std::map<std::wstring, std::unique_ptr<Tracked>> m_tracked;
        ULONGLONG m_lastRescan = 0;
        size_t m_lastCount = SIZE_MAX;
        std::chrono::steady_clock::time_point m_mockStart = std::chrono::steady_clock::now();
    };

    class GitHubProvider final : public SnapshotProvider<PullRequestsSnapshot>
    {
    public:
        explicit GitHubProvider(Config const& config);

    protected:
        bool PollReal() override;
        void PollMock() override;
        std::chrono::milliseconds MockInterval() const override { return std::chrono::seconds(2); }

    private:
        bool QueryAll(PullRequestsSnapshot& snap);
        bool QueryRepo(std::wstring const& repo, PullRequestsSnapshot& snap);
        bool RunGh(std::wstring const& args, std::string& out);
        std::vector<std::wstring> m_repos;
        std::wstring m_gh;
    };

    class SystemProvider final : public SnapshotProvider<SystemSnapshot>
    {
    public:
        explicit SystemProvider(Config const& config);
        ~SystemProvider() override;

    protected:
        bool PollReal() override;
        void PollMock() override;

    private:
        uint64_t m_idle = 0, m_kernel = 0, m_user = 0;
        void* m_query = nullptr;   // PDH_HQUERY
        void* m_counter = nullptr; // PDH_HCOUNTER
        bool m_gpuTried = false;
        int m_gpuTick = 0;
        float m_gpu = 0;
        float m_cpu = 0;
        std::chrono::steady_clock::time_point m_mockStart = std::chrono::steady_clock::now();
    };

    class CalendarProvider final : public SnapshotProvider<CalendarSnapshot>
    {
    public:
        explicit CalendarProvider(Config const& config);

    protected:
        bool PollReal() override;
        void PollMock() override;
        std::chrono::milliseconds MockInterval() const override { return std::chrono::seconds(30); }

    private:
        bool TryAppointments(CalendarSnapshot& snap);
        bool TryIcs(CalendarSnapshot& snap);
        std::wstring m_icsUrl;
        ULONGLONG m_appointmentsRetryAt = 0;
        bool m_loggedFallback = false;
    };

    class PortsProvider final : public SnapshotProvider<PortsSnapshot>
    {
    public:
        explicit PortsProvider(Config const& config);

    protected:
        bool PollReal() override;
        void PollMock() override;

    private:
        std::unordered_map<DWORD, std::wstring> m_names;
    };

    class LogTailProvider final : public SnapshotProvider<LogTailSnapshot>
    {
    public:
        explicit LogTailProvider(Config const& config);

    protected:
        bool PollReal() override;
        void PollMock() override;
        void OnModeChanged() override;

    private:
        std::wstring m_path;
        int m_lines;
        uint64_t m_size = 0;
        uint64_t m_errorSerial = 0;
        std::vector<LogLine> m_mockLines;
        int m_mockTick = 0;
    };

    // Watches HKCU\...\Explorer\VirtualDesktops\CurrentVirtualDesktop with RegNotifyChangeKeyValue.
    class VirtualDesktopWatcher
    {
    public:
        ~VirtualDesktopWatcher();
        std::function<void(VirtualDesktopInfo const&)> OnChanged; // UI thread
        void Start(winrt::Windows::System::DispatcherQueue const& queue);
        void Stop();
        static VirtualDesktopInfo ReadCurrent();

    private:
        void Run();
        winrt::Windows::System::DispatcherQueue m_queue{ nullptr };
        std::thread m_thread;
        HANDLE m_stopEvent = nullptr;
    };
}
