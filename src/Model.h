#pragma once
#include "pch.h"
#include "Util.h"

// Immutable snapshots that providers publish to the UI thread.
namespace ld
{
    enum class AgentStatus { Working, Waiting, Done, Idle };
    const wchar_t* AgentStatusName(AgentStatus status);

    struct AgentSession
    {
        std::wstring id;
        std::wstring title;     // user-given name, or the first line of the first prompt
        std::wstring repo;      // "owner/repo" or folder name
        std::wstring branch;
        std::wstring detail;    // e.g. the pending ask_user question or the running tool
        AgentStatus status = AgentStatus::Idle;
        TimePoint since{};      // when the session entered `status`
        TimePoint lastActivity{};
        bool live = false;      // a process currently holds the session (inuse.<pid>.lock)
    };

    struct SessionsSnapshot
    {
        std::vector<AgentSession> sessions; // sorted: waiting, working, done, idle; then recency
        bool mock = false;
        int generation = 0;                 // bumps when the data source changes (mock toggle)
    };

    enum class CheckState { None, Pending, Success, Failure };

    struct PullRequest
    {
        std::wstring repo;
        std::wstring title;
        std::wstring url;
        std::wstring author;
        int number = 0;
        CheckState checks = CheckState::None;
        bool draft = false;
        TimePoint updated{};
    };

    struct PullRequestsSnapshot
    {
        std::vector<PullRequest> mine;
        std::vector<PullRequest> reviews;
        bool mock = false;
        bool AnyFailing() const;
        bool AnyPending() const;
    };

    struct SystemSnapshot
    {
        float cpu = 0;   // 0..1
        float memory = 0;
        float gpu = 0;
        bool gpuAvailable = false;
        bool mock = false;
    };

    struct Meeting
    {
        std::wstring title;
        std::wstring location;
        TimePoint start{};
        TimePoint end{};
        bool allDay = false;
    };

    struct CalendarSnapshot
    {
        std::vector<Meeting> meetings; // today's, sorted by start
        std::wstring source;           // "Windows Calendar", "ICS", "mock"
        bool mock = false;
        std::optional<Meeting> NextMeeting(TimePoint now) const;
    };

    struct ListeningPort
    {
        uint16_t port = 0;
        std::wstring process;
        DWORD pid = 0;
        std::wstring address; // "localhost", "0.0.0.0", "[::]"
    };

    struct PortsSnapshot
    {
        std::vector<ListeningPort> ports;
        bool mock = false;
    };

    struct LogLine
    {
        std::wstring text;
        bool error = false;
    };

    struct LogTailSnapshot
    {
        std::wstring path;
        std::vector<LogLine> lines;
        uint64_t errorSerial = 0; // increments whenever a new error line shows up
        bool mock = false;
    };

    struct VirtualDesktopInfo
    {
        std::wstring id;   // "{GUID}"
        std::wstring name; // user name, or "Desktop N"
        int index = 0;     // 1-based
    };

    struct WorkingTree
    {
        std::wstring path;     // git root
        std::wstring name;     // folder name
        std::wstring branch;   // "(detached)" when HEAD is detached
        std::wstring upstream; // empty when the branch has no upstream
        int ahead = 0, behind = 0;
        int staged = 0, modified = 0, untracked = 0, conflicts = 0;
        TimePoint lastActivity{};
        bool fromSession = false; // discovered from a recent Copilot session
        int Dirty() const { return staged + modified + untracked + conflicts; }
    };

    struct WorktreesSnapshot
    {
        std::vector<WorkingTree> trees; // most recently active first
        bool mock = false;
    };
}
