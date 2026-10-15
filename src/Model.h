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
        // Best-effort guess that this session is a sub-agent spawned by another session in the
        // same repo (there is no explicit parent/child field in workspace.yaml or events.jsonl,
        // so this is inferred from directory creation order - see SessionsProvider::LinkChildren).
        std::wstring parentId;
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
}
