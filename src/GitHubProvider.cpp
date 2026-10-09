#include "pch.h"
#include "Providers.h"
#include "Log.h"
#include "Util.h"

namespace wdj = winrt::Windows::Data::Json;
using namespace std::chrono;

// Pull requests via the GitHub CLI (`gh`), using whatever account `gh auth login` set up.
//  - No repos configured: one GraphQL call returns my open PRs (+ statusCheckRollup) and review requests.
//  - Repos configured: `gh pr list --repo X --author @me ...` and `--search review-requested:@me` per repo.
namespace ld
{
    namespace
    {
        constexpr wchar_t kGraphQl[] =
            L"query { viewer { pullRequests(first:20, states:OPEN, orderBy:{field:UPDATED_AT,direction:DESC}) { nodes { "
            L"number title url isDraft updatedAt repository { nameWithOwner } "
            L"commits(last:1) { nodes { commit { statusCheckRollup { state } } } } } } } "
            L"search(query:\\\"is:open is:pr review-requested:@me archived:false\\\", type:ISSUE, first:20) { nodes { "
            L"... on PullRequest { number title url updatedAt author { login } repository { nameWithOwner } } } } }";

        std::wstring Str(wdj::JsonObject const& o, wchar_t const* key)
        {
            if (!o || !o.HasKey(key)) return {};
            auto v = o.GetNamedValue(key);
            return v.ValueType() == wdj::JsonValueType::String ? std::wstring(v.GetString()) : std::wstring{};
        }

        wdj::JsonObject Obj(wdj::JsonObject const& o, wchar_t const* key)
        {
            if (!o || !o.HasKey(key)) return nullptr;
            auto v = o.GetNamedValue(key);
            return v.ValueType() == wdj::JsonValueType::Object ? v.GetObject() : nullptr;
        }

        wdj::JsonArray Arr(wdj::JsonObject const& o, wchar_t const* key)
        {
            if (!o || !o.HasKey(key)) return nullptr;
            auto v = o.GetNamedValue(key);
            return v.ValueType() == wdj::JsonValueType::Array ? v.GetArray() : nullptr;
        }

        CheckState RollupState(std::wstring const& state)
        {
            if (state == L"SUCCESS") return CheckState::Success;
            if (state == L"FAILURE" || state == L"ERROR") return CheckState::Failure;
            if (state == L"PENDING" || state == L"EXPECTED") return CheckState::Pending;
            return CheckState::None;
        }

        // `gh pr list --json statusCheckRollup` returns individual CheckRun / StatusContext entries.
        CheckState FromChecks(wdj::JsonArray const& checks)
        {
            if (!checks || checks.Size() == 0) return CheckState::None;
            bool pending = false;
            for (auto const& v : checks)
            {
                if (v.ValueType() != wdj::JsonValueType::Object) continue;
                auto c = v.GetObject();
                auto conclusion = Str(c, L"conclusion");
                auto status = Str(c, L"status");
                auto state = Str(c, L"state");
                if (conclusion == L"FAILURE" || conclusion == L"TIMED_OUT" || conclusion == L"CANCELLED" || conclusion == L"ACTION_REQUIRED" ||
                    conclusion == L"STARTUP_FAILURE" || state == L"FAILURE" || state == L"ERROR")
                    return CheckState::Failure;
                if ((!status.empty() && status != L"COMPLETED") || state == L"PENDING" || state == L"EXPECTED") pending = true;
            }
            return pending ? CheckState::Pending : CheckState::Success;
        }

        PullRequest ParsePr(wdj::JsonObject const& n, std::wstring const& fallbackRepo)
        {
            PullRequest pr;
            pr.number = static_cast<int>(n.HasKey(L"number") ? n.GetNamedNumber(L"number", 0) : 0);
            pr.title = Str(n, L"title");
            pr.url = Str(n, L"url");
            pr.repo = Str(Obj(n, L"repository"), L"nameWithOwner");
            if (pr.repo.empty()) pr.repo = fallbackRepo;
            pr.author = Str(Obj(n, L"author"), L"login");
            pr.draft = n.HasKey(L"isDraft") && n.GetNamedValue(L"isDraft").ValueType() == wdj::JsonValueType::Boolean && n.GetNamedBoolean(L"isDraft");
            ParseIsoTime(WideToUtf8(Str(n, L"updatedAt")), pr.updated);
            return pr;
        }
    }

    GitHubProvider::GitHubProvider(Config const& config) : SnapshotProvider(L"github", config.github), m_repos(config.repos) {}

    bool GitHubProvider::RunGh(std::wstring const& args, std::string& out)
    {
        if (m_gh.empty())
        {
            wchar_t path[MAX_PATH]{};
            if (!SearchPathW(nullptr, L"gh.exe", nullptr, MAX_PATH, path, nullptr))
            {
                Log(L"[github] gh.exe not found on PATH - install GitHub CLI and run `gh auth login`");
                return false;
            }
            m_gh = path;
        }
        ProcessResult result;
        if (!RunProcess(L"\"" + m_gh + L"\" " + args, 45'000, result, StopFlag()))
        {
            Log(L"[github] gh timed out or failed to start");
            return false;
        }
        if (result.exitCode != 0)
        {
            auto err = Utf8ToWide(result.err.substr(0, 300));
            Log(L"[github] gh exited %lu: %s", result.exitCode, Trim(err).c_str());
            return false;
        }
        out = std::move(result.out);
        return true;
    }

    bool GitHubProvider::QueryAll(PullRequestsSnapshot& snap)
    {
        std::string out;
        if (!RunGh(std::wstring(L"api graphql -f \"query=") + kGraphQl + L"\"", out)) return false;
        wdj::JsonObject root{ nullptr };
        if (!wdj::JsonObject::TryParse(Utf8ToWide(out), root)) return false;
        auto data = Obj(root, L"data");
        if (!data) return false;
        if (auto nodes = Arr(Obj(Obj(data, L"viewer"), L"pullRequests"), L"nodes"))
        {
            for (auto const& v : nodes)
            {
                if (v.ValueType() != wdj::JsonValueType::Object) continue;
                auto n = v.GetObject();
                auto pr = ParsePr(n, L"");
                if (auto commits = Arr(Obj(n, L"commits"), L"nodes"); commits && commits.Size() > 0 && commits.GetAt(0).ValueType() == wdj::JsonValueType::Object)
                {
                    auto rollup = Obj(Obj(commits.GetAt(0).GetObject(), L"commit"), L"statusCheckRollup");
                    pr.checks = RollupState(Str(rollup, L"state"));
                }
                snap.mine.push_back(std::move(pr));
            }
        }
        if (auto nodes = Arr(Obj(data, L"search"), L"nodes"))
        {
            for (auto const& v : nodes)
            {
                if (v.ValueType() != wdj::JsonValueType::Object) continue;
                auto pr = ParsePr(v.GetObject(), L"");
                if (pr.number) snap.reviews.push_back(std::move(pr));
            }
        }
        return true;
    }

    bool GitHubProvider::QueryRepo(std::wstring const& repo, PullRequestsSnapshot& snap)
    {
        std::string out;
        if (!RunGh(L"pr list --repo " + repo + L" --author @me --state open --limit 20 --json number,title,url,isDraft,updatedAt,statusCheckRollup", out)) return false;
        wdj::JsonArray mine{ nullptr };
        if (!wdj::JsonArray::TryParse(Utf8ToWide(out), mine)) return false;
        for (auto const& v : mine)
        {
            if (v.ValueType() != wdj::JsonValueType::Object) continue;
            auto n = v.GetObject();
            auto pr = ParsePr(n, repo);
            pr.checks = FromChecks(Arr(n, L"statusCheckRollup"));
            snap.mine.push_back(std::move(pr));
        }
        out.clear();
        if (!RunGh(L"pr list --repo " + repo + L" --search \"review-requested:@me\" --state open --limit 20 --json number,title,url,updatedAt,author", out)) return false;
        wdj::JsonArray reviews{ nullptr };
        if (!wdj::JsonArray::TryParse(Utf8ToWide(out), reviews)) return false;
        for (auto const& v : reviews)
        {
            if (v.ValueType() == wdj::JsonValueType::Object) snap.reviews.push_back(ParsePr(v.GetObject(), repo));
        }
        return true;
    }

    bool GitHubProvider::PollReal()
    {
        PullRequestsSnapshot snap;
        bool ok = true;
        if (m_repos.empty())
        {
            ok = QueryAll(snap);
        }
        else
        {
            for (auto const& repo : m_repos)
            {
                if (Stopping()) return true;
                // Repo names go on a command line: only allow owner/name characters.
                if (repo.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./") != std::wstring::npos) continue;
                ok = QueryRepo(repo, snap) && ok;
            }
        }
        if (!ok && snap.mine.empty() && snap.reviews.empty()) return false;
        auto byUpdated = [](PullRequest const& a, PullRequest const& b) { return a.updated > b.updated; };
        std::sort(snap.mine.begin(), snap.mine.end(), byUpdated);
        std::sort(snap.reviews.begin(), snap.reviews.end(), byUpdated);
        int failing = 0;
        for (auto const& pr : snap.mine) failing += pr.checks == CheckState::Failure;
        Log(L"[github] %zu open PR(s) of mine (%d failing checks), %zu review request(s)", snap.mine.size(), failing, snap.reviews.size());
        Publish(std::move(snap));
        return ok;
    }

    void GitHubProvider::PollMock()
    {
        auto now = Clock::now();
        PullRequestsSnapshot snap;
        snap.mock = true;
        int ci = Mocks().ci;
        auto add = [&](std::vector<PullRequest>& list, const wchar_t* repo, int number, const wchar_t* title, const wchar_t* author, CheckState checks, int minutesAgo, bool draft = false)
        {
            PullRequest pr;
            pr.repo = repo;
            pr.number = number;
            pr.title = title;
            pr.author = author;
            pr.checks = checks;
            pr.draft = draft;
            pr.updated = now - minutes(minutesAgo);
            pr.url = L"https://github.com/" + std::wstring(repo) + L"/pull/" + std::to_wstring(number);
            list.push_back(std::move(pr));
        };
        add(snap.mine, L"contoso/storefront", 1287, L"Fix flaky checkout integration tests", L"me", ci == 1 ? CheckState::Failure : CheckState::Success, 6);
        add(snap.mine, L"contoso/portal", 342, L"Dark mode for settings page", L"me", ci == 0 ? CheckState::Pending : CheckState::Success, 18);
        add(snap.mine, L"contoso/pipeline", 77, L"Bound queue worker memory with backpressure", L"me", CheckState::Success, 95);
        add(snap.mine, L"contoso/docs", 410, L"Release notes for v2.4", L"me", CheckState::Success, 240, true);
        add(snap.reviews, L"contoso/portal", 351, L"Migrate settings store to IndexedDB", L"adele", CheckState::Success, 12);
        add(snap.reviews, L"contoso/storefront", 1290, L"Add Apple Pay to checkout", L"miguel", CheckState::Pending, 41);
        add(snap.reviews, L"contoso/infra", 58, L"Terraform: move CDN to Front Door", L"priya", CheckState::Success, 130);
        Publish(std::move(snap));
    }
}
