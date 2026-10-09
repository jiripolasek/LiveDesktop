#include "pch.h"
#include "Dashboard.h"
#include "Log.h"

using namespace std::chrono_literals;

namespace ld
{
    namespace
    {
        constexpr size_t kMaxAgents = 6;
        constexpr size_t kMaxMine = 5;
        constexpr size_t kMaxReviews = 4;
        constexpr size_t kMaxMeetings = 6;
        constexpr size_t kMaxPorts = 8;
        constexpr size_t kMaxLogLines = 10;

        wuc::CompositionEasingFunction Decelerate(wuc::Compositor const& c)
        {
            return c.CreateCubicBezierEasingFunction(float2(0.1f, 0.9f), float2(0.2f, 1.0f));
        }

        wuc::CompositionEasingFunction Accelerate(wuc::Compositor const& c)
        {
            return c.CreateCubicBezierEasingFunction(float2(0.7f, 0.0f), float2(1.0f, 0.5f));
        }

        void AnimateScalar(wuc::Compositor const& c, wuc::CompositionObject const& target, wchar_t const* property,
            std::optional<float> from, float to, float seconds, float delay, wuc::CompositionEasingFunction const& ease)
        {
            auto a = c.CreateScalarKeyFrameAnimation();
            if (from) a.InsertKeyFrame(0.0f, *from);
            else a.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
            a.InsertKeyFrame(1.0f, to, ease);
            a.Duration(Seconds(seconds));
            a.DelayTime(Seconds(delay));
            a.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            target.StartAnimation(property, a);
        }

        void AnimateVector3(wuc::Compositor const& c, wuc::CompositionObject const& target, wchar_t const* property,
            std::optional<float3> from, float3 to, float seconds, float delay, wuc::CompositionEasingFunction const& ease)
        {
            auto a = c.CreateVector3KeyFrameAnimation();
            if (from) a.InsertKeyFrame(0.0f, *from);
            else a.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
            a.InsertKeyFrame(1.0f, to, ease);
            a.Duration(Seconds(seconds));
            a.DelayTime(Seconds(delay));
            a.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            target.StartAnimation(property, a);
        }

        std::wstring Age(TimePoint from)
        {
            auto d = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - from);
            return FormatAge(std::max(d, std::chrono::seconds(0)));
        }

        std::wstring StatusText(AgentSession const& s)
        {
            switch (s.status)
            {
            case AgentStatus::Waiting: return L"Needs your input \u00B7 " + Age(s.since);
            case AgentStatus::Working: return L"Working \u00B7 " + Age(s.since);
            case AgentStatus::Done: return L"Finished " + Age(s.since) + L" ago";
            default: return L"Idle \u00B7 " + Age(s.lastActivity);
            }
        }

        std::wstring FileName(std::wstring const& path)
        {
            auto pos = path.find_last_of(L"\\/");
            return pos == std::wstring::npos ? path : path.substr(pos + 1);
        }

        bool Intersects(RECT const& a, RECT const& b)
        {
            return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
        }
    }

    std::wstring Dashboard::Content::Key() const
    {
        std::wstring k = icon + L"\x1F" + title + L"\x1F" + meta + L"\x1F" + footer + L"\x1F" + std::to_wstring(metaColor);
        for (auto const& r : rows)
        {
            k += L"\x1E" + std::to_wstring(static_cast<int>(r.style)) + L"\x1F" + r.glyph + L"\x1F" + std::to_wstring(r.glyphColor) +
                L"\x1F" + (r.dot ? std::to_wstring(*r.dot) : L"-") + L"\x1F" + r.lead + L"\x1F" + r.primary + L"\x1F" + r.secondary +
                L"\x1F" + r.trailing + L"\x1F" + std::to_wstring(r.trailingColor) + L"\x1F" + r.url +
                (r.dim ? L"d" : L"") + (r.error ? L"e" : L"") + (r.accent ? L"a" : L"");
        }
        return k;
    }

    // ---------------------------------------------------------------- lifetime

    void Dashboard::Initialize(Scene& scene, Graphics& graphics, bool logTailEnabled)
    {
        m_scene = &scene;
        m_graphics = &graphics;
        m_c = scene.Compositor();
        m_logTail = logTailEnabled;
        m_root = m_c.CreateContainerVisual();
        m_root.IsVisible(false);
        scene.Overlay().Children().InsertAtTop(m_root);

        m_hoverGeometry = m_c.CreateRoundedRectangleGeometry();
        auto shape = m_c.CreateSpriteShape(m_hoverGeometry);
        shape.FillBrush(m_c.CreateColorBrush(Argb(18, 0xFFFFFF)));
        shape.StrokeBrush(m_c.CreateColorBrush(Argb(40, 0xFFFFFF)));
        shape.StrokeThickness(1.0f);
        m_hover = m_c.CreateShapeVisual();
        m_hover.Shapes().Append(shape);
        m_hover.Opacity(0.0f);
        m_root.Children().InsertAtTop(m_hover);
    }

    void Dashboard::Shutdown()
    {
        m_cards.clear();
        *this = Dashboard{};
    }

    void Dashboard::SetLayout(SceneLayout const& layout)
    {
        m_layout = layout;
        Hover(std::nullopt);
        if (m_visible) Relayout(false);
    }

    void Dashboard::SetPreset(ScenePreset const& preset)
    {
        m_preset = preset;
        ApplyVisibility();
        if (m_visible) Relayout(true);
    }

    void Dashboard::Show(bool visible)
    {
        m_requested = visible;
        ApplyVisibility();
    }

    void Dashboard::ApplyVisibility()
    {
        if (!m_root) return;
        bool want = m_requested && m_preset.dashboard;
        if (want == m_visible) return;
        m_visible = want;
        uint32_t serial = ++m_showSerial;
        if (want)
        {
            m_root.IsVisible(true);
            Relayout(false);
            auto ease = Decelerate(m_c);
            float lift = D(24);
            for (size_t i = 0; i < m_cards.size(); ++i)
            {
                auto& card = m_cards[i];
                float delay = 0.045f * static_cast<float>(i);
                float3 to(card.pos.x, card.pos.y, 0);
                AnimateScalar(m_c, card.visual, L"Opacity", 0.0f, 1.0f, 0.4f, delay, ease);
                AnimateVector3(m_c, card.visual, L"Offset", float3(to.x, to.y + lift, 0), to, 0.4f, delay, ease);
                AnimateVector3(m_c, card.visual, L"Scale", float3(0.96f, 0.96f, 1), float3(1, 1, 1), 0.4f, delay, ease);
            }
            Log(L"Dashboard: shown (%zu cards)", m_cards.size());
        }
        else
        {
            Hover(std::nullopt);
            auto ease = Accelerate(m_c);
            auto batch = m_c.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
            for (auto& card : m_cards)
            {
                AnimateScalar(m_c, card.visual, L"Opacity", std::nullopt, 0.0f, 0.22f, 0.0f, ease);
                AnimateVector3(m_c, card.visual, L"Offset", std::nullopt, float3(card.pos.x, card.pos.y + D(12), 0), 0.22f, 0.0f, ease);
            }
            batch.End();
            auto root = m_root;
            batch.Completed([this, root, serial](auto&&, auto&&)
            {
                if (m_root == root && m_showSerial == serial && !m_visible) root.IsVisible(false);
            });
            Log(L"Dashboard: hidden");
        }
    }

    // ---------------------------------------------------------------- data

    void Dashboard::Update(SessionsSnapshot const& snapshot) { m_sessions = snapshot; m_haveSessions = true; Changed(Kind::Agents); }
    void Dashboard::Update(PullRequestsSnapshot const& snapshot) { m_prs = snapshot; m_havePrs = true; m_prsAt = Clock::now(); Changed(Kind::PullRequests); }
    void Dashboard::Update(CalendarSnapshot const& snapshot) { m_calendar = snapshot; m_haveCalendar = true; Changed(Kind::Today); }
    void Dashboard::Update(PortsSnapshot const& snapshot) { m_ports = snapshot; m_havePorts = true; Changed(Kind::Ports); }
    void Dashboard::Update(WorktreesSnapshot const& snapshot) { m_trees = snapshot; m_haveTrees = true; Changed(Kind::Worktrees); }

    void Dashboard::Update(LogTailSnapshot const& snapshot)
    {
        bool newError = m_haveLog && snapshot.errorSerial != m_logErrorSerial;
        m_log = snapshot;
        m_haveLog = true;
        m_logErrorSerial = snapshot.errorSerial;
        Changed(Kind::Log);
        if (newError && m_visible)
        {
            if (auto* card = Find(Kind::Log)) FlashCard(*card, palette::Failure);
        }
    }

    void Dashboard::Tick()
    {
        if (!m_visible) return;
        Changed(Kind::Agents);
        Changed(Kind::PullRequests);
        Changed(Kind::Today);
    }

    void Dashboard::RedrawAll()
    {
        for (auto& card : m_cards)
        {
            card.key.clear();
            if (m_visible) Draw(card, true);
        }
    }

    void Dashboard::Changed(Kind kind)
    {
        if (!m_visible) return; // drawn lazily on the next reveal
        auto kinds = ActiveKinds();
        if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) return;
        Card* card = Find(kind);
        if (!card || Draw(*card, false)) Relayout(true);
    }

    Dashboard::Card* Dashboard::Find(Kind kind)
    {
        for (auto& card : m_cards) if (card.kind == kind) return &card;
        return nullptr;
    }

    std::vector<Dashboard::Kind> Dashboard::ActiveKinds() const
    {
        std::vector<Kind> kinds{ Kind::Agents, Kind::PullRequests, Kind::Today, Kind::Ports };
        if (m_haveTrees && (m_preset.devCards || !m_trees.trees.empty())) kinds.push_back(Kind::Worktrees);
        if (m_preset.devCards && m_logTail) kinds.push_back(Kind::Log);
        return kinds;
    }

    // ---------------------------------------------------------------- content

    Dashboard::Content Dashboard::Build(Kind kind) const
    {
        Content c;
        auto now = Clock::now();
        switch (kind)
        {
        case Kind::Agents:
        {
            c.icon = L"\uE99A";
            c.title = L"Agents";
            c.accent = palette::Working;
            int waiting = 0, working = 0;
            for (auto const& s : m_sessions.sessions)
            {
                waiting += s.status == AgentStatus::Waiting;
                working += s.status == AgentStatus::Working;
            }
            if (waiting) { c.meta = std::to_wstring(waiting) + L" waiting on you"; c.metaColor = palette::Waiting; }
            else if (working) { c.meta = std::to_wstring(working) + L" working"; c.metaColor = palette::Working; }
            else if (!m_sessions.sessions.empty()) c.meta = L"all quiet";
            for (size_t i = 0; i < m_sessions.sessions.size() && i < kMaxAgents; ++i)
            {
                auto const& s = m_sessions.sessions[i];
                Row r;
                r.dot = s.status == AgentStatus::Done ? palette::Done : StatusRgb(s.status);
                r.primary = s.title.empty() ? (s.repo.empty() ? s.id : s.repo) : s.title;
                r.secondary = StatusText(s);
                if (!s.repo.empty()) r.secondary += L" \u00B7 " + s.repo;
                r.url = L"ghapp://sessions/" + s.id;
                r.dim = s.status == AgentStatus::Idle;
                r.accent = s.status == AgentStatus::Waiting;
                c.rows.push_back(std::move(r));
            }
            if (c.rows.empty())
            {
                c.rows.push_back(Row{ RowStyle::Empty });
                c.rows.back().primary = m_haveSessions ? L"No recent Copilot sessions" : L"Looking for sessions\u2026";
            }
            size_t more = m_sessions.sessions.size() > kMaxAgents ? m_sessions.sessions.size() - kMaxAgents : 0;
            c.footer = more ? L"+" + std::to_wstring(more) + L" more \u00B7 click to open in Copilot" : L"Click a session to open it in Copilot";
            if (m_sessions.mock) c.footer += L" \u00B7 mock";
            break;
        }
        case Kind::PullRequests:
        {
            c.icon = L"\uE943";
            c.title = L"Pull requests";
            c.accent = palette::Accent;
            int failing = 0, pending = 0;
            for (auto const& pr : m_prs.mine)
            {
                failing += pr.checks == CheckState::Failure;
                pending += pr.checks == CheckState::Pending;
            }
            if (failing) { c.meta = std::to_wstring(failing) + L" failing"; c.metaColor = palette::Failure; }
            else if (pending) { c.meta = L"checks running"; c.metaColor = palette::Pending; }
            else if (!m_prs.mine.empty()) { c.meta = L"all green"; c.metaColor = palette::Success; }

            c.rows.push_back(Row{ RowStyle::Section });
            c.rows.back().primary = L"Yours";
            for (size_t i = 0; i < m_prs.mine.size() && i < kMaxMine; ++i)
            {
                auto const& pr = m_prs.mine[i];
                Row r;
                switch (pr.checks)
                {
                case CheckState::Success: r.glyph = L"\uE73E"; r.glyphColor = palette::Success; break;
                case CheckState::Failure: r.glyph = L"\uE711"; r.glyphColor = palette::Failure; r.accent = true; break;
                case CheckState::Pending: r.glyph = L"\uE823"; r.glyphColor = palette::Pending; break;
                default: r.dot = palette::Idle; break;
                }
                r.primary = pr.title;
                r.secondary = pr.repo + L" #" + std::to_wstring(pr.number);
                if (pr.draft) r.secondary += L" \u00B7 draft";
                if (pr.updated != TimePoint{}) r.secondary += L" \u00B7 " + Age(pr.updated);
                r.url = pr.url;
                c.rows.push_back(std::move(r));
            }
            if (m_prs.mine.empty())
            {
                c.rows.push_back(Row{ RowStyle::Empty });
                c.rows.back().primary = m_havePrs ? L"No open pull requests" : L"Asking gh\u2026";
            }
            if (!m_prs.reviews.empty())
            {
                c.rows.push_back(Row{ RowStyle::Section });
                c.rows.back().primary = L"Review requested";
                for (size_t i = 0; i < m_prs.reviews.size() && i < kMaxReviews; ++i)
                {
                    auto const& pr = m_prs.reviews[i];
                    Row r;
                    r.glyph = L"\uE890";
                    r.glyphColor = palette::Firefly;
                    r.primary = pr.title;
                    r.secondary = pr.repo + L" #" + std::to_wstring(pr.number);
                    if (!pr.author.empty()) r.secondary += L" \u00B7 @" + pr.author;
                    r.url = pr.url;
                    c.rows.push_back(std::move(r));
                }
            }
            c.footer = !m_havePrs ? L"via gh" : (m_prsAt + 60s > now ? L"via gh \u00B7 just updated" : L"via gh \u00B7 updated " + Age(m_prsAt) + L" ago");
            if (m_prs.mock) c.footer += L" \u00B7 mock";
            break;
        }
        case Kind::Today:
        {
            c.icon = L"\uE787";
            c.title = L"Today";
            c.accent = palette::Meeting;
            auto const& meetings = m_calendar.meetings;
            size_t first = 0;
            while (first < meetings.size() && meetings[first].end <= now) ++first;
            if (first > 0) --first; // keep the most recent past meeting for context
            bool nextMarked = false;
            for (size_t i = first; i < meetings.size() && i < first + kMaxMeetings; ++i)
            {
                auto const& m = meetings[i];
                Row r;
                r.lead = m.allDay ? L"All day" : FormatClock(m.start);
                r.primary = m.title;
                r.secondary = m.location.empty() ? (m.allDay ? L"" : FormatClock(m.start) + L" \u2013 " + FormatClock(m.end)) : m.location;
                bool past = !m.allDay && m.end <= now;
                bool current = !m.allDay && m.start <= now && now < m.end;
                r.dim = past;
                if (current)
                {
                    r.trailing = L"now";
                    r.trailingColor = palette::MeetingNow;
                    r.trailingAlpha = 1.0f;
                    r.accent = true;
                    if (c.meta.empty()) { c.meta = L"In a meeting"; c.metaColor = palette::MeetingNow; }
                }
                else if (!past && !m.allDay && !nextMarked)
                {
                    nextMarked = true;
                    auto mins = std::chrono::duration_cast<std::chrono::minutes>(m.start - now).count();
                    r.trailing = L"in " + FormatAge(std::chrono::duration_cast<std::chrono::seconds>(m.start - now));
                    r.trailingColor = mins <= 10 ? palette::MeetingSoon : palette::Meeting;
                    r.trailingAlpha = 1.0f;
                    r.accent = mins <= 10;
                    if (c.meta.empty()) { c.meta = L"Next " + r.trailing; c.metaColor = r.trailingColor; }
                }
                c.rows.push_back(std::move(r));
            }
            if (c.rows.empty())
            {
                c.rows.push_back(Row{ RowStyle::Empty });
                c.rows.back().primary = m_haveCalendar ? L"Nothing else on the calendar today" : L"Reading calendar\u2026";
            }
            c.footer = m_calendar.source.empty() ? L"Calendar" : m_calendar.source;
            if (m_calendar.mock && c.footer != L"mock") c.footer += L" \u00B7 mock";
            break;
        }
        case Kind::Ports:
        {
            c.icon = L"\uE968";
            c.title = L"Dev ports";
            c.accent = palette::Success;
            if (!m_ports.ports.empty()) c.meta = std::to_wstring(m_ports.ports.size()) + L" listening";
            for (size_t i = 0; i < m_ports.ports.size() && i < kMaxPorts; ++i)
            {
                auto const& p = m_ports.ports[i];
                Row r{ RowStyle::One };
                r.dot = palette::Success;
                r.lead = L":" + std::to_wstring(p.port);
                r.primary = p.process.empty() ? L"pid " + std::to_wstring(p.pid) : p.process;
                r.trailing = p.address;
                r.url = L"http://localhost:" + std::to_wstring(p.port);
                c.rows.push_back(std::move(r));
            }
            if (c.rows.empty())
            {
                c.rows.push_back(Row{ RowStyle::Empty });
                c.rows.back().primary = m_havePorts ? L"No dev servers listening" : L"Scanning ports\u2026";
            }
            c.footer = L"Click to open http://localhost";
            if (m_ports.mock) c.footer += L" \u00B7 mock";
            break;
        }
        case Kind::Worktrees:
        {
            c.icon = L"\uE8B7";
            c.title = L"Working trees";
            c.accent = palette::Pending;
            int dirty = 0, unpushed = 0, conflicted = 0;
            for (auto const& t : m_trees.trees)
            {
                dirty += t.Dirty() > 0;
                unpushed += t.ahead > 0 || (t.upstream.empty() && t.branch != L"(detached)");
                conflicted += t.conflicts > 0;
            }
            if (conflicted) { c.meta = std::to_wstring(conflicted) + L" with conflicts"; c.metaColor = palette::Failure; }
            else if (dirty) { c.meta = std::to_wstring(dirty) + L" with changes"; c.metaColor = palette::Pending; }
            else if (unpushed) { c.meta = std::to_wstring(unpushed) + L" unpushed"; c.metaColor = palette::Accent; }
            else if (!m_trees.trees.empty()) { c.meta = L"all clean"; c.metaColor = palette::Success; }
            for (auto const& t : m_trees.trees)
            {
                Row r;
                r.glyph = L"\uE8B7";
                if (t.conflicts) r.glyphColor = palette::Failure;
                else if (t.Dirty()) r.glyphColor = palette::Pending;
                else if (t.ahead || t.upstream.empty()) r.glyphColor = palette::Accent;
                else r.glyphColor = palette::Success;
                r.primary = t.name + L"  \u00B7  " + t.branch;

                std::vector<std::wstring> parts;
                if (t.conflicts) parts.push_back(std::to_wstring(t.conflicts) + L" conflicted");
                if (t.staged) parts.push_back(std::to_wstring(t.staged) + L" staged");
                if (t.modified) parts.push_back(std::to_wstring(t.modified) + L" modified");
                if (t.untracked) parts.push_back(std::to_wstring(t.untracked) + L" new");
                if (parts.empty()) parts.push_back(L"clean");
                for (size_t i = 0; i < parts.size(); ++i) r.secondary += (i ? L", " : L"") + parts[i];
                if (t.lastActivity != TimePoint{}) r.secondary += L" \u00B7 " + Age(t.lastActivity);
                if (t.fromSession) r.secondary += L" \u00B7 agent";

                if (t.ahead || t.behind)
                {
                    if (t.ahead) r.trailing = L"\u2191" + std::to_wstring(t.ahead);
                    if (t.behind) r.trailing += (r.trailing.empty() ? L"" : L" ") + std::wstring(L"\u2193") + std::to_wstring(t.behind);
                    r.trailingColor = t.behind ? palette::Pending : palette::Accent;
                    r.trailingAlpha = 1.0f;
                }
                else if (t.upstream.empty() && t.branch != L"(detached)") r.trailing = L"not pushed";
                else r.trailing = L"synced";
                r.url = L"folder:" + t.path;
                r.accent = t.conflicts > 0;
                c.rows.push_back(std::move(r));
            }
            if (c.rows.empty())
            {
                c.rows.push_back(Row{ RowStyle::Empty });
                c.rows.back().primary = L"No recent repos \u2014 add paths under providers.worktrees";
            }
            c.footer = L"Click to open the folder \u00B7 git status";
            if (m_trees.mock) c.footer += L" \u00B7 mock";
            break;
        }
        case Kind::Log:
        {
            c.icon = L"\uE7C3";
            c.title = L"Dev log";
            c.accent = palette::Idle;
            c.meta = FileName(m_log.path);
            size_t start = m_log.lines.size() > kMaxLogLines ? m_log.lines.size() - kMaxLogLines : 0;
            for (size_t i = start; i < m_log.lines.size(); ++i)
            {
                Row r{ RowStyle::Mono };
                r.primary = m_log.lines[i].text;
                r.error = m_log.lines[i].error;
                c.rows.push_back(std::move(r));
            }
            if (start < m_log.lines.size() && m_log.lines.back().error) c.metaColor = palette::Failure;
            if (c.rows.empty())
            {
                c.rows.push_back(Row{ RowStyle::Empty });
                c.rows.back().primary = L"Waiting for log output\u2026";
            }
            c.footer = m_log.path;
            if (m_log.mock) c.footer += L" \u00B7 mock";
            break;
        }
        }
        return c;
    }

    // ---------------------------------------------------------------- drawing

    float Dashboard::RowHeight(RowStyle style) const
    {
        switch (style)
        {
        case RowStyle::Section: return D(30);
        case RowStyle::Two: return D(46);
        case RowStyle::One: return D(32);
        case RowStyle::Mono: return D(18);
        default: return D(40);
        }
    }

    bool Dashboard::Draw(Card& card, bool force)
    {
        if (!m_graphics || m_cardW <= 0) return false;
        Content content = Build(card.kind);
        std::wstring key = content.Key() + L"|" + std::to_wstring(m_cardW) + L"|" + std::to_wstring(m_layout.scale);
        if (!force && card.surface && key == card.key) return false;
        card.key = key;

        auto& g = *m_graphics;
        float W = m_cardW, P = D(16), header = D(50), footerH = content.footer.empty() ? 0.0f : D(30);
        float h = header + D(6);
        for (auto const& r : content.rows) h += RowHeight(r.style);
        h = std::ceil(h + D(6) + footerH);
        float2 oldSize = card.size;
        card.size = float2(W, h);
        if (!card.surface)
        {
            card.surface = g.CreateSurface(card.size);
            card.visual.Brush(m_c.CreateSurfaceBrush(card.surface));
        }

        card.items.clear();
        {
            float y = header + D(6);
            for (auto const& r : content.rows)
            {
                float rh = RowHeight(r.style);
                if (!r.url.empty()) card.items.push_back(Item{ D2D1::RectF(D(6), y, W - D(6), y + rh), r.url });
                y += rh;
            }
        }

        float leadW = 0;
        for (auto const& r : content.rows)
        {
            if (r.lead.empty()) continue;
            auto l = g.Layout(r.lead, g.Font(FontRole::UiSemibold, 13), W);
            leadW = std::max(leadW, g.Measure(l.get()).x);
        }
        if (leadW > 0) leadW = std::ceil(leadW + D(10));

        g.Draw(card.surface, card.size, [&](ID2D1DeviceContext* dc)
        {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(D2DColor(0x1A2030, 0.70f), brush.put());
            auto shape = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, h - 0.5f), D(12), D(12));
            dc->FillRoundedRectangle(shape, brush.get());
            if (auto noise = g.NoiseBrush(dc)) dc->FillRoundedRectangle(shape, noise);

            D2D1_GRADIENT_STOP stops[2] = { { 0.0f, D2DColor(0xFFFFFF, 0.075f) }, { 1.0f, D2DColor(0xFFFFFF, 0.0f) } };
            winrt::com_ptr<ID2D1GradientStopCollection> collection;
            winrt::com_ptr<ID2D1LinearGradientBrush> sheen;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, collection.put())) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, D(80))), collection.get(), sheen.put())))
            {
                dc->FillRoundedRectangle(shape, sheen.get());
            }
            brush->SetColor(D2DColor(0xFFFFFF, 0.12f));
            dc->DrawRoundedRectangle(shape, brush.get(), 1.0f);

            // Header: icon, title, meta.
            auto icon = g.Layout(content.icon, g.Font(FontRole::Icons, 16), D(24), 10000, false);
            float2 is = g.Measure(icon.get());
            brush->SetColor(D2DColor(content.accent, 1.0f));
            dc->DrawTextLayout(D2D1::Point2F(P, std::round((header - is.y) * 0.5f)), icon.get(), brush.get());
            auto title = g.Layout(content.title, g.Font(FontRole::UiSemibold, 14.5f), W * 0.6f);
            float2 ts = g.Measure(title.get());
            brush->SetColor(D2DColor(0xFFFFFF, 0.96f));
            float titleX = P + D(28);
            dc->DrawTextLayout(D2D1::Point2F(titleX, std::round((header - ts.y) * 0.5f)), title.get(), brush.get());
            if (!content.meta.empty())
            {
                float metaX = titleX + ts.x + D(12);
                auto meta = g.Layout(content.meta, g.Font(FontRole::Ui, 12, DWRITE_TEXT_ALIGNMENT_TRAILING), W - P - metaX);
                float2 ms = g.Measure(meta.get());
                brush->SetColor(D2DColor(content.metaColor, content.metaColor == 0xFFFFFF ? 0.55f : 0.95f));
                dc->DrawTextLayout(D2D1::Point2F(metaX, std::round((header - ms.y) * 0.5f)), meta.get(), brush.get());
            }
            brush->SetColor(D2DColor(0xFFFFFF, 0.07f));
            dc->DrawLine(D2D1::Point2F(P, header - 0.5f), D2D1::Point2F(W - P, header - 0.5f), brush.get(), 1.0f);

            float y = header + D(6);
            for (auto const& r : content.rows)
            {
                float rh = RowHeight(r.style);
                float primaryAlpha = r.dim ? 0.42f : 0.93f;
                float secondaryAlpha = r.dim ? 0.32f : 0.58f;
                if (r.accent)
                {
                    uint32_t tint = r.glyph.empty() && r.dot ? *r.dot : r.glyphColor;
                    if (!r.trailing.empty() && r.trailingAlpha >= 1.0f) tint = r.trailingColor;
                    brush->SetColor(D2DColor(tint, 0.10f));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(D(6), y + D(2), W - D(6), y + rh - D(2)), D(8), D(8)), brush.get());
                }
                switch (r.style)
                {
                case RowStyle::Section:
                {
                    auto l = g.Layout(r.primary, g.Font(FontRole::UiSemibold, 11), W - P * 2);
                    brush->SetColor(D2DColor(0xFFFFFF, 0.45f));
                    dc->DrawTextLayout(D2D1::Point2F(P, y + D(11)), l.get(), brush.get());
                    break;
                }
                case RowStyle::Empty:
                {
                    auto l = g.Layout(r.primary, g.Font(FontRole::Ui, 12.5f), W - P * 2);
                    float2 s = g.Measure(l.get());
                    brush->SetColor(D2DColor(0xFFFFFF, 0.48f));
                    dc->DrawTextLayout(D2D1::Point2F(P, y + std::round((rh - s.y) * 0.5f)), l.get(), brush.get());
                    break;
                }
                case RowStyle::Mono:
                {
                    auto l = g.Layout(r.primary, g.Font(FontRole::Mono, 11), W - P * 2);
                    brush->SetColor(r.error ? D2DColor(palette::Failure, 0.95f) : D2DColor(0xFFFFFF, 0.55f));
                    dc->DrawTextLayout(D2D1::Point2F(P, y + D(1)), l.get(), brush.get());
                    break;
                }
                default:
                {
                    bool two = r.style == RowStyle::Two;
                    auto primaryLayout = g.Layout(r.primary, g.Font(FontRole::Ui, 13), W);
                    float lineH = g.Measure(primaryLayout.get()).y;
                    float top = two ? y + D(5) : y + std::round((rh - lineH) * 0.5f);
                    float markerCenter = top + lineH * 0.5f;
                    if (r.dot)
                    {
                        brush->SetColor(D2DColor(*r.dot, r.dim ? 0.5f : 1.0f));
                        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(P + D(5), markerCenter), D(4), D(4)), brush.get());
                    }
                    else if (!r.glyph.empty())
                    {
                        auto gl = g.Layout(r.glyph, g.Font(FontRole::Icons, 12), D(20), 10000, false);
                        float2 gs = g.Measure(gl.get());
                        brush->SetColor(D2DColor(r.glyphColor, 1.0f));
                        dc->DrawTextLayout(D2D1::Point2F(P, std::round(markerCenter - gs.y * 0.5f)), gl.get(), brush.get());
                    }
                    float x = P + D(22);
                    if (!r.lead.empty())
                    {
                        auto ll = g.Layout(r.lead, g.Font(FontRole::UiSemibold, 13), leadW);
                        brush->SetColor(D2DColor(0xFFFFFF, primaryAlpha));
                        dc->DrawTextLayout(D2D1::Point2F(x, top), ll.get(), brush.get());
                        x += leadW;
                    }
                    float right = W - P;
                    if (!r.trailing.empty())
                    {
                        auto tl = g.Layout(r.trailing, g.Font(FontRole::Ui, 12, DWRITE_TEXT_ALIGNMENT_TRAILING), D(110));
                        float2 tsz = g.Measure(tl.get());
                        brush->SetColor(D2DColor(r.trailingColor, r.trailingAlpha * (r.dim ? 0.6f : 1.0f)));
                        dc->DrawTextLayout(D2D1::Point2F(right - D(110), top + std::round((lineH - tsz.y) * 0.5f)), tl.get(), brush.get());
                        right -= tsz.x + D(10);
                    }
                    auto fitted = g.Layout(r.primary, g.Font(FontRole::Ui, 13), right - x);
                    brush->SetColor(D2DColor(0xFFFFFF, primaryAlpha));
                    dc->DrawTextLayout(D2D1::Point2F(x, top), fitted.get(), brush.get());
                    if (two && !r.secondary.empty())
                    {
                        auto sl = g.Layout(r.secondary, g.Font(FontRole::Ui, 11.5f), W - P - x);
                        brush->SetColor(D2DColor(0xFFFFFF, secondaryAlpha));
                        dc->DrawTextLayout(D2D1::Point2F(x, top + lineH + D(1)), sl.get(), brush.get());
                    }
                    break;
                }
                }
                y += rh;
            }

            if (!content.footer.empty())
            {
                float fy = h - footerH;
                brush->SetColor(D2DColor(0xFFFFFF, 0.06f));
                dc->DrawLine(D2D1::Point2F(P, fy + 0.5f), D2D1::Point2F(W - P, fy + 0.5f), brush.get(), 1.0f);
                auto fl = g.Layout(content.footer, g.Font(FontRole::Ui, 11), W - P * 2);
                float2 fs = g.Measure(fl.get());
                brush->SetColor(D2DColor(0xFFFFFF, 0.40f));
                dc->DrawTextLayout(D2D1::Point2F(P, fy + std::round((footerH - fs.y) * 0.5f)), fl.get(), brush.get());
            }
        });
        card.visual.Size(card.size);
        card.visual.CenterPoint(float3(W * 0.5f, h * 0.5f, 0));
        if (card.flash)
        {
            card.flash.Size(card.size);
            card.flashGeometry.Size(card.size);
        }
        return oldSize.x != card.size.x || oldSize.y != card.size.y;
    }

    // ---------------------------------------------------------------- layout

    RECT Dashboard::FreeRegion() const
    {
        RECT work = m_layout.primaryWork;
        if (work.right <= work.left || work.bottom <= work.top)
            work = RECT{ 0, 0, static_cast<LONG>(m_layout.size.x), static_cast<LONG>(m_layout.size.y) };
        LONG margin = static_cast<LONG>(D(32));
        RECT r = work;
        r.left = work.left + static_cast<LONG>(RectW(work) / 3.0f);
        r.right -= margin;
        r.top += margin;
        r.bottom -= margin;

        // Shrink away from icon columns that poke into the region.
        LONG pad = static_cast<LONG>(D(16));
        float mid = (r.left + r.right) * 0.5f;
        for (auto icon : m_layout.icons)
        {
            InflateRect(&icon, pad, pad);
            if (!Intersects(icon, r)) continue;
            float cx = (icon.left + icon.right) * 0.5f;
            if (cx < mid) r.left = std::max(r.left, icon.right);
            else r.right = std::min(r.right, icon.left);
        }
        // Keep clear of the clipboard shelf's resting footprint.
        if (m_reserved.right > m_reserved.left && Intersects(m_reserved, r))
            r.bottom = std::min(r.bottom, m_reserved.top - pad);
        if (RectW(r) < D(340))
        {
            r = work;
            InflateRect(&r, -margin, -margin);
        }
        return r;
    }

    void Dashboard::Relayout(bool animate)
    {
        if (!m_root || m_layout.size.x <= 0) return;
        RECT region = FreeRegion();
        float gap = D(16);
        float regionW = RectW(region);
        float cardW = std::min(D(392), regionW);

        auto kinds = ActiveKinds();
        std::vector<Card> ordered;
        for (auto kind : kinds)
        {
            auto it = std::find_if(m_cards.begin(), m_cards.end(), [kind](Card const& c) { return c.kind == kind; });
            if (it != m_cards.end())
            {
                ordered.push_back(std::move(*it));
                m_cards.erase(it);
                continue;
            }
            Card card;
            card.kind = kind;
            card.visual = m_c.CreateSpriteVisual();
            card.visual.Opacity(m_visible && animate ? 0.0f : 1.0f);
            auto shadow = m_c.CreateDropShadow();
            shadow.BlurRadius(D(30));
            shadow.Offset(float3(0, D(10), 0));
            shadow.Color(Argb(255, 0x000000));
            shadow.Opacity(0.42f);
            shadow.SourcePolicy(wuc::CompositionDropShadowSourcePolicy::InheritFromVisualContent);
            card.visual.Shadow(shadow);
            m_root.Children().InsertAtBottom(card.visual);
            if (m_visible && animate) AnimateScalar(m_c, card.visual, L"Opacity", 0.0f, 1.0f, 0.35f, 0.1f, Decelerate(m_c));
            ordered.push_back(std::move(card));
        }
        for (auto& stale : m_cards)
        {
            m_root.Children().Remove(stale.visual);
            stale.visual.Close();
        }
        m_cards = std::move(ordered);

        bool widthChanged = cardW != m_cardW;
        m_cardW = cardW;
        for (auto& card : m_cards) Draw(card, widthChanged);

        int cols = std::clamp(static_cast<int>((regionW + gap) / (cardW + gap)), 1, 3);
        cols = std::min<int>(cols, static_cast<int>(m_cards.size()));
        if (cols <= 0) return;
        std::vector<float> heights(cols, 0.0f);
        std::vector<float2> local;
        for (auto& card : m_cards)
        {
            int col = static_cast<int>(std::min_element(heights.begin(), heights.end()) - heights.begin());
            local.push_back(float2(col * (cardW + gap), heights[col]));
            heights[col] += card.size.y + gap;
        }
        float blockW = cols * cardW + (cols - 1) * gap;
        float blockH = *std::max_element(heights.begin(), heights.end()) - gap;
        float left = std::round(region.left + (regionW - blockW) * 0.5f);
        float top = std::round(region.top + std::max(0.0f, (RectH(region) - blockH) * 0.4f));

        auto ease = Decelerate(m_c);
        for (size_t i = 0; i < m_cards.size(); ++i)
        {
            auto& card = m_cards[i];
            float2 pos(left + local[i].x, top + local[i].y);
            bool moved = pos.x != card.pos.x || pos.y != card.pos.y;
            card.pos = pos;
            if (animate && m_visible && moved)
                AnimateVector3(m_c, card.visual, L"Offset", std::nullopt, float3(pos.x, pos.y, 0), 0.35f, 0.0f, ease);
            else
                card.visual.Offset(float3(pos.x, pos.y, 0));
        }
        if (m_hovered) Hover(std::nullopt);
    }

    // ---------------------------------------------------------------- input

    bool Dashboard::InsideCard(float2 pos) const
    {
        if (!m_visible) return false;
        for (auto const& card : m_cards)
        {
            if (pos.x >= card.pos.x && pos.x < card.pos.x + card.size.x && pos.y >= card.pos.y && pos.y < card.pos.y + card.size.y)
                return true;
        }
        return false;
    }

    std::optional<Dashboard::Hit> Dashboard::HitTest(float2 pos) const
    {
        if (!m_visible) return std::nullopt;
        for (size_t c = 0; c < m_cards.size(); ++c)
        {
            auto const& card = m_cards[c];
            float2 p = pos - card.pos;
            if (p.x < 0 || p.y < 0 || p.x >= card.size.x || p.y >= card.size.y) continue;
            for (size_t i = 0; i < card.items.size(); ++i)
            {
                auto const& r = card.items[i].rect;
                if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom)
                    return Hit{ static_cast<int>(c), static_cast<int>(i), card.items[i].url };
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    void Dashboard::Hover(std::optional<float2> pos)
    {
        if (!m_hover) return;
        std::optional<Hit> hit = pos ? HitTest(*pos) : std::nullopt;
        if (hit == m_hovered) return;
        bool wasVisible = m_hovered.has_value();
        m_hovered = hit;
        auto ease = Decelerate(m_c);
        if (!hit)
        {
            AnimateScalar(m_c, m_hover, L"Opacity", std::nullopt, 0.0f, 0.15f, 0.0f, ease);
            return;
        }
        auto const& card = m_cards[hit->card];
        auto const& r = card.items[hit->item].rect;
        float3 offset(card.pos.x + r.left, card.pos.y + r.top, 0);
        float2 size(r.right - r.left, r.bottom - r.top);
        m_hoverGeometry.CornerRadius(float2(D(8), D(8)));
        if (wasVisible)
        {
            AnimateVector3(m_c, m_hover, L"Offset", std::nullopt, offset, 0.16f, 0.0f, ease);
            auto a = m_c.CreateVector2KeyFrameAnimation();
            a.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
            a.InsertKeyFrame(1.0f, size, ease);
            a.Duration(Seconds(0.16f));
            m_hoverGeometry.StartAnimation(L"Size", a);
        }
        else
        {
            m_hover.Offset(offset);
            m_hoverGeometry.Size(size);
        }
        m_hover.Size(float2(std::max(size.x, D(400)), std::max(size.y, D(60))));
        AnimateScalar(m_c, m_hover, L"Opacity", std::nullopt, 1.0f, 0.12f, 0.0f, ease);
    }

    void Dashboard::Activate(Hit const& hit)
    {
        if (hit.card < 0 || hit.card >= static_cast<int>(m_cards.size())) return;
        auto& card = m_cards[hit.card];
        if (hit.item < 0 || hit.item >= static_cast<int>(card.items.size())) return;
        auto const& r = card.items[hit.item].rect;

        auto geometry = m_c.CreateRoundedRectangleGeometry();
        geometry.CornerRadius(float2(D(8), D(8)));
        geometry.Size(float2(r.right - r.left, r.bottom - r.top));
        auto shape = m_c.CreateSpriteShape(geometry);
        shape.FillBrush(m_c.CreateColorBrush(Argb(70, palette::Accent)));
        shape.StrokeBrush(m_c.CreateColorBrush(Argb(200, palette::Accent)));
        shape.StrokeThickness(1.5f);
        auto flash = m_c.CreateShapeVisual();
        flash.Shapes().Append(shape);
        flash.Size(float2(r.right - r.left + 2, r.bottom - r.top + 2));
        flash.Offset(float3(card.pos.x + r.left, card.pos.y + r.top, 0));
        m_root.Children().InsertAtTop(flash);
        auto batch = m_c.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        AnimateScalar(m_c, flash, L"Opacity", 1.0f, 0.0f, 0.7f, 0.08f, Decelerate(m_c));
        batch.End();
        auto root = m_root;
        batch.Completed([root, flash](auto&&, auto&&)
        {
            root.Children().Remove(flash);
            flash.Close();
        });
        Log(L"Dashboard: open %s", hit.url.c_str());
        if (hit.url.rfind(L"folder:", 0) == 0) OpenFolder(hit.url.substr(7));
        else OpenUrl(hit.url);
    }

    void Dashboard::FlashCard(Card& card, uint32_t rgb)
    {
        if (!card.flash)
        {
            card.flashGeometry = m_c.CreateRoundedRectangleGeometry();
            card.flashGeometry.CornerRadius(float2(D(12), D(12)));
            card.flashGeometry.Size(card.size);
            auto shape = m_c.CreateSpriteShape(card.flashGeometry);
            shape.FillBrush(m_c.CreateColorBrush(Argb(34, rgb)));
            shape.StrokeBrush(m_c.CreateColorBrush(Argb(230, rgb)));
            shape.StrokeThickness(1.5f);
            card.flash = m_c.CreateShapeVisual();
            card.flash.Shapes().Append(shape);
            card.flash.Size(card.size);
            card.flash.Opacity(0.0f);
            card.visual.Children().InsertAtTop(card.flash);
        }
        auto a = m_c.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(0.0f, 0.0f);
        a.InsertKeyFrame(0.12f, 1.0f);
        a.InsertKeyFrame(0.35f, 0.35f);
        a.InsertKeyFrame(0.5f, 0.9f);
        a.InsertKeyFrame(1.0f, 0.0f);
        a.Duration(Seconds(1.6f));
        card.flash.StartAnimation(L"Opacity", a);
    }
}
