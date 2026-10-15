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
        static void LinkChildren(std::vector<AgentSession>& sessions, std::map<std::wstring, TimePoint> const& created);
        std::wstring m_root;
        double m_maxIdleHours;
        int m_maxOrbs;
        std::map<std::wstring, std::unique_ptr<Tracked>> m_tracked;
        ULONGLONG m_lastRescan = 0;
        size_t m_lastCount = SIZE_MAX;
        std::chrono::steady_clock::time_point m_mockStart = std::chrono::steady_clock::now();
    };
}
