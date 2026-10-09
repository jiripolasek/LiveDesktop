#include "pch.h"
#include "Providers.h"
#include "Log.h"
#include "Util.h"
#include <fstream>

using namespace std::chrono;

// Working trees: the repos I'm actually touching. Sources, in order:
//  1. providers.worktrees.paths from config.json
//  2. git_root (or cwd) of Copilot sessions whose workspace.yaml / events.jsonl changed in the
//     last 7 days, so agents' repos show up automatically.
// For each, `git --no-optional-locks status --porcelain=v2 --branch` gives branch, upstream,
// ahead/behind and change counts without taking index.lock or touching the network.
namespace ld
{
    namespace
    {
        constexpr auto kSessionWindow = hours(24 * 7);

        TimePoint WriteTime(std::filesystem::path const& p)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data)) return {};
            return FromFileTime(data.ftLastWriteTime);
        }

        // Walks up from `dir` to the folder containing .git (dir or file, for worktrees).
        std::wstring GitRoot(std::wstring const& dir)
        {
            std::error_code ec;
            std::filesystem::path p(dir);
            for (int i = 0; i < 12 && !p.empty(); ++i)
            {
                if (std::filesystem::exists(p / L".git", ec)) return p.wstring();
                auto parent = p.parent_path();
                if (parent == p) break;
                p = parent;
            }
            return {};
        }

        std::wstring Normalize(std::wstring path)
        {
            while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
            std::replace(path.begin(), path.end(), L'/', L'\\');
            return path;
        }
    }

    WorktreesProvider::WorktreesProvider(Config const& config)
        : SnapshotProvider(L"worktrees", config.worktrees), m_paths(config.worktreePaths), m_max(config.worktreeMax)
    {
        m_sessionRoot = KnownFolder(FOLDERID_Profile) + L"\\.copilot\\session-state";
    }

    std::vector<WorktreesProvider::Candidate> WorktreesProvider::Discover() const
    {
        std::vector<Candidate> out;
        auto add = [&](std::wstring root, TimePoint activity, bool fromSession)
        {
            root = Normalize(root);
            if (root.empty()) return;
            auto key = ToLower(root);
            for (auto& c : out)
            {
                if (ToLower(c.root) == key)
                {
                    c.activity = std::max(c.activity, activity);
                    c.fromSession = c.fromSession || fromSession;
                    return;
                }
            }
            out.push_back({ root, activity, fromSession });
        };

        for (auto const& path : m_paths)
        {
            auto root = GitRoot(path);
            if (root.empty()) continue;
            auto activity = std::max(WriteTime(std::filesystem::path(root) / L".git" / L"index"), WriteTime(std::filesystem::path(root) / L".git" / L"HEAD"));
            add(root, activity, false);
        }

        std::error_code ec;
        auto cutoff = Clock::now() - kSessionWindow;
        for (std::filesystem::directory_iterator it(m_sessionRoot, ec), end; !ec && it != end; it.increment(ec))
        {
            if (Stopping()) break;
            if (!it->is_directory(ec)) continue;
            auto yaml = it->path() / L"workspace.yaml";
            auto activity = std::max(WriteTime(yaml), WriteTime(it->path() / L"events.jsonl"));
            if (activity < cutoff) continue;

            std::ifstream in(yaml, std::ios::binary);
            std::string line, gitRoot, cwd;
            for (int n = 0; n < 40 && std::getline(in, line); ++n)
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("git_root:", 0) == 0) gitRoot = line.substr(9);
                else if (line.rfind("cwd:", 0) == 0) cwd = line.substr(4);
            }
            auto dir = Trim(Utf8ToWide(gitRoot.empty() ? cwd : gitRoot));
            if (dir.size() >= 2 && (dir.front() == L'"' || dir.front() == L'\'')) dir = dir.substr(1, dir.size() - 2);
            if (dir.empty()) continue;
            auto root = GitRoot(dir);
            if (!root.empty()) add(root, activity, true);
        }

        std::sort(out.begin(), out.end(), [](Candidate const& a, Candidate const& b) { return a.activity > b.activity; });
        if (out.size() > static_cast<size_t>(m_max)) out.resize(m_max);
        return out;
    }

    bool WorktreesProvider::Status(Candidate const& c, WorkingTree& tree)
    {
        ProcessResult result;
        if (!RunProcess(L"\"" + m_git + L"\" --no-optional-locks -C \"" + c.root + L"\" status --porcelain=v2 --branch", 15'000, result, StopFlag()))
            return false;
        if (result.exitCode != 0) return false;

        tree.path = c.root;
        tree.name = std::filesystem::path(c.root).filename().wstring();
        tree.lastActivity = c.activity;
        tree.fromSession = c.fromSession;
        size_t pos = 0;
        auto const& out = result.out;
        while (pos < out.size())
        {
            size_t eol = out.find('\n', pos);
            if (eol == std::string::npos) eol = out.size();
            std::string_view line(out.data() + pos, eol - pos);
            pos = eol + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty()) continue;

            if (line.rfind("# branch.head ", 0) == 0) tree.branch = Utf8ToWide(line.substr(14));
            else if (line.rfind("# branch.upstream ", 0) == 0) tree.upstream = Utf8ToWide(line.substr(18));
            else if (line.rfind("# branch.ab ", 0) == 0)
            {
                int a = 0, b = 0;
                if (sscanf_s(std::string(line.substr(12)).c_str(), "+%d -%d", &a, &b) == 2)
                {
                    tree.ahead = a;
                    tree.behind = b;
                }
            }
            else if ((line[0] == '1' || line[0] == '2') && line.size() > 3)
            {
                if (line[2] != '.') ++tree.staged;
                if (line[3] != '.') ++tree.modified;
            }
            else if (line[0] == 'u') ++tree.conflicts;
            else if (line[0] == '?') ++tree.untracked;
        }
        return true;
    }

    bool WorktreesProvider::PollReal()
    {
        if (m_git.empty())
        {
            wchar_t path[MAX_PATH]{};
            if (!SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, path, nullptr))
            {
                Log(L"[worktrees] git.exe not found on PATH");
                return false;
            }
            m_git = path;
        }

        WorktreesSnapshot snap;
        for (auto const& c : Discover())
        {
            if (Stopping()) return true;
            WorkingTree tree;
            if (Status(c, tree)) snap.trees.push_back(std::move(tree));
            else Log(L"[worktrees] git status failed in %s", c.root.c_str());
        }
        if (snap.trees.size() != m_lastCount)
        {
            m_lastCount = snap.trees.size();
            std::wstring names;
            for (auto const& t : snap.trees) names += (names.empty() ? L"" : L", ") + t.name + L"@" + t.branch + L" (" + std::to_wstring(t.Dirty()) + L" dirty)";
            Log(L"[worktrees] %zu working trees: %s", snap.trees.size(), names.c_str());
        }
        Publish(std::move(snap));
        return true;
    }

    void WorktreesProvider::PollMock()
    {
        int t = m_mockTick++;
        auto now = Clock::now();
        WorktreesSnapshot snap;
        snap.mock = true;

        WorkingTree a;
        a.path = L"C:\\src\\contoso-web";
        a.name = L"contoso-web";
        a.branch = L"fix/checkout-flaky-tests";
        a.upstream = L"origin/fix/checkout-flaky-tests";
        a.ahead = 2 + (t / 6) % 2;
        a.modified = 3 + t % 3;
        a.untracked = 1;
        a.lastActivity = now - seconds(40);
        a.fromSession = true;
        snap.trees.push_back(a);

        WorkingTree b;
        b.path = L"C:\\src\\queue-worker";
        b.name = L"queue-worker";
        b.branch = L"investigate/memory-leak";
        b.upstream = L"origin/investigate/memory-leak";
        b.staged = 2;
        b.modified = 1;
        b.behind = 4;
        b.lastActivity = now - minutes(6);
        b.fromSession = true;
        snap.trees.push_back(b);

        WorkingTree c;
        c.path = L"C:\\src\\settings-ui";
        c.name = L"settings-ui";
        c.branch = L"feature/dark-mode";
        c.ahead = 5;
        c.lastActivity = now - minutes(22);
        c.fromSession = true;
        snap.trees.push_back(c);

        WorkingTree d;
        d.path = L"C:\\src\\design-tokens";
        d.name = L"design-tokens";
        d.branch = L"main";
        d.upstream = L"origin/main";
        d.lastActivity = now - hours(3);
        snap.trees.push_back(d);

        Publish(std::move(snap));
    }
}
