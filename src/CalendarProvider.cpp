#include "pch.h"
#include "Providers.h"
#include "Log.h"
#include "Util.h"
#include <winrt/Windows.ApplicationModel.Appointments.h>
#include <winrt/Windows.Web.Http.h>

using namespace std::chrono;
namespace wam = winrt::Windows::ApplicationModel::Appointments;

// Today's meetings: Windows AppointmentStore (works only when the OS grants calendar access to the
// process - usually not for unpackaged apps), then an ICS URL from config, then mock data.
namespace ld
{
    namespace
    {
        template <class Op>
        bool WaitFor(Op const& op, seconds timeout)
        {
            if (op.wait_for(timeout) != winrt::Windows::Foundation::AsyncStatus::Completed)
            {
                op.Cancel();
                return false;
            }
            return true;
        }

        TimePoint FromDateTime(winrt::Windows::Foundation::DateTime dt)
        {
            return winrt::clock::to_sys(dt);
        }

        // DTSTART;TZID=W. Europe Standard Time:20261009T140000 | DTSTART:20261009T120000Z | DTSTART;VALUE=DATE:20261009
        bool ParseIcsTime(std::wstring const& params, std::wstring const& value, TimePoint& out, bool& allDay)
        {
            allDay = params.find(L"VALUE=DATE") != std::wstring::npos && params.find(L"DATE-TIME") == std::wstring::npos;
            if (value.size() < 8) return false;
            SYSTEMTIME st{};
            st.wYear = static_cast<WORD>(std::stoi(value.substr(0, 4)));
            st.wMonth = static_cast<WORD>(std::stoi(value.substr(4, 2)));
            st.wDay = static_cast<WORD>(std::stoi(value.substr(6, 2)));
            if (value.size() >= 15 && value[8] == L'T')
            {
                st.wHour = static_cast<WORD>(std::stoi(value.substr(9, 2)));
                st.wMinute = static_cast<WORD>(std::stoi(value.substr(11, 2)));
                st.wSecond = static_cast<WORD>(std::stoi(value.substr(13, 2)));
            }
            else allDay = true;
            if (!value.empty() && value.back() == L'Z')
            {
                FILETIME ft{};
                SystemTimeToFileTime(&st, &ft);
                out = FromFileTime(ft);
            }
            else
            {
                out = FromLocal(st); // TZID or floating time: assume the local zone (POC simplification)
            }
            return true;
        }

        std::wstring IcsUnescape(std::wstring v)
        {
            std::wstring r;
            for (size_t i = 0; i < v.size(); ++i)
            {
                if (v[i] == L'\\' && i + 1 < v.size())
                {
                    wchar_t n = v[++i];
                    r += (n == L'n' || n == L'N') ? L' ' : n;
                }
                else r += v[i];
            }
            return r;
        }
    }

    CalendarProvider::CalendarProvider(Config const& config) : SnapshotProvider(L"calendar", config.calendar), m_icsUrl(config.icsUrl) {}

    bool CalendarProvider::TryAppointments(CalendarSnapshot& snap)
    {
        if (GetTickCount64() < m_appointmentsRetryAt) return false;
        try
        {
            auto storeOp = wam::AppointmentManager::RequestStoreAsync(wam::AppointmentStoreAccessType::AllCalendarsReadOnly);
            if (!WaitFor(storeOp, seconds(10))) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            auto store = storeOp.GetResults();
            if (!store) throw winrt::hresult_error(E_ACCESSDENIED);
            auto now = Clock::now();
            auto dayStart = StartOfLocalDay(now);
            wam::FindAppointmentsOptions options;
            options.MaxCount(30);
            for (auto name : { wam::AppointmentProperties::Subject(), wam::AppointmentProperties::Location(),
                               wam::AppointmentProperties::StartTime(), wam::AppointmentProperties::Duration(), wam::AppointmentProperties::AllDay() })
                options.FetchProperties().Append(name);
            auto findOp = store.FindAppointmentsAsync(winrt::clock::from_sys(dayStart), hours(24), options);
            if (!WaitFor(findOp, seconds(10))) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            for (auto const& a : findOp.GetResults())
            {
                Meeting m;
                m.title = std::wstring(a.Subject());
                m.location = std::wstring(a.Location());
                m.start = FromDateTime(a.StartTime());
                m.end = m.start + duration_cast<Clock::duration>(a.Duration());
                m.allDay = a.AllDay();
                snap.meetings.push_back(std::move(m));
            }
            snap.source = L"Windows Calendar";
            return true;
        }
        catch (winrt::hresult_error const& e)
        {
            m_appointmentsRetryAt = GetTickCount64() + 60 * 60 * 1000; // don't hammer a denied API
            if (!m_loggedFallback)
                Log(L"[calendar] AppointmentStore unavailable (0x%08X: %s)", static_cast<uint32_t>(e.code()), e.message().c_str());
            return false;
        }
    }

    bool CalendarProvider::TryIcs(CalendarSnapshot& snap)
    {
        if (m_icsUrl.empty() || m_icsUrl.rfind(L"https://", 0) != 0) return false;
        try
        {
            winrt::Windows::Web::Http::HttpClient client;
            auto op = client.GetStringAsync(winrt::Windows::Foundation::Uri(m_icsUrl));
            if (!WaitFor(op, seconds(20))) return false;
            std::wstring text(op.GetResults());

            // Unfold continuation lines, then walk VEVENT blocks.
            std::vector<std::wstring> lines;
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t end = text.find(L'\n', pos);
                if (end == std::wstring::npos) end = text.size();
                std::wstring line = text.substr(pos, end - pos);
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                if (!line.empty() && (line[0] == L' ' || line[0] == L'\t') && !lines.empty()) lines.back() += line.substr(1);
                else lines.push_back(std::move(line));
                pos = end + 1;
            }
            auto now = Clock::now();
            auto dayStart = StartOfLocalDay(now);
            auto dayEnd = dayStart + hours(24);
            Meeting current;
            bool inEvent = false, hasStart = false;
            for (auto const& line : lines)
            {
                if (line == L"BEGIN:VEVENT") { inEvent = true; hasStart = false; current = {}; continue; }
                if (line == L"END:VEVENT")
                {
                    inEvent = false;
                    if (hasStart && current.start < dayEnd && (current.end > dayStart || current.start >= dayStart)) snap.meetings.push_back(current);
                    continue;
                }
                if (!inEvent) continue;
                auto colon = line.find(L':');
                if (colon == std::wstring::npos) continue;
                auto head = line.substr(0, colon);
                auto value = line.substr(colon + 1);
                auto semi = head.find(L';');
                auto name = head.substr(0, semi);
                auto params = semi == std::wstring::npos ? L"" : head.substr(semi);
                bool allDay = false;
                if (name == L"SUMMARY") current.title = IcsUnescape(value);
                else if (name == L"LOCATION") current.location = IcsUnescape(value);
                else if (name == L"DTSTART" && ParseIcsTime(params, value, current.start, allDay)) { hasStart = true; current.allDay = allDay; if (current.end == TimePoint{}) current.end = current.start + minutes(30); }
                else if (name == L"DTEND") ParseIcsTime(params, value, current.end, allDay);
            }
            snap.source = L"ICS";
            return true;
        }
        catch (winrt::hresult_error const& e)
        {
            Log(L"[calendar] ICS fetch failed (0x%08X: %s)", static_cast<uint32_t>(e.code()), e.message().c_str());
            return false;
        }
    }

    bool CalendarProvider::PollReal()
    {
        CalendarSnapshot snap;
        if (TryAppointments(snap) || TryIcs(snap))
        {
            std::sort(snap.meetings.begin(), snap.meetings.end(), [](auto const& a, auto const& b) { return a.start < b.start; });
            Log(L"[calendar] %zu meeting(s) today from %s", snap.meetings.size(), snap.source.c_str());
            Publish(std::move(snap));
            return true;
        }
        if (!m_loggedFallback)
        {
            m_loggedFallback = true;
            Log(L"[calendar] no calendar source available - showing mock meetings (set providers.calendar.icsUrl for real data)");
        }
        PollMock();
        return true;
    }

    void CalendarProvider::PollMock()
    {
        // Meetings anchored to when the mock started so "next meeting in X min" counts down.
        static const auto anchor = time_point_cast<minutes>(Clock::now());
        auto rounded = anchor;
        CalendarSnapshot snap;
        snap.mock = true;
        snap.source = L"mock";
        auto add = [&](const wchar_t* title, const wchar_t* location, int startMin, int lengthMin)
        {
            Meeting m;
            m.title = title;
            m.location = location;
            m.start = rounded + minutes(startMin);
            m.end = m.start + minutes(lengthMin);
            snap.meetings.push_back(std::move(m));
        };
        add(L"Standup", L"Teams", -95, 15);
        add(L"Design sync: Ambient desktop", L"Teams", 12, 30);
        add(L"1:1 with Adele", L"Room 4.12", 75, 30);
        add(L"PR review hour", L"Focus", 150, 60);
        Publish(std::move(snap));
    }
}
