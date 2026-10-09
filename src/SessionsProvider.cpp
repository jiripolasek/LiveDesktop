#include "pch.h"
#include "Providers.h"
#include "Log.h"
#include "Util.h"

using namespace std::chrono;

// Reads Copilot CLI / GitHub Copilot app session state from %USERPROFILE%\.copilot\session-state\<id>\
// strictly read-only (files are opened with full sharing and never written).
//
// Signals used per session directory:
//   workspace.yaml     -> name (first prompt or user-given name), repository/cwd, branch
//   inuse.<pid>.lock   -> a live CLI process currently owns the session (pid checked for liveness)
//   events.jsonl       -> append-only event log; we tail it incrementally and track
//                         assistant.turn_start/turn_end, tool.execution_start/complete (pending tools),
//                         permission.requested/completed, user.message, session.task_complete, session.shutdown
namespace ld
{
    namespace
    {
        constexpr uint64_t kInitialTail = 512 * 1024;
        constexpr auto kDoneWindow = minutes(20);

        bool IsWaitTool(std::string_view name)
        {
            return name == "ask_user" || name == "exit_plan_mode" || name == "request_user_input";
        }

        // Value of the first `"key":"..."` occurrence, with basic JSON unescaping.
        std::string JsonStringField(std::string_view line, std::string_view key, bool fromEnd = false)
        {
            std::string pattern = "\"" + std::string(key) + "\":\"";
            size_t pos = fromEnd ? line.rfind(pattern) : line.find(pattern);
            if (pos == std::string_view::npos) return {};
            pos += pattern.size();
            std::string value;
            for (size_t i = pos; i < line.size(); ++i)
            {
                char ch = line[i];
                if (ch == '"') break;
                if (ch == '\\' && i + 1 < line.size())
                {
                    char esc = line[++i];
                    switch (esc)
                    {
                    case 'n': value += ' '; break;
                    case 't': value += ' '; break;
                    case 'r': break;
                    case 'u':
                        if (i + 4 < line.size())
                        {
                            unsigned cp = std::strtoul(std::string(line.substr(i + 1, 4)).c_str(), nullptr, 16);
                            i += 4;
                            if (cp < 0x80) value += static_cast<char>(cp);
                            else if (cp < 0x800) { value += static_cast<char>(0xC0 | (cp >> 6)); value += static_cast<char>(0x80 | (cp & 0x3F)); }
                            else { value += static_cast<char>(0xE0 | (cp >> 12)); value += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); value += static_cast<char>(0x80 | (cp & 0x3F)); }
                        }
                        break;
                    default: value += esc; break;
                    }
                    continue;
                }
                value += ch;
                if (value.size() > 400) break;
            }
            return value;
        }

        bool FileInfo(std::wstring const& path, uint64_t& size, FILETIME& mtime)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
            size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            mtime = data.ftLastWriteTime;
            return true;
        }

        bool IsProcessAlive(DWORD pid)
        {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
            DWORD code = 0;
            bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
            CloseHandle(h);
            return alive;
        }

        bool IsSessionDirName(std::wstring const& name)
        {
            return name.size() == 36 && name[8] == L'-' && name[13] == L'-' && name[18] == L'-' && name[23] == L'-';
        }
    }

    struct SessionsProvider::Tracked
    {
        std::wstring id;
        std::wstring dir;
        // workspace.yaml
        FILETIME workspaceTime{};
        std::wstring title, repo, branch;
        // events.jsonl tail state
        uint64_t offset = 0;
        std::string partial;
        bool inTurn = false;
        bool shutdown = false;
        bool taskComplete = false;
        std::map<std::string, std::string> pendingTools; // toolCallId -> toolName
        std::set<std::string> pendingPermissions;
        std::string waitDetail;
        std::string runningTool;
        TimePoint turnStart{}, waitStart{}, doneAt{}, lastEvent{};
        TimePoint eventsTime{};
        bool live = false;

        void ReadWorkspace()
        {
            uint64_t size = 0;
            FILETIME mtime{};
            auto path = dir + L"\\workspace.yaml";
            if (!FileInfo(path, size, mtime) || CompareFileTime(&mtime, &workspaceTime) == 0) return;
            workspaceTime = mtime;

            HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) return;
            std::string text(static_cast<size_t>(std::min<uint64_t>(size, 256 * 1024)), '\0');
            DWORD read = 0;
            ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
            CloseHandle(h);
            text.resize(read);

            std::string name, repository, cwd, gitBranch;
            bool userNamed = false;
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) end = text.size();
                std::string_view line(text.data() + pos, end - pos);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                pos = end + 1;
                auto colon = line.find(": ");
                if (line.empty() || line[0] == ' ' || colon == std::string_view::npos)
                {
                    if (line.size() > 1 && line.back() == ':' && line[0] != ' ') colon = line.size() - 1;
                    else continue;
                }
                auto key = line.substr(0, colon);
                auto value = colon + 2 <= line.size() ? line.substr(colon + 2) : std::string_view{};
                if (key == "name")
                {
                    if (value.starts_with("|") || value.starts_with(">"))
                    {
                        // Block scalar: take the first non-empty indented line.
                        while (pos < text.size())
                        {
                            size_t e = text.find('\n', pos);
                            if (e == std::string::npos) e = text.size();
                            std::string_view l(text.data() + pos, e - pos);
                            if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
                            if (!l.empty() && l[0] != ' ') break;
                            pos = e + 1;
                            auto first = l.find_first_not_of(' ');
                            if (name.empty() && first != std::string_view::npos) name = std::string(l.substr(first));
                        }
                    }
                    else
                    {
                        name = std::string(value);
                        if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'')) name = name.substr(1, name.size() - 2);
                    }
                }
                else if (key == "user_named") userNamed = value == "true";
                else if (key == "repository") repository = std::string(value);
                else if (key == "cwd") cwd = std::string(value);
                else if (key == "branch") gitBranch = std::string(value);
            }

            auto wname = Trim(Utf8ToWide(name));
            // First prompts are often long instructions: keep the first sentence-ish fragment.
            if (!userNamed)
            {
                auto cut = wname.find_first_of(L".:\u2014");
                if (cut != std::wstring::npos && cut > 12) wname = wname.substr(0, cut);
            }
            if (!repository.empty()) repo = Utf8ToWide(repository);
            else if (!cwd.empty()) repo = std::filesystem::path(Utf8ToWide(cwd)).filename().wstring();
            branch = Utf8ToWide(gitBranch);
            title = Truncate(wname.empty() ? (repo.empty() ? id.substr(0, 8) : repo) : wname, 64);
        }

        void HandleLine(std::string_view line)
        {
            if (line.size() < 12) return;
            auto type = JsonStringField(line.substr(0, 96), "type");
            if (type.empty()) return;
            TimePoint ts{};
            auto stamp = JsonStringField(line.substr(line.size() > 200 ? line.size() - 200 : 0), "timestamp", true);
            if (stamp.empty() || !ParseIsoTime(stamp, ts)) ts = lastEvent;
            if (ts > lastEvent) lastEvent = ts;

            if (type == "user.message")
            {
                taskComplete = false;
                shutdown = false;
                turnStart = ts;
            }
            else if (type == "assistant.turn_start")
            {
                if (!inTurn) turnStart = ts;
                inTurn = true;
                shutdown = false;
            }
            else if (type == "assistant.turn_end")
            {
                inTurn = false;
                doneAt = ts;
            }
            else if (type == "tool.execution_start")
            {
                auto head = line.substr(0, std::min<size_t>(line.size(), 600));
                auto callId = JsonStringField(head, "toolCallId");
                auto toolName = JsonStringField(head, "toolName");
                if (!callId.empty()) pendingTools[callId] = toolName;
                runningTool = toolName;
                if (IsWaitTool(toolName))
                {
                    waitStart = ts;
                    waitDetail = JsonStringField(line, "question");
                    if (waitDetail.empty()) waitDetail = toolName == "exit_plan_mode" ? "Plan ready for approval" : "Waiting for your input";
                }
            }
            else if (type == "tool.execution_complete")
            {
                auto callId = JsonStringField(line.substr(0, std::min<size_t>(line.size(), 600)), "toolCallId");
                pendingTools.erase(callId);
                if (pendingTools.empty()) runningTool.clear();
            }
            else if (type == "permission.requested")
            {
                auto requestId = JsonStringField(line, "requestId");
                pendingPermissions.insert(requestId);
                waitStart = ts;
                waitDetail = "Permission requested";
            }
            else if (type == "permission.completed")
            {
                pendingPermissions.erase(JsonStringField(line, "requestId"));
            }
            else if (type == "session.task_complete")
            {
                taskComplete = true;
                doneAt = ts;
            }
            else if (type == "session.shutdown")
            {
                shutdown = true;
                inTurn = false;
                pendingTools.clear();
                pendingPermissions.clear();
            }
            else if (type == "session.start" || type == "session.resume")
            {
                shutdown = false;
                // Tools that were pending when a previous process died never complete.
                pendingTools.clear();
                pendingPermissions.clear();
                inTurn = false;
            }
        }

        void ReadEvents()
        {
            uint64_t size = 0;
            FILETIME mtime{};
            auto path = dir + L"\\events.jsonl";
            if (!FileInfo(path, size, mtime)) return;
            eventsTime = FromFileTime(mtime);
            if (size < offset)
            {
                offset = 0; // truncated / replaced: start over
                partial.clear();
            }
            if (size == offset) return;

            bool initial = offset == 0;
            uint64_t start = initial && size > kInitialTail ? size - kInitialTail : offset;
            HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (h == INVALID_HANDLE_VALUE) return;
            LARGE_INTEGER li{};
            li.QuadPart = static_cast<LONGLONG>(start);
            SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
            std::string data(static_cast<size_t>(std::min<uint64_t>(size - start, 8 * 1024 * 1024)), '\0');
            DWORD read = 0;
            ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
            CloseHandle(h);
            data.resize(read);
            offset = start + read;

            size_t pos = 0;
            if (initial && start > 0)
            {
                pos = data.find('\n'); // skip the partial first line
                pos = pos == std::string::npos ? data.size() : pos + 1;
            }
            std::string buffer = std::move(partial);
            partial.clear();
            while (pos < data.size())
            {
                size_t end = data.find('\n', pos);
                if (end == std::string::npos)
                {
                    partial = buffer + data.substr(pos);
                    buffer.clear();
                    break;
                }
                if (!buffer.empty())
                {
                    buffer.append(data, pos, end - pos);
                    HandleLine(buffer);
                    buffer.clear();
                }
                else
                {
                    HandleLine(std::string_view(data).substr(pos, end - pos));
                }
                pos = end + 1;
            }
            if (partial.size() > 4 * 1024 * 1024) partial.clear();
        }

        void CheckLive()
        {
            live = false;
            WIN32_FIND_DATAW fd{};
            HANDLE find = FindFirstFileW((dir + L"\\inuse.*.lock").c_str(), &fd);
            if (find == INVALID_HANDLE_VALUE) return;
            do
            {
                DWORD pid = wcstoul(fd.cFileName + 6, nullptr, 10);
                if (pid && IsProcessAlive(pid)) live = true;
            } while (!live && FindNextFileW(find, &fd));
            FindClose(find);
        }

        AgentSession Snapshot(TimePoint now) const
        {
            AgentSession s;
            s.id = id;
            s.title = title;
            s.repo = repo;
            s.branch = branch;
            s.live = live;
            s.lastActivity = std::max(lastEvent, eventsTime);
            bool waitingTool = std::any_of(pendingTools.begin(), pendingTools.end(), [](auto const& kv) { return IsWaitTool(kv.second); });
            if (live && !shutdown && (waitingTool || !pendingPermissions.empty()))
            {
                s.status = AgentStatus::Waiting;
                s.since = waitStart;
                s.detail = Truncate(Utf8ToWide(waitDetail), 90);
            }
            else if (live && !shutdown && (inTurn || !pendingTools.empty()))
            {
                s.status = AgentStatus::Working;
                s.since = turnStart;
                if (!runningTool.empty()) s.detail = L"Running " + Utf8ToWide(runningTool);
            }
            else if (doneAt != TimePoint{} && (taskComplete || now - doneAt < kDoneWindow) && now - doneAt < hours(12))
            {
                s.status = AgentStatus::Done;
                s.since = doneAt;
                s.detail = taskComplete ? L"Task complete" : L"Turn finished";
            }
            else
            {
                s.status = AgentStatus::Idle;
                s.since = doneAt != TimePoint{} ? doneAt : s.lastActivity;
            }
            if (s.status == AgentStatus::Done && now - s.since > kDoneWindow) s.status = AgentStatus::Idle;
            if (s.since == TimePoint{} || s.since > now) s.since = s.lastActivity;
            return s;
        }
    };

    SessionsProvider::SessionsProvider(Config const& config)
        : SnapshotProvider(L"sessions", config.sessions), m_maxIdleHours(config.sessionMaxIdleHours), m_maxOrbs(config.maxOrbs)
    {
        m_root = KnownFolder(FOLDERID_Profile) + L"\\.copilot\\session-state";
    }

    SessionsProvider::~SessionsProvider() { Stop(); }

    void SessionsProvider::OnModeChanged()
    {
        m_tracked.clear();
        m_lastRescan = 0;
        m_mockStart = steady_clock::now();
    }

    void SessionsProvider::Rescan()
    {
        auto now = Clock::now();
        auto cutoff = now - duration_cast<Clock::duration>(duration<double, std::ratio<3600>>(m_maxIdleHours));
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileExW((m_root + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) return;
        std::set<std::wstring> seen;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsSessionDirName(fd.cFileName)) continue;
            std::wstring dir = m_root + L"\\" + fd.cFileName;
            uint64_t size = 0;
            FILETIME mtime{};
            bool recent = FileInfo(dir + L"\\events.jsonl", size, mtime) && FromFileTime(mtime) >= cutoff;
            if (!recent)
            {
                WIN32_FIND_DATAW lock{};
                HANDLE lf = FindFirstFileW((dir + L"\\inuse.*.lock").c_str(), &lock);
                if (lf != INVALID_HANDLE_VALUE)
                {
                    recent = IsProcessAlive(wcstoul(lock.cFileName + 6, nullptr, 10));
                    FindClose(lf);
                }
            }
            if (!recent) continue;
            seen.insert(fd.cFileName);
            if (!m_tracked.contains(fd.cFileName))
            {
                auto t = std::make_unique<Tracked>();
                t->id = fd.cFileName;
                t->dir = dir;
                m_tracked.emplace(fd.cFileName, std::move(t));
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        std::erase_if(m_tracked, [&](auto const& kv) { return !seen.contains(kv.first); });
    }

    bool SessionsProvider::PollReal()
    {
        if (GetFileAttributesW(m_root.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Publish({});
            return false;
        }
        if (m_lastRescan == 0 || GetTickCount64() - m_lastRescan > 60'000)
        {
            Rescan();
            m_lastRescan = GetTickCount64();
        }

        auto now = Clock::now();
        auto maxIdle = duration_cast<Clock::duration>(duration<double, std::ratio<3600>>(m_maxIdleHours));
        SessionsSnapshot snap;
        for (auto& [id, t] : m_tracked)
        {
            if (Stopping()) return true;
            t->ReadWorkspace();
            t->ReadEvents();
            t->CheckLive();
            auto s = t->Snapshot(now);
            if (s.status == AgentStatus::Idle && !s.live && now - s.lastActivity > maxIdle) continue;
            snap.sessions.push_back(std::move(s));
        }
        std::sort(snap.sessions.begin(), snap.sessions.end(), [](AgentSession const& a, AgentSession const& b)
        {
            return a.lastActivity > b.lastActivity;
        });
        // Waiting first, then working, done, idle (recency within each group).
        std::stable_sort(snap.sessions.begin(), snap.sessions.end(), [](AgentSession const& a, AgentSession const& b)
        {
            auto rank = [](AgentStatus s) { return s == AgentStatus::Waiting ? 0 : s == AgentStatus::Working ? 1 : s == AgentStatus::Done ? 2 : 3; };
            return rank(a.status) < rank(b.status);
        });
        if (snap.sessions.size() > static_cast<size_t>(m_maxOrbs)) snap.sessions.resize(static_cast<size_t>(m_maxOrbs));
        if (snap.sessions.size() != m_lastCount)
        {
            m_lastCount = snap.sessions.size();
            int counts[4]{};
            for (auto const& s : snap.sessions) counts[static_cast<int>(s.status)]++;
            Log(L"[sessions] %zu recent session(s) (tracking %zu): %d working, %d waiting, %d done, %d idle", snap.sessions.size(),
                m_tracked.size(), counts[0], counts[1], counts[2], counts[3]);
        }
        Publish(std::move(snap));
        return true;
    }

    void SessionsProvider::PollMock()
    {
        struct Phase { AgentStatus status; int seconds; const wchar_t* detail; };
        struct MockSession
        {
            const wchar_t* id; const wchar_t* title; const wchar_t* repo; const wchar_t* branch;
            int offset; std::vector<Phase> phases;
        };
        static const std::vector<MockSession> sessions = {
            { L"7d1f2c9a-0000-4000-8000-000000000001", L"Add dark mode to settings page", L"contoso/portal", L"feat/dark-mode", 0,
              { { AgentStatus::Waiting, 75, L"Approve plan: migrate theme tokens to CSS vars?" }, { AgentStatus::Working, 45, L"Running edit" }, { AgentStatus::Done, 50, L"Task complete" } } },
            { L"7d1f2c9a-0000-4000-8000-000000000002", L"Fix flaky checkout integration tests", L"contoso/storefront", L"fix/flaky-checkout", 10,
              { { AgentStatus::Working, 55, L"Running npm test" }, { AgentStatus::Done, 45, L"Task complete" }, { AgentStatus::Working, 40, L"Running powershell" } } },
            { L"7d1f2c9a-0000-4000-8000-000000000003", L"Investigate memory leak in queue worker", L"contoso/pipeline", L"main", 0,
              { { AgentStatus::Working, 110, L"Running dotnet-counters" }, { AgentStatus::Waiting, 40, L"Which heap dump should I analyze?" } } },
            { L"7d1f2c9a-0000-4000-8000-000000000004", L"Draft release notes for v2.4", L"contoso/docs", L"release/2.4", 0,
              { { AgentStatus::Done, 100000, L"Task complete" } } },
            { L"7d1f2c9a-0000-4000-8000-000000000005", L"Bump dependencies and regenerate lockfiles", L"contoso/infra", L"chore/deps", 0,
              { { AgentStatus::Idle, 100000, L"" } } },
        };
        static const int staticAgeSeconds[] = { 0, 0, 0, 14 * 60, 3 * 3600 + 20 * 60 };

        auto now = Clock::now();
        auto elapsed = static_cast<int>(duration_cast<seconds>(steady_clock::now() - m_mockStart).count());
        SessionsSnapshot snap;
        snap.mock = true;
        for (size_t i = 0; i < sessions.size(); ++i)
        {
            auto const& m = sessions[i];
            int cycle = 0;
            for (auto const& p : m.phases) cycle += p.seconds;
            int t = (elapsed + m.offset) % std::max(1, cycle);
            int phaseStart = 0;
            Phase const* phase = &m.phases.front();
            for (auto const& p : m.phases)
            {
                if (t < phaseStart + p.seconds) { phase = &p; break; }
                phaseStart += p.seconds;
            }
            AgentSession s;
            s.id = m.id;
            s.title = m.title;
            s.repo = m.repo;
            s.branch = m.branch;
            s.status = phase->status;
            s.detail = phase->detail;
            s.live = phase->status != AgentStatus::Idle;
            int age = staticAgeSeconds[i] ? staticAgeSeconds[i] : (t - phaseStart) + (phase->status == AgentStatus::Working ? 240 : 30);
            s.since = now - seconds(age);
            s.lastActivity = phase->status == AgentStatus::Idle || phase->status == AgentStatus::Done ? s.since : now;
            snap.sessions.push_back(std::move(s));
        }
        std::stable_sort(snap.sessions.begin(), snap.sessions.end(), [](AgentSession const& a, AgentSession const& b)
        {
            auto rank = [](AgentStatus s) { return s == AgentStatus::Waiting ? 0 : s == AgentStatus::Working ? 1 : s == AgentStatus::Done ? 2 : 3; };
            return rank(a.status) < rank(b.status);
        });
        Publish(std::move(snap));
    }
}
