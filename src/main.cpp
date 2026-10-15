// LiveDesktop - an interactive Meadow wallpaper living behind the desktop icons.
#include "pch.h"
#include "Log.h"
#include "DesktopHost.h"
#include "DesktopIcons.h"
#include "Scene.h"
#include "Config.h"
#include "Providers.h"
#include "Graphics.h"
#include "Reveal.h"
#include "Util.h"
#include "Snapshot.h"
#include <map>

namespace ld { HICON CreateAppIcon(int size); }

using namespace ld;

namespace
{
    constexpr wchar_t kControllerClass[] = L"LiveDesktop.Controller";
    constexpr wchar_t kMutexName[] = L"Local\\LiveDesktop.SingleInstance";

    constexpr UINT WM_APP_TRAY = WM_APP + 1;
    constexpr UINT WM_APP_POINTER = WM_APP + 2;
    constexpr UINT WM_APP_CLICK = WM_APP + 3;
    constexpr UINT WM_APP_HOST_LOST = WM_APP + 4;
    constexpr UINT WM_APP_RELEASE = WM_APP + 5;
    constexpr UINT WM_APP_REVEAL = WM_APP + 6;

    constexpr UINT_PTR TIMER_HEALTH = 1;
    constexpr UINT_PTR TIMER_REATTACH = 2;
    constexpr UINT_PTR TIMER_RELAYOUT = 3;
    constexpr UINT_PTR TIMER_AUTOEXIT = 4;
    constexpr UINT_PTR TIMER_REVEAL = 5;
    constexpr UINT_PTR TIMER_TICK = 6;

    constexpr UINT IDM_PAUSE = 101;
    constexpr UINT IDM_EXIT = 102;
    constexpr UINT IDM_MOCK = 103;
    constexpr UINT kTrayId = 1;
    constexpr ULONG_PTR kCopyDataCommand = 0x4C44; // 'LD'

    struct Options
    {
        bool layered = false;
        bool refreshWallpaper = true;
        UINT exitAfterSeconds = 0;
        bool mock = false;
    };

    struct Providers
    {
        std::unique_ptr<SessionsProvider> sessions;

        template <class F> void ForEach(F&& f)
        {
            Provider* all[] = { sessions.get() };
            for (auto* p : all) if (p) f(*p);
        }
    };

    // Left-button press being tracked for a click on a Meadow animal (agent).
    struct PendingClick
    {
        bool active = false;
        POINT screen{};
        ULONGLONG tick = 0;
        std::optional<std::wstring> agent;
    };

    struct App
    {
        HINSTANCE instance = nullptr;
        HWND controller = nullptr;
        UINT taskbarCreated = 0;
        Options options;
        DesktopHost host;
        DesktopIcons icons;
        Scene scene;
        HICON icon = nullptr;
        bool trayAdded = false;
        bool exiting = false;
        bool userPaused = false;
        bool forceRender = false;          // --send render:on: keep animating while covered (offscreen snapshots)
        bool fullscreenPaused = false;
        int reattachAttempts = 0;
        std::wstring displaySignature;
        HANDLE hookThread = nullptr;
        DWORD hookThreadId = 0;

        Config config;
        Graphics graphics;
        RevealMonitor reveal;
        Providers providers;
        bool mock = false;
        bool occluded = false;
        PendingClick click;
        bool buttonDown = false;
        POINT downAt{};
        // Last sessions snapshot from the provider, plus statuses forced via --send agent:<n>:<state>.
        std::optional<SessionsSnapshot> sessions;
        std::map<std::wstring, std::pair<AgentStatus, TimePoint>> agentOverrides;
        winrt::Windows::System::DispatcherQueueTimer agentOpenTimer{ nullptr };
    };

    App g_app;
    HANDLE g_cleanupDone = nullptr;

    // State shared with the low-level mouse hook thread.
    std::atomic<HWND> g_hookTarget{ nullptr };
    std::atomic<LONG> g_pointerX{ 0 }, g_pointerY{ 0 };
    std::atomic<bool> g_pointerPending{ false };

    // ---------------------------------------------------------------------------------------------
    // Low-level mouse hook, on its own thread so nothing the UI thread does (COM calls into Explorer,
    // composition work) can ever delay system-wide mouse input. It only observes; it never blocks.
    // ---------------------------------------------------------------------------------------------
    LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION)
        {
            auto const* info = reinterpret_cast<MSLLHOOKSTRUCT const*>(lParam);
            HWND target = g_hookTarget.load(std::memory_order_relaxed);
            if (target)
            {
                if (wParam == WM_MOUSEMOVE)
                {
                    g_pointerX.store(info->pt.x, std::memory_order_relaxed);
                    g_pointerY.store(info->pt.y, std::memory_order_relaxed);
                    // Coalesce: at most one pending pointer message in the UI queue.
                    if (!g_pointerPending.exchange(true)) PostMessageW(target, WM_APP_POINTER, 0, 0);
                }
                else if (wParam == WM_LBUTTONDOWN)
                {
                    PostMessageW(target, WM_APP_CLICK, static_cast<WPARAM>(info->pt.x), static_cast<LPARAM>(info->pt.y));
                }
                else if (wParam == WM_LBUTTONUP)
                {
                    PostMessageW(target, WM_APP_RELEASE, static_cast<WPARAM>(info->pt.x), static_cast<LPARAM>(info->pt.y));
                }
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    DWORD WINAPI HookThreadProc(void*)
    {
        HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
        if (!hook)
        {
            Log(L"SetWindowsHookEx(WH_MOUSE_LL) failed: %lu - interactivity disabled", GetLastError());
            return 1;
        }
        Log(L"Low-level mouse hook installed (thread %lu)", GetCurrentThreadId());
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            DispatchMessageW(&msg);
        }
        UnhookWindowsHookEx(hook);
        return 0;
    }

    void StartHookThread()
    {
        g_app.hookThread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, &g_app.hookThreadId);
        if (g_app.hookThread) SetThreadPriority(g_app.hookThread, THREAD_PRIORITY_ABOVE_NORMAL);
    }

    void StopHookThread()
    {
        g_hookTarget = nullptr;
        if (!g_app.hookThread) return;
        PostThreadMessageW(g_app.hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_app.hookThread, 2000);
        CloseHandle(g_app.hookThread);
        g_app.hookThread = nullptr;
        Log(L"Low-level mouse hook removed");
    }

    // ---------------------------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------------------------
    std::wstring ClassOf(HWND hwnd)
    {
        wchar_t name[128]{};
        if (hwnd) GetClassNameW(hwnd, name, ARRAYSIZE(name));
        return name;
    }

    // True when the window under the cursor belongs to the desktop (icons / empty desktop).
    bool IsDesktopAt(POINT pt)
    {
        HWND hit = WindowFromPoint(pt);
        if (!hit) return false;
        HWND root = GetAncestor(hit, GA_ROOT);
        auto cls = ClassOf(root);
        return cls == L"Progman" || cls == L"WorkerW";
    }

    // `reason` describes what triggered the detection (for the log).
    bool IsFullscreenAppActive(std::wstring& reason)
    {
        QUERY_USER_NOTIFICATION_STATE state{};
        if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
            (state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE))
        {
            reason = L"notification state " + std::to_wstring(static_cast<int>(state));
            return true;
        }

        HWND fg = GetForegroundWindow();
        if (!fg || !IsWindowVisible(fg) || IsIconic(fg)) return false;
        auto cls = ClassOf(GetAncestor(fg, GA_ROOT));
        if (cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd") return false;

        RECT wr{};
        if (FAILED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr)))) GetWindowRect(fg, &wr);
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
        bool covers = wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
                      wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
        if (covers) reason = L"foreground window '" + cls + L"' covers its monitor";
        return covers;
    }

    // Paused = nothing animates. "Heavy" pause (fullscreen app, or the desktop fully occluded) also
    // throttles the providers.
    void UpdatePaused()
    {
        bool heavy = !g_app.forceRender && (g_app.fullscreenPaused || g_app.occluded);
        bool paused = g_app.userPaused || heavy;
        if (paused != g_app.scene.Paused())
        {
            Log(L"Rendering %s (user=%d fullscreen=%d occluded=%d)", paused ? L"paused" : L"resumed",
                g_app.userPaused, g_app.fullscreenPaused, g_app.occluded);
        }
        g_app.scene.SetPaused(paused);

        static int lastThrottle = -1;
        int throttle = heavy ? 4 : 1;
        if (throttle != lastThrottle)
        {
            lastThrottle = throttle;
            g_app.providers.ForEach([&](Provider& p) { p.SetThrottle(throttle); });
        }
    }

    // Re-applies the user's current wallpaper so the desktop repaints it (needed on the classic layout,
    // where the WorkerW can keep showing our last frame). No SPIF_UPDATEINIFILE: nothing is persisted.
    void RestoreWallpaper()
    {
        if (!g_app.options.refreshWallpaper)
        {
            Log(L"Wallpaper refresh skipped (--no-wallpaper-refresh)");
            return;
        }
        wchar_t path[MAX_PATH * 2]{};
        if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(path), path, 0))
        {
            Log(L"SPI_GETDESKWALLPAPER failed: %lu", GetLastError());
            return;
        }
        if (!path[0])
        {
            Log(L"No wallpaper image configured (solid color/slideshow); desktop repaint only");
            return;
        }
        BOOL ok = SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, path, SPIF_SENDCHANGE);
        Log(L"Wallpaper re-applied (%s): %s", ok ? L"ok" : L"failed", path);
    }

    // ---------------------------------------------------------------------------------------------
    // Tray icon
    // ---------------------------------------------------------------------------------------------
    void UpdateTrayTip()
    {
        if (!g_app.trayAdded) return;
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = g_app.controller;
        nid.uID = kTrayId;
        nid.uFlags = NIF_TIP | NIF_SHOWTIP;
        swprintf_s(nid.szTip, L"LiveDesktop - %s%s%s", g_app.scene.EffectName(),
            g_app.mock ? L", mock" : L"", g_app.userPaused ? L" (paused)" : L"");
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void AddTrayIcon()
    {
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = g_app.controller;
        nid.uID = kTrayId;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = WM_APP_TRAY;
        nid.hIcon = g_app.icon;
        swprintf_s(nid.szTip, L"LiveDesktop - %s", g_app.scene.EffectName());
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_app.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        if (g_app.trayAdded)
        {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
        }
        Log(L"Tray icon %s", g_app.trayAdded ? L"added" : L"could not be added (taskbar not ready)");
    }

    void RemoveTrayIcon()
    {
        if (!g_app.trayAdded) return;
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = g_app.controller;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_app.trayAdded = false;
    }

    void ShowTrayMenu(POINT pt)
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (g_app.mock ? MF_CHECKED : 0), IDM_MOCK, L"Mock data");
        AppendMenuW(menu, MF_STRING | (g_app.userPaused ? MF_CHECKED : 0), IDM_PAUSE, L"Pause animations");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
        SetForegroundWindow(g_app.controller);
        TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, g_app.controller, nullptr);
        PostMessageW(g_app.controller, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }

    // ---------------------------------------------------------------------------------------------
    // Attach / re-attach
    // ---------------------------------------------------------------------------------------------
    float2 HostSize()
    {
        SIZE s = g_app.host.Size();
        return float2(static_cast<float>(s.cx), static_cast<float>(s.cy));
    }

    float2 ToLocal(POINT screen)
    {
        POINT p = g_app.host.ScreenToHost(screen);
        return float2(static_cast<float>(p.x), static_cast<float>(p.y));
    }

    HMONITOR PrimaryMonitor(MONITORINFO& info)
    {
        HMONITOR monitor = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        info = MONITORINFO{ sizeof(info) };
        GetMonitorInfoW(monitor, &info);
        return monitor;
    }

    std::vector<RECT> WorkAreas()
    {
        std::vector<RECT> areas;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM param) -> BOOL {
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(m, &mi)) reinterpret_cast<std::vector<RECT>*>(param)->push_back(mi.rcWork);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&areas));
        return areas;
    }

    // Recomputes the primary monitor's DPI scale and pushes it to the graphics layer (text/label sizing).
    void ApplyDpiScale()
    {
        MONITORINFO mi;
        HMONITOR primary = PrimaryMonitor(mi);
        UINT dpiX = 96, dpiY = 96;
        if (SUCCEEDED(GetDpiForMonitor(primary, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        {
            float scale = dpiX / 96.0f;
            if (scale != g_app.graphics.Scale())
            {
                g_app.graphics.SetScale(scale);
                Log(L"DPI scale: %.2f", scale);
            }
        }
    }

    void EvaluateReveal()
    {
        MONITORINFO mi;
        PrimaryMonitor(mi);
        auto state = g_app.reveal.Evaluate(mi.rcWork, WorkAreas());
        if (state.occluded != g_app.occluded)
        {
            Log(L"Desktop %s: %s", state.occluded ? L"fully occluded" : L"visible", state.reason.c_str());
        }
        g_app.occluded = state.occluded;
        UpdatePaused();
    }

    void SetMock(bool mock)
    {
        if (g_app.mock == mock) return;
        g_app.mock = mock;
        Log(L"Mock data %s", mock ? L"on" : L"off");
        g_app.providers.ForEach([&](Provider& p) { p.SetMock(mock); });
        UpdateTrayTip();
    }

    void Detach()
    {
        g_app.scene.DetachFromWindow();
        g_app.host.Detach();
        g_app.icons.Reset();
    }

    bool TryAttach()
    {
        if (!g_app.host.Attach(g_app.instance, g_app.controller, WM_APP_HOST_LOST, g_app.options.layered))
        {
            return false;
        }
        try
        {
            g_app.scene.AttachToWindow(g_app.host.Hwnd(), HostSize(), g_app.host.MonitorRectsInHost());
        }
        catch (winrt::hresult_error const& e)
        {
            Log(L"CreateDesktopWindowTarget/scene setup failed: 0x%08X %s", e.code().value, e.message().c_str());
            Detach();
            return false;
        }
        g_app.displaySignature = DesktopHost::DisplaySignature();
        g_app.icons.Reset();
        ApplyDpiScale();
        Log(L"Attached. Effect '%s' rendering into HWND 0x%p (parent 0x%p)",
            g_app.scene.EffectName(), g_app.host.Hwnd(), g_app.host.Windows().parent);
        return true;
    }

    void ScheduleReattach(const wchar_t* reason)
    {
        if (g_app.exiting) return;
        Log(L"Re-attach scheduled: %s", reason);
        Detach();
        g_app.reattachAttempts = 0;
        SetTimer(g_app.controller, TIMER_REATTACH, 1000, nullptr);
    }

    void Relayout()
    {
        if (!g_app.host.IsAttached()) return;
        auto signature = DesktopHost::DisplaySignature();
        if (signature == g_app.displaySignature) return;
        Log(L"Display topology/DPI changed -> relayout (%s)", signature.c_str());
        g_app.displaySignature = signature;
        g_app.host.UpdateBounds();
        g_app.scene.Resize(HostSize(), g_app.host.MonitorRectsInHost());
        ApplyDpiScale();
    }

    void BeginExit(const wchar_t* reason)
    {
        if (g_app.exiting) return;
        g_app.exiting = true;
        Log(L"Exiting (%s)", reason);
        KillTimer(g_app.controller, TIMER_HEALTH);
        KillTimer(g_app.controller, TIMER_REATTACH);
        KillTimer(g_app.controller, TIMER_RELAYOUT);
        KillTimer(g_app.controller, TIMER_AUTOEXIT);
        KillTimer(g_app.controller, TIMER_REVEAL);
        KillTimer(g_app.controller, TIMER_TICK);
        g_app.reveal.Stop();
        StopHookThread();
        g_app.providers.ForEach([](Provider& p) { p.Stop(); });
        Detach();
        RemoveTrayIcon();
        RestoreWallpaper();
        DestroyWindow(g_app.controller);
    }

    // Sessions from the provider with any --send agent:<n>:<state> overrides applied, to the scene.
    void ForwardSessions()
    {
        if (!g_app.sessions) return;
        SessionsSnapshot s = *g_app.sessions;
        if (!g_app.agentOverrides.empty())
        {
            for (auto& session : s.sessions)
                if (auto it = g_app.agentOverrides.find(session.id); it != g_app.agentOverrides.end())
                {
                    if (session.status != it->second.first)
                    {
                        static constexpr wchar_t const* kDetail[] = { L"Working\u2026", L"Needs your input", L"Task complete", L"" };
                        auto st = it->second.first;
                        session.detail = kDetail[st == AgentStatus::Working ? 0 : st == AgentStatus::Waiting ? 1 : st == AgentStatus::Done ? 2 : 3];
                    }
                    session.status = it->second.first;
                    session.since = it->second.second;
                    session.lastActivity = std::max(session.lastActivity, it->second.second);
                    if (session.status == AgentStatus::Waiting && session.detail.empty()) session.detail = L"Needs your input";
                }
            auto rank = [](AgentStatus st) { return st == AgentStatus::Waiting ? 0 : st == AgentStatus::Working ? 1 : st == AgentStatus::Done ? 2 : 3; };
            std::stable_sort(s.sessions.begin(), s.sessions.end(), [&](AgentSession const& a, AgentSession const& b) {
                if (rank(a.status) != rank(b.status)) return rank(a.status) < rank(b.status);
                return a.lastActivity > b.lastActivity;
            });
        }
        g_app.scene.UpdateSessions(s);
    }

    // agent:<n|id-prefix>:<working|waiting|done|idle|auto>, agent:list, agent:auto
    void HandleAgentCommand(std::wstring const& value)
    {
        if (!g_app.sessions) { Log(L"agent: no sessions yet"); return; }
        std::vector<AgentSession const*> list;
        for (auto const& s : g_app.sessions->sessions) list.push_back(&s);
        std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return a->id < b->id; });
        if (value == L"list")
        {
            for (size_t i = 0; i < list.size(); ++i)
                Log(L"  agent %zu: %s %s '%s'", i + 1, list[i]->id.c_str(), AgentStatusName(list[i]->status), list[i]->title.c_str());
            return;
        }
        if (value == L"auto") { g_app.agentOverrides.clear(); ForwardSessions(); return; }
        size_t colon = value.find(L':');
        if (colon == std::wstring::npos) { Log(L"agent: expected agent:<n>:<state>"); return; }
        std::wstring who = value.substr(0, colon), state = value.substr(colon + 1);
        AgentSession const* target = nullptr;
        if (!who.empty() && std::all_of(who.begin(), who.end(), iswdigit))
        {
            size_t n = static_cast<size_t>(_wtoi(who.c_str()));
            if (n >= 1 && n <= list.size()) target = list[n - 1];
        }
        else
            for (auto* s : list)
                if (ToLower(s->id).rfind(who, 0) == 0) { target = s; break; }
        if (!target) { Log(L"agent: no session '%s' (%zu known)", who.c_str(), list.size()); return; }
        if (state == L"auto") g_app.agentOverrides.erase(target->id);
        else
        {
            AgentStatus st;
            if (state == L"working") st = AgentStatus::Working;
            else if (state == L"waiting") st = AgentStatus::Waiting;
            else if (state == L"done") st = AgentStatus::Done;
            else if (state == L"idle") st = AgentStatus::Idle;
            else { Log(L"agent: unknown state '%s'", state.c_str()); return; }
            g_app.agentOverrides[target->id] = { st, Clock::now() };
        }
        Log(L"agent: '%s' forced %s", target->title.c_str(), state.c_str());
        ForwardSessions();
    }

    // Commands from `LiveDesktop.exe --send <cmd>` (WM_COPYDATA). Used for scripted demos/screenshots.
    void HandleCommand(std::wstring const& raw)
    {
        std::wstring trimmed = Trim(raw);
        if (ToLower(trimmed.substr(0, 9)) == L"snapshot:")
        {
            // Case-preserving path. Captures the composition tree offscreen (works while locked/covered).
            std::wstring path = trimmed.substr(9);
            Log(L"Command: snapshot %s", path.c_str());
            if (auto root = g_app.scene.Root())
                SnapshotVisual(root, path, [path](bool ok) { Log(L"Snapshot %s: %s", ok ? L"saved" : L"FAILED", path.c_str()); });
            return;
        }
        std::wstring cmd = ToLower(trimmed);
        Log(L"Command: %s", cmd.c_str());
        auto arg = [&](std::wstring const& prefix, std::wstring& value) {
            if (cmd.rfind(prefix, 0) != 0) return false;
            value = cmd.substr(prefix.size());
            return true;
        };
        std::wstring value;
        if (arg(L"time:", value))
        {
            float hour = MeadowPhaseHour(value);
            if (hour < -1.5f) Log(L"Unknown time '%s'", value.c_str());
            else g_app.scene.SetTimeOverride(hour);
        }
        else if (arg(L"mock:", value)) SetMock(value == L"on");
        else if (arg(L"agent:", value)) HandleAgentCommand(value);
        else if (arg(L"render:", value)) { g_app.forceRender = value == L"on"; UpdatePaused(); }
        else if (cmd == L"pause") { g_app.userPaused = !g_app.userPaused; UpdatePaused(); UpdateTrayTip(); }
        else if (cmd == L"stop" || cmd == L"exit") BeginExit(L"--send stop");
        else if (cmd.rfind(L"hit:", 0) == 0)
        {
            // Diagnostic: report what a click at screen x,y would do, without clicking.
            POINT pt{};
            if (swscanf_s(cmd.c_str() + 4, L"%ld,%ld", &pt.x, &pt.y) != 2) return;
            ReplyMessage(TRUE); // release the sender so the icon COM call isn't an input-synchronous callout
            float2 local = ToLocal(pt);
            auto agent = g_app.scene.HitAgent(local);
            Log(L"hit %ld,%ld -> local %.0f,%.0f desktop=%d icon=%d agent=%s", pt.x, pt.y, local.x, local.y,
                IsDesktopAt(pt) ? 1 : 0, g_app.icons.HitTestIcon(pt) ? 1 : 0, agent ? agent->c_str() : L"-");
        }
        else if (cmd == L"agents?")
        {
            // Diagnostic: scan the host and log where each animal can be clicked (screen coordinates).
            ReplyMessage(TRUE);
            RECT rc{};
            GetClientRect(g_app.host.Hwnd(), &rc);
            POINT origin{};
            MapWindowPoints(g_app.host.Hwnd(), HWND_DESKTOP, &origin, 1);
            std::map<std::wstring, std::pair<float2, int>> found;
            for (LONG y = 0; y < rc.bottom; y += 12)
                for (LONG x = 0; x < rc.right; x += 12)
                {
                    POINT pt{ origin.x + x, origin.y + y };
                    if (auto agent = g_app.scene.HitAgent(ToLocal(pt)))
                    {
                        auto& [sum, n] = found[*agent];
                        sum += float2(static_cast<float>(pt.x), static_cast<float>(pt.y));
                        ++n;
                    }
                }
            Log(L"agents? %zu clickable animal(s)", found.size());
            for (auto const& [id, acc] : found)
            {
                POINT c{ static_cast<LONG>(acc.first.x / acc.second), static_cast<LONG>(acc.first.y / acc.second) };
                bool hits = g_app.scene.HitAgent(ToLocal(c)) == id;
                Log(L"  agent %s at %ld,%ld (area %d cells, centre hits=%d, icon=%d)", id.c_str(), c.x, c.y, acc.second, hits ? 1 : 0,
                    g_app.icons.HitTestIcon(c) ? 1 : 0);
            }
        }
        else Log(L"Unknown command '%s'", cmd.c_str());
    }

    // ---------------------------------------------------------------------------------------------
    // Controller window (hidden top-level: receives TaskbarCreated, WM_DISPLAYCHANGE, tray, timers)
    // ---------------------------------------------------------------------------------------------
    LRESULT CALLBACK ControllerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == g_app.taskbarCreated && g_app.taskbarCreated != 0)
        {
            Log(L"TaskbarCreated received (Explorer restarted)");
            AddTrayIcon();
            ScheduleReattach(L"Explorer restarted");
            return 0;
        }

        switch (msg)
        {
        case WM_APP_POINTER:
        {
            g_pointerPending = false;
            if (!g_app.host.IsAttached()) return 0;
            POINT pt{ g_pointerX.load(), g_pointerY.load() };
            bool over = !g_app.fullscreenPaused && IsDesktopAt(pt);
            float2 local = ToLocal(pt);
            g_app.scene.PointerMoved(local, over);
            if (g_app.scene.HostsAgents())
            {
                bool free = over && !g_app.icons.HitTestIcon(pt);
                g_app.scene.HoverAgent(free ? std::optional<float2>(local) : std::nullopt);
            }
            return 0;
        }
        case WM_APP_CLICK:
        {
            g_app.click.active = false;
            POINT pt{ static_cast<LONG>(wParam), static_cast<LONG>(lParam) };
            g_app.buttonDown = true;
            g_app.downAt = pt;
            if (!g_app.host.IsAttached() || g_app.fullscreenPaused) return 0;
            if (!IsDesktopAt(pt) || g_app.icons.HitTestIcon(pt)) return 0;
            float2 local = ToLocal(pt);
            if (auto agent = g_app.scene.HitAgent(local))
            {
                // Animals open their session on button-up, like a normal click.
                Log(L"Click down %ld,%ld -> agent %s", pt.x, pt.y, agent->c_str());
                g_app.click = PendingClick{ true, pt, GetTickCount64(), agent };
                return 0;
            }
            g_app.scene.Click(local);
            return 0;
        }
        case WM_APP_RELEASE:
        {
            g_app.buttonDown = false;
            if (!g_app.click.active) return 0;
            g_app.click.active = false;
            POINT pt{ static_cast<LONG>(wParam), static_cast<LONG>(lParam) };
            if (std::abs(pt.x - g_app.click.screen.x) > 6 || std::abs(pt.y - g_app.click.screen.y) > 6 ||
                GetTickCount64() - g_app.click.tick > 600)
            {
                Log(L"Click up %ld,%ld ignored: moved or held (%llu ms)", pt.x, pt.y, GetTickCount64() - g_app.click.tick);
                return 0;
            }
            if (!IsDesktopAt(pt) || g_app.icons.HitTestIcon(pt))
            {
                Log(L"Click up %ld,%ld ignored: not desktop or on an icon", pt.x, pt.y);
                return 0;
            }
            float2 local = ToLocal(pt);
            if (g_app.click.agent)
            {
                if (g_app.scene.HitAgent(local) != g_app.click.agent)
                {
                    Log(L"Click up %ld,%ld ignored: animal moved away", pt.x, pt.y);
                    return 0;
                }
                std::wstring id = *g_app.click.agent;
                int delay = g_app.scene.ActivateAgent(id);
                std::wstring url = L"ghapp://sessions/" + id;
                Log(L"Meadow: open agent %s in %d ms", url.c_str(), delay);
                auto timer = g_app.scene.Queue().CreateTimer();
                timer.Interval(std::chrono::milliseconds(std::max(delay, 1)));
                timer.IsRepeating(false);
                timer.Tick([url](winrt::Windows::System::DispatcherQueueTimer const& sender, auto&&) {
                    sender.Stop();
                    OpenUrl(url);
                });
                timer.Start();
                g_app.agentOpenTimer = timer;
                return 0;
            }
            return 0;
        }
        case WM_APP_REVEAL:
            g_app.reveal.Acknowledge();
            SetTimer(hwnd, TIMER_REVEAL, 120, nullptr);
            return 0;
        case WM_COPYDATA:
        {
            auto const* cds = reinterpret_cast<COPYDATASTRUCT const*>(lParam);
            if (!cds || cds->dwData != kCopyDataCommand || !cds->lpData || cds->cbData > 4096) return FALSE;
            HandleCommand(std::wstring(static_cast<wchar_t const*>(cds->lpData), cds->cbData / sizeof(wchar_t)));
            return TRUE;
        }
        case WM_APP_HOST_LOST:
            ScheduleReattach(L"host window destroyed externally");
            return 0;

        case WM_APP_TRAY:
            switch (LOWORD(lParam))
            {
            case WM_CONTEXTMENU:
            case WM_RBUTTONUP:
                ShowTrayMenu(POINT{ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) });
                break;
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDM_PAUSE:
                g_app.userPaused = !g_app.userPaused;
                Log(L"User %s animations", g_app.userPaused ? L"paused" : L"resumed");
                UpdatePaused();
                UpdateTrayTip();
                break;
            case IDM_EXIT:
                BeginExit(L"tray menu");
                break;
            case IDM_MOCK:
                SetMock(!g_app.mock);
                break;
            }
            return 0;

        case WM_TIMER:
            switch (wParam)
            {
            case TIMER_HEALTH:
            {
                if (g_app.host.IsAttached())
                {
                    if (!g_app.host.IsHealthy())
                    {
                        ScheduleReattach(L"health check failed (desktop windows changed)");
                    }
                    else
                    {
                        g_app.host.EnsureZOrder();
                        Relayout();
                    }
                }
                std::wstring reason;
                bool fullscreen = IsFullscreenAppActive(reason);
                if (fullscreen != g_app.fullscreenPaused)
                {
                    g_app.fullscreenPaused = fullscreen;
                    Log(L"Fullscreen app %s -> animations %s%s%s", fullscreen ? L"detected" : L"gone",
                        fullscreen ? L"paused" : L"resumed", reason.empty() ? L"" : L": ", reason.c_str());
                    UpdatePaused();
                }
                EvaluateReveal(); // safety net for missed WinEvents
                return 0;
            }
            case TIMER_REVEAL:
                KillTimer(hwnd, TIMER_REVEAL);
                EvaluateReveal();
                return 0;
            case TIMER_TICK:
                if (!g_app.scene.Paused())
                {
                    g_app.scene.Tick();
                }
                return 0;
            case TIMER_REATTACH:
                ++g_app.reattachAttempts;
                if (TryAttach())
                {
                    KillTimer(hwnd, TIMER_REATTACH);
                }
                else if (g_app.reattachAttempts % 10 == 0)
                {
                    Log(L"Still waiting for the desktop (attempt %d)", g_app.reattachAttempts);
                }
                return 0;
            case TIMER_RELAYOUT:
                KillTimer(hwnd, TIMER_RELAYOUT);
                Relayout();
                return 0;
            case TIMER_AUTOEXIT:
                BeginExit(L"--exit-after elapsed");
                return 0;
            }
            break;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
        case WM_SETTINGCHANGE:
            // Debounce: topology changes arrive as bursts.
            SetTimer(hwnd, TIMER_RELAYOUT, 500, nullptr);
            if (msg == WM_DPICHANGED) return 0;
            break;

        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam) BeginExit(L"session ending");
            return 0;

        case WM_CLOSE:
            BeginExit(L"WM_CLOSE");
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    BOOL WINAPI ConsoleCtrlHandler(DWORD type)
    {
        HWND controller = FindWindowW(kControllerClass, nullptr);
        if (controller) PostMessageW(controller, WM_CLOSE, 0, 0);
        if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
        {
            // Process is torn down when we return; give the UI thread time to clean up.
            if (g_cleanupDone) WaitForSingleObject(g_cleanupDone, 4000);
        }
        return TRUE;
    }

    void PrintUsage()
    {
        wprintf(L"LiveDesktop - an interactive Meadow wallpaper living behind the desktop icons\n\n"
                L"Usage: LiveDesktop.exe [options]\n"
                L"  --mock                  use realistic fake session data (no credentials needed)\n"
                L"  --exit-after <seconds>  exit automatically (for testing)\n"
                L"  --no-wallpaper-refresh  do not re-apply the wallpaper on exit\n"
                L"  --layered               experimental: make the host a layered child window\n"
                L"  --stop                  tell the running instance to exit cleanly\n"
                L"  --send <command>        send a command to the running instance:\n"
                L"                            mock:on|off | pause | stop | hit:<x>,<y> | agents?\n"
                L"                            time:<dawn|sunrise|morning|day|afternoon|golden|sunset|dusk|night|HH:MM|auto>\n"
                L"                            agent:<n|id-prefix>:<working|waiting|done|idle|auto> | agent:list | agent:auto\n"
                L"                            render:on|auto (keep animating while covered) | snapshot:<file.png>\n"
                L"Ctrl+C, the tray menu 'Exit', or WM_CLOSE exit cleanly.\n"
                L"Config: %%LOCALAPPDATA%%\\LiveDesktop\\config.json (created with defaults on first run).\n");
    }

    int SendCommand(std::wstring const& command)
    {
        HWND controller = FindWindowW(kControllerClass, nullptr);
        if (!controller)
        {
            wprintf(L"LiveDesktop is not running.\n");
            return 1;
        }
        COPYDATASTRUCT cds{ kCopyDataCommand, static_cast<DWORD>(command.size() * sizeof(wchar_t)), const_cast<wchar_t*>(command.data()) };
        DWORD_PTR result = 0;
        if (!SendMessageTimeoutW(controller, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 5000, &result) || !result)
        {
            wprintf(L"Command '%s' was not accepted.\n", command.c_str());
            return 1;
        }
        wprintf(L"Sent '%s' to running instance.\n", command.c_str());
        return 0;
    }

    bool ParseOptions(int argc, wchar_t** argv, Options& opts, int& commandExit)
    {
        for (int i = 1; i < argc; ++i)
        {
            std::wstring arg = argv[i];
            auto next = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : L"0"; };
            if (arg == L"--exit-after") opts.exitAfterSeconds = static_cast<UINT>(_wtoi(next()));
            else if (arg == L"--no-wallpaper-refresh") opts.refreshWallpaper = false;
            else if (arg == L"--layered") opts.layered = true;
            else if (arg == L"--mock") opts.mock = true;
            else if (arg == L"--send")
            {
                commandExit = SendCommand(next());
                return false;
            }
            else if (arg == L"--stop")
            {
                HWND controller = FindWindowW(kControllerClass, nullptr);
                if (!controller)
                {
                    wprintf(L"LiveDesktop is not running.\n");
                    commandExit = 1;
                    return false;
                }
                PostMessageW(controller, WM_CLOSE, 0, 0);
                wprintf(L"Sent %s to running instance.\n", arg.c_str());
                commandExit = 0;
                return false;
            }
            else
            {
                PrintUsage();
                commandExit = (arg == L"--help" || arg == L"-h" || arg == L"/?") ? 0 : 2;
                return false;
            }
        }
        return true;
    }
}

int wmain(int argc, wchar_t** argv)
{
    int commandExit = 0;
    if (!ParseOptions(argc, argv, g_app.options, commandExit)) return commandExit;

    // Launched from Explorer (we own the console alone): drop the console window; the log file remains.
    {
        DWORD processes[2]{};
        if (GetConsoleProcessList(processes, 2) == 1) FreeConsole();
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        wprintf(L"LiveDesktop is already running (use --stop).\n");
        return 1;
    }

    LogInit();
    // The manifest already declares PerMonitorV2; this is a no-op safety net.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    {
        OSVERSIONINFOW ver{ sizeof(ver) };
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        if (auto rtl = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) rtl(&ver);
        Log(L"LiveDesktop starting. Windows %lu.%lu.%lu, PID %lu, DPI awareness=%d, log: %s",
            ver.dwMajorVersion, ver.dwMinorVersion, ver.dwBuildNumber, GetCurrentProcessId(),
            static_cast<int>(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext())), LogPath().c_str());
    }

    g_cleanupDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    g_app.instance = GetModuleHandleW(nullptr);
    g_app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = ControllerProc;
    wc.hInstance = g_app.instance;
    wc.lpszClassName = kControllerClass;
    RegisterClassExW(&wc);
    g_app.controller = CreateWindowExW(WS_EX_TOOLWINDOW, kControllerClass, L"LiveDesktop", WS_OVERLAPPED,
        0, 0, 0, 0, nullptr, nullptr, g_app.instance, nullptr);
    if (!g_app.controller)
    {
        Log(L"Failed to create controller window: %lu", GetLastError());
        return 1;
    }
    // Allow TaskbarCreated through UIPI if we happen to run elevated.
    ChangeWindowMessageFilterEx(g_app.controller, g_app.taskbarCreated, MSGFLT_ALLOW, nullptr);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    try
    {
        g_app.scene.Initialize();
    }
    catch (winrt::hresult_error const& e)
    {
        Log(L"Composition initialization failed: 0x%08X %s", e.code().value, e.message().c_str());
        return 1;
    }

    g_app.config = LoadConfig();
    g_app.mock = g_app.options.mock || g_app.config.mock;
    Log(L"Config: %s (mock=%d)", g_app.config.path.c_str(), g_app.mock);
    try
    {
        g_app.graphics.Initialize(g_app.scene.Compositor());
        g_app.scene.SetGraphics(&g_app.graphics);
    }
    catch (winrt::hresult_error const& e)
    {
        Log(L"Direct2D/DirectWrite initialization failed: 0x%08X %s", e.code().value, e.message().c_str());
        return 1;
    }
    g_app.graphics.OnDeviceReplaced = [] {
        Log(L"Graphics device replaced -> redrawing surfaces");
        g_app.scene.Refresh();
    };

    g_app.icon = CreateAppIcon(GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()));
    AddTrayIcon();
    UpdateTrayTip();

    if (!TryAttach())
    {
        Log(L"Desktop not ready; retrying every second");
        g_app.reattachAttempts = 0;
        SetTimer(g_app.controller, TIMER_REATTACH, 1000, nullptr);
    }

    g_hookTarget = g_app.controller;
    StartHookThread();

    // Data providers: background threads -> immutable snapshots -> UI thread via the DispatcherQueue.
    {
        auto& p = g_app.providers;
        p.sessions = std::make_unique<SessionsProvider>(g_app.config);
        p.sessions->OnSnapshot = [](SessionsSnapshot const& s) { g_app.sessions = s; ForwardSessions(); };
        auto queue = g_app.scene.Queue();
        p.ForEach([&](Provider& provider) {
            provider.Start(queue, g_app.mock);
            Log(L"Provider '%s': %s", provider.Name().c_str(), provider.Enabled() ? (g_app.mock ? L"mock" : L"real") : L"disabled");
        });
    }

    g_app.reveal.Start(g_app.controller, WM_APP_REVEAL);
    EvaluateReveal();

    SetTimer(g_app.controller, TIMER_HEALTH, 2000, nullptr);
    SetTimer(g_app.controller, TIMER_TICK, 5000, nullptr);
    if (g_app.options.exitAfterSeconds)
    {
        SetTimer(g_app.controller, TIMER_AUTOEXIT, g_app.options.exitAfterSeconds * 1000, nullptr);
        Log(L"Will exit automatically after %u s", g_app.options.exitAfterSeconds);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    winrt::uninit_apartment();
    if (g_cleanupDone) SetEvent(g_cleanupDone);
    ReleaseMutex(mutex);
    return static_cast<int>(msg.wParam);
}
