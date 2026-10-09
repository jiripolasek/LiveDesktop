#include "pch.h"
#include "Provider.h"
#include "Log.h"

using namespace std::chrono;

namespace ld
{
    MockControls& Mocks()
    {
        static MockControls controls;
        return controls;
    }

    const wchar_t* AgentStatusName(AgentStatus status)
    {
        switch (status)
        {
        case AgentStatus::Working: return L"working";
        case AgentStatus::Waiting: return L"waiting";
        case AgentStatus::Done: return L"done";
        default: return L"idle";
        }
    }

    bool PullRequestsSnapshot::AnyFailing() const
    {
        return std::any_of(mine.begin(), mine.end(), [](auto const& p) { return p.checks == CheckState::Failure; });
    }

    bool PullRequestsSnapshot::AnyPending() const
    {
        return std::any_of(mine.begin(), mine.end(), [](auto const& p) { return p.checks == CheckState::Pending; });
    }

    std::optional<Meeting> CalendarSnapshot::NextMeeting(TimePoint now) const
    {
        for (auto const& m : meetings)
        {
            if (!m.allDay && m.start >= now) return m;
        }
        return std::nullopt;
    }

    Provider::Provider(std::wstring name, ProviderSettings settings) : m_name(std::move(name)), m_settings(settings) {}

    Provider::~Provider() { Stop(); }

    void Provider::Start(winrt::Windows::System::DispatcherQueue const& queue, bool mock)
    {
        if (!m_settings.enabled || m_thread.joinable()) return;
        m_queue = queue;
        m_mock = mock;
        m_stop = false;
        m_thread = std::thread([this] { Run(); });
    }

    void Provider::Stop()
    {
        {
            std::lock_guard lock(m_lock);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    void Provider::Wake()
    {
        {
            std::lock_guard lock(m_lock);
            m_wake = true;
        }
        m_cv.notify_all();
    }

    void Provider::SetMock(bool mock)
    {
        if (m_mock.exchange(mock) == mock) return;
        {
            std::lock_guard lock(m_lock);
            m_modeChanged = true;
            m_wake = true;
        }
        m_cv.notify_all();
    }

    void Provider::SetThrottle(int multiplier)
    {
        int previous = m_throttle.exchange(std::max(0, multiplier));
        if (previous != multiplier && (previous == 0 || multiplier < previous)) Wake();
    }

    void Provider::Enqueue(std::function<void()> fn)
    {
        if (m_stop || !m_queue) return;
        m_queue.TryEnqueue([fn = std::move(fn)]() { fn(); });
    }

    void Provider::Run()
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        SetThreadDescription(GetCurrentThread(), (L"provider:" + m_name).c_str());
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        int failures = 0;
        while (!m_stop)
        {
            bool modeChanged = false;
            {
                std::lock_guard lock(m_lock);
                modeChanged = std::exchange(m_modeChanged, false);
                m_wake = false;
            }
            if (modeChanged)
            {
                failures = 0;
                OnModeChanged();
            }

            bool mock = m_mock;
            bool ok = true;
            try
            {
                if (mock) PollMock();
                else ok = PollReal();
            }
            catch (winrt::hresult_error const& e)
            {
                Log(L"[%s] error 0x%08X: %s", m_name.c_str(), static_cast<uint32_t>(e.code()), e.message().c_str());
                ok = false;
            }
            catch (std::exception const& e)
            {
                Log(L"[%s] error: %S", m_name.c_str(), e.what());
                ok = false;
            }

            if (ok)
            {
                if (failures > 0) Log(L"[%s] recovered after %d failure(s)", m_name.c_str(), failures);
                failures = 0;
            }
            else
            {
                ++failures;
            }

            milliseconds interval = mock ? MockInterval() : milliseconds(seconds(m_settings.intervalSeconds));
            if (failures > 0)
            {
                auto backoff = interval * (1ll << std::min(failures, 10));
                interval = std::min<milliseconds>(backoff, minutes(30));
                Log(L"[%s] poll failed (%d in a row) - next attempt in %llds", m_name.c_str(), failures,
                    static_cast<long long>(duration_cast<seconds>(interval).count()));
            }

            std::unique_lock lock(m_lock);
            int throttle = m_throttle;
            if (throttle == 0)
            {
                m_cv.wait(lock, [&] { return m_stop || m_wake || m_throttle != 0; });
            }
            else
            {
                m_cv.wait_for(lock, interval * throttle, [&] { return m_stop.load() || m_wake; });
            }
        }
        winrt::uninit_apartment();
    }
}
