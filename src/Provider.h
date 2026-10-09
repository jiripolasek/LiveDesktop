#pragma once
#include "pch.h"
#include "Model.h"
#include "Config.h"

namespace ld
{
    // Demo knobs shared by the mock providers (set from the UI thread via --send / tray).
    struct MockControls
    {
        std::atomic<int> ci{ 0 };       // 0 = mostly green with one pending, 1 = one failing, -1 = all green
        std::atomic<bool> logError{ false };
    };
    MockControls& Mocks();

    // A provider polls one data source on its own thread and publishes immutable snapshots to the
    // UI thread through the DispatcherQueue. Failures are logged once and back off exponentially;
    // nothing here ever blocks the UI thread.
    class Provider
    {
    public:
        Provider(std::wstring name, ProviderSettings settings);
        virtual ~Provider();
        Provider(Provider const&) = delete;
        Provider& operator=(Provider const&) = delete;

        void Start(winrt::Windows::System::DispatcherQueue const& queue, bool mock);
        void Stop();
        void Wake();
        void SetMock(bool mock);
        // 1 = normal cadence, N = N times slower, 0 = suspended until woken / throttle changes.
        void SetThrottle(int multiplier);
        std::wstring const& Name() const { return m_name; }
        bool Enabled() const { return m_settings.enabled; }

    protected:
        // Return false on failure; the provider then backs off and keeps the last snapshot.
        virtual bool PollReal() = 0;
        virtual void PollMock() = 0;
        virtual void OnModeChanged() {}
        virtual std::chrono::milliseconds MockInterval() const { return std::chrono::seconds(m_settings.intervalSeconds); }
        bool IsMock() const { return m_mock.load(); }
        bool Stopping() const { return m_stop.load(); }
        std::atomic<bool> const* StopFlag() const { return &m_stop; }
        void Enqueue(std::function<void()> fn);

    private:
        void Run();

        std::wstring m_name;
        ProviderSettings m_settings;
        winrt::Windows::System::DispatcherQueue m_queue{ nullptr };
        std::thread m_thread;
        std::mutex m_lock;
        std::condition_variable m_cv;
        std::atomic<bool> m_stop{ false };
        std::atomic<bool> m_mock{ false };
        std::atomic<int> m_throttle{ 1 };
        bool m_wake = false;
        bool m_modeChanged = false;
    };

    template <class T>
    class SnapshotProvider : public Provider
    {
    public:
        using Provider::Provider;
        std::function<void(T const&)> OnSnapshot; // set before Start(); invoked on the UI thread

    protected:
        void Publish(T snapshot)
        {
            Enqueue([this, s = std::move(snapshot)]() { if (OnSnapshot) OnSnapshot(s); });
        }
    };
}
