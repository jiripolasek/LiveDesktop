// LiveDesktop - interactive Windows.UI.Composition wallpaper living behind the desktop icons.
#include "pch.h"
#include "Log.h"
#include "DesktopHost.h"
#include "DesktopIcons.h"
#include "Scene.h"
#include "Config.h"
#include "Providers.h"
#include "Graphics.h"
#include "Ambient.h"
#include "Dashboard.h"
#include "Reveal.h"
#include "Clipboard.h"
#include "ClipboardStack.h"
#include "DropWindow.h"
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
    constexpr UINT_PTR TIMER_DROPHIDE = 7;

    constexpr UINT IDM_NEXT_EFFECT = 100;
    constexpr UINT IDM_PAUSE = 101;
    constexpr UINT IDM_EXIT = 102;
    constexpr UINT IDM_MOCK = 103;
    constexpr UINT IDM_DASHBOARD = 104;
    constexpr UINT IDM_NEXT_SCENE = 105;
    constexpr UINT IDM_CLIPBOARD = 106;
    constexpr UINT kTrayId = 1;
    constexpr ULONG_PTR kCopyDataCommand = 0x4C44; // 'LD'

    enum class DashboardMode { Auto, On, Off };

    struct Options
    {
        size_t effect = 0;
        bool layered = false;
        bool refreshWallpaper = true;
        UINT exitAfterSeconds = 0;
        bool mock = false;
        std::wstring scene;          // forced scene (overrides the virtual-desktop mapping)
        bool dashboard = false;      // start with the dashboard forced on
    };

    struct Providers
    {
        std::unique_ptr<SessionsProvider> sessions;
        std::unique_ptr<GitHubProvider> github;
        std::unique_ptr<SystemProvider> system;
        std::unique_ptr<CalendarProvider> calendar;
        std::unique_ptr<PortsProvider> ports;
        std::unique_ptr<LogTailProvider> logTail;
        std::unique_ptr<WorktreesProvider> worktrees;

        template <class F> void ForEach(F&& f)
        {
            Provider* all[] = { sessions.get(), github.get(), system.get(), calendar.get(), ports.get(), logTail.get(), worktrees.get() };
            for (auto* p : all) if (p) f(*p);
        }
    };

    // Left-button press being tracked for a click on a dashboard card or a clipboard card.
    struct PendingClick
    {
        bool active = false;
        POINT screen{};
        ULONGLONG tick = 0;
        Dashboard::Hit hit;
        std::optional<uint64_t> clip;
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
        Ambient ambient;
        Dashboard dashboard;
        RevealMonitor reveal;
        Providers providers;
        VirtualDesktopWatcher desktops;
        SceneLayout layout;
        bool mock = false;
        std::wstring sceneName = L"default";
        std::wstring sceneOverride;        // tray / --scene / --send; cleared when the virtual desktop changes
        VirtualDesktopInfo desktop;
        DashboardMode dashboardMode = DashboardMode::Auto;
        bool revealed = false;
        bool occluded = false;
        std::wstring revealReason;
        PendingClick click;
        ClipboardWatcher clipboard;
        ClipboardStack stack;
        DropWindow drop;
        bool buttonDown = false;           // left button held anywhere (drag-to-shelf detection)
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

    // Paused = nothing animates. "Heavy" pause (fullscreen app, or the desktop fully covered with no
    // dashboard showing) also throttles the providers.
    void UpdatePaused()
    {
        // Fully occluded means nothing of the desktop is visible, so even a forced dashboard can stop rendering.
        bool heavy = !g_app.forceRender && (g_app.fullscreenPaused || g_app.occluded);
        bool paused = g_app.userPaused || heavy;
        if (paused != g_app.scene.Paused())
        {
            Log(L"Rendering %s (user=%d fullscreen=%d occluded=%d dashboard=%d)", paused ? L"paused" : L"resumed",
                g_app.userPaused, g_app.fullscreenPaused, g_app.occluded, g_app.dashboard.Visible());
        }
        g_app.scene.SetPaused(paused);

        static int lastThrottle = -1;
        int throttle = heavy ? 4 : 1;
        if (throttle != lastThrottle)
        {
            lastThrottle = throttle;
            Provider* system = g_app.providers.system.get();
            g_app.providers.ForEach([&](Provider& p) { p.SetThrottle(&p == system ? (heavy ? 0 : 1) : throttle); });
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
        swprintf_s(nid.szTip, L"LiveDesktop - %s, %s scene%s%s", g_app.scene.CurrentEffectName(), g_app.sceneName.c_str(),
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
        swprintf_s(nid.szTip, L"LiveDesktop - %s", g_app.scene.CurrentEffectName());
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
        wchar_t next[128];
        swprintf_s(next, L"Next effect\t(current: %s)", g_app.scene.CurrentEffectName());
        AppendMenuW(menu, MF_STRING, IDM_NEXT_EFFECT, next);
        wchar_t scene[128];
        swprintf_s(scene, L"Next scene\t(current: %s)", g_app.sceneName.c_str());
        AppendMenuW(menu, MF_STRING, IDM_NEXT_SCENE, scene);
        AppendMenuW(menu, MF_STRING | (g_app.dashboardMode == DashboardMode::On ? MF_CHECKED : 0), IDM_DASHBOARD, L"Force dashboard");
        AppendMenuW(menu, MF_STRING | (g_app.mock ? MF_CHECKED : 0), IDM_MOCK, L"Mock data");
        AppendMenuW(menu, MF_STRING | (g_app.stack.Enabled() ? MF_CHECKED : 0), IDM_CLIPBOARD, L"Clipboard shelf");
        AppendMenuW(menu, MF_STRING | (g_app.userPaused ? MF_CHECKED : 0), IDM_PAUSE, L"Pause animations");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
        SetMenuDefaultItem(menu, IDM_NEXT_EFFECT, FALSE);
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

    // ---------------------------------------------------------------------------------------------
    // Layout, scenes, dashboard
    // ---------------------------------------------------------------------------------------------
    float2 ToLocal(POINT screen)
    {
        POINT p = g_app.host.ScreenToHost(screen);
        return float2(static_cast<float>(p.x), static_cast<float>(p.y));
    }

    RECT RectToHost(RECT const& r)
    {
        POINT a = g_app.host.ScreenToHost(POINT{ r.left, r.top });
        POINT b = g_app.host.ScreenToHost(POINT{ r.right, r.bottom });
        return RECT{ a.x, a.y, b.x, b.y };
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

    bool SameRects(std::vector<RECT> const& a, std::vector<RECT> const& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) if (!EqualRect(&a[i], &b[i])) return false;
        return true;
    }

    // Recomputes the host-space layout (monitors, primary work area, DPI scale, icon cells) and pushes it
    // to the ambient + dashboard layers if anything changed.
    void ApplyLayout(bool force)
    {
        if (!g_app.host.IsAttached()) return;
        SceneLayout layout;
        layout.size = HostSize();
        layout.monitors = g_app.host.MonitorRectsInHost();
        MONITORINFO mi;
        HMONITOR primary = PrimaryMonitor(mi);
        layout.primary = RectToHost(mi.rcMonitor);
        layout.primaryWork = RectToHost(mi.rcWork);
        UINT dpiX = 96, dpiY = 96;
        if (SUCCEEDED(GetDpiForMonitor(primary, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) layout.scale = dpiX / 96.0f;
        std::vector<RECT> icons;
        if (g_app.icons.IconRects(icons))
        {
            for (auto const& r : icons) layout.icons.push_back(RectToHost(r));
        }
        else
        {
            layout.icons = g_app.layout.icons; // keep the last known cells if Explorer is busy
        }

        auto const& old = g_app.layout;
        bool changed = force || old.size.x != layout.size.x || old.size.y != layout.size.y || old.scale != layout.scale ||
            !EqualRect(&old.primaryWork, &layout.primaryWork) || !SameRects(old.monitors, layout.monitors) ||
            !SameRects(old.icons, layout.icons);
        if (!changed) return;
        g_app.layout = layout;
        Log(L"Layout: host %.0fx%.0f, primary work (%ld,%ld)-(%ld,%ld) host px, scale %.2f, %zu icon cells",
            layout.size.x, layout.size.y, layout.primaryWork.left, layout.primaryWork.top, layout.primaryWork.right,
            layout.primaryWork.bottom, layout.scale, layout.icons.size());
        g_app.graphics.SetScale(layout.scale);
        g_app.ambient.SetLayout(layout);
        g_app.stack.SetLayout(layout);
        g_app.dashboard.SetReserved(g_app.stack.Enabled() ? g_app.stack.Footprint() : RECT{});
        g_app.dashboard.SetLayout(layout);
    }

    std::wstring ResolveScene()
    {
        if (!g_app.sceneOverride.empty()) return g_app.sceneOverride;
        auto const& d = g_app.desktop;
        std::wstring id = ToLower(d.id), name = ToLower(d.name), index = std::to_wstring(d.index);
        for (auto const& [key, scene] : g_app.config.desktopScenes)
        {
            if ((!id.empty() && key == id) || (!name.empty() && key == name) || (d.index > 0 && key == index)) return scene;
        }
        return g_app.config.defaultScene;
    }

    void UpdateDashboard();

    void ApplyScene(bool force = false)
    {
        ScenePreset preset = PresetFor(ResolveScene());
        if (!force && preset.name == g_app.sceneName) return;
        g_app.sceneName = preset.name;
        Log(L"Scene '%s' (virtual desktop %d '%s' %s%s)", preset.name.c_str(), g_app.desktop.index,
            g_app.desktop.name.c_str(), g_app.desktop.id.c_str(), g_app.sceneOverride.empty() ? L"" : L", overridden");
        g_app.ambient.SetPreset(preset);
        g_app.dashboard.SetPreset(preset);
        g_app.stack.SetPreset(preset);
        UpdateDashboard();
        UpdateTrayTip();
    }

    void UpdateDashboard()
    {
        bool want = g_app.dashboardMode == DashboardMode::On ||
            (g_app.dashboardMode == DashboardMode::Auto && g_app.revealed && !g_app.fullscreenPaused);
        bool before = g_app.dashboard.Visible();
        // Icons may have moved since the last reveal: refresh the free region before the cards animate in.
        if (want && !before) ApplyLayout(false);
        g_app.dashboard.Show(want);
        bool shown = g_app.dashboard.Visible();
        if (shown != before)
        {
            Log(L"Dashboard %s", shown ? L"shown" : L"hidden");
            g_app.ambient.SetDashboardVisible(shown);
            if (!shown) g_app.dashboard.Hover(std::nullopt);
            if (shown)
            {
                // Refresh cheap, glanceable data the moment the user looks at it.
                if (g_app.providers.ports) g_app.providers.ports->Wake();
                if (g_app.providers.sessions) g_app.providers.sessions->Wake();
                if (g_app.providers.worktrees) g_app.providers.worktrees->Wake();
            }
        }
        UpdatePaused();
    }

    void EvaluateReveal()
    {
        MONITORINFO mi;
        PrimaryMonitor(mi);
        auto state = g_app.reveal.Evaluate(mi.rcWork, WorkAreas());
        if (state.revealed != g_app.revealed || state.occluded != g_app.occluded ||
            (!state.revealed && state.reason != g_app.revealReason))
        {
            Log(L"Desktop %s%s: %s", state.revealed ? L"revealed" : L"not revealed",
                state.occluded ? L" (fully occluded)" : L"", state.reason.c_str());
        }
        g_app.revealed = state.revealed;
        g_app.occluded = state.occluded;
        g_app.revealReason = state.reason;
        UpdateDashboard();
    }

    void SetMock(bool mock)
    {
        if (g_app.mock == mock) return;
        g_app.mock = mock;
        Log(L"Mock data %s", mock ? L"on" : L"off");
        g_app.providers.ForEach([&](Provider& p) { p.SetMock(mock); });
        g_app.clipboard.SetMock(mock);
        UpdateTrayTip();
    }

    void StartClipboard()
    {
        g_app.clipboard.OnSnapshot = [](ClipboardSnapshot const& s) { g_app.stack.Update(s); };
        g_app.clipboard.Start(g_app.scene.Queue(), g_app.mock, static_cast<size_t>(g_app.config.clipboardItems));
    }

    // Hiding the shelf also stops watching the clipboard (and forgets the in-memory history).
    void SetClipboardShelf(bool enabled)
    {
        if (enabled == g_app.stack.Enabled()) return;
        g_app.drop.Hide();
        if (enabled) StartClipboard();
        else
        {
            g_app.clipboard.Stop();
            g_app.stack.Update(ClipboardSnapshot{});
        }
        g_app.stack.SetEnabled(enabled);
        ApplyLayout(true);
    }

    void SetSceneOverride(std::wstring name)
    {
        g_app.sceneOverride = std::move(name);
        ApplyScene();
    }

    void NextScene()
    {
        auto names = SceneNames();
        auto it = std::find(names.begin(), names.end(), g_app.sceneName);
        size_t next = it == names.end() ? 0 : (static_cast<size_t>(it - names.begin()) + 1) % names.size();
        SetSceneOverride(names[next]);
    }

    void SetDashboardMode(DashboardMode mode)
    {
        g_app.dashboardMode = mode;
        Log(L"Dashboard mode: %s", mode == DashboardMode::On ? L"forced on" : mode == DashboardMode::Off ? L"forced off" : L"auto (Win+D)");
        UpdateDashboard();
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
        ApplyLayout(true);
        Log(L"Attached. Effect '%s' rendering into HWND 0x%p (parent 0x%p)",
            g_app.scene.CurrentEffectName(), g_app.host.Hwnd(), g_app.host.Windows().parent);
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
        ApplyLayout(true);
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
        KillTimer(g_app.controller, TIMER_DROPHIDE);
        g_app.reveal.Stop();
        StopHookThread();
        g_app.drop.Destroy();
        g_app.clipboard.Stop();
        g_app.desktops.Stop();
        g_app.providers.ForEach([](Provider& p) { p.Stop(); });
        Detach();
        RemoveTrayIcon();
        RestoreWallpaper();
        DestroyWindow(g_app.controller);
    }

    // Sessions from the provider with any --send agent:<n>:<state> overrides applied, to every consumer.
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
        g_app.ambient.Update(s);
        g_app.dashboard.Update(s);
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
        if (ToLower(trimmed.substr(0, 5)) == L"clip:")
        {
            // Case-preserving: simulates a drop of text onto the shelf.
            Log(L"Command: clip (%zu chars)", trimmed.size() - 5);
            g_app.clipboard.AddText(trimmed.substr(5), L"send");
            return;
        }
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
        if (cmd == L"next-effect" || cmd == L"next") { g_app.scene.NextEffect(); UpdateTrayTip(); }
        else if (cmd == L"next-scene") NextScene();
        else if (arg(L"scene:", value)) SetSceneOverride(value == L"auto" ? L"" : value);
        else if (arg(L"effect:", value)) { g_app.scene.SetEffect(static_cast<size_t>(_wtoi(value.c_str())) % g_app.scene.EffectCount()); UpdateTrayTip(); }
        else if (arg(L"time:", value))
        {
            float hour = MeadowPhaseHour(value);
            if (hour < -1.5f) Log(L"Unknown time '%s'", value.c_str());
            else g_app.scene.SetTimeOverride(hour);
        }
        else if (arg(L"dashboard:", value))
            SetDashboardMode(value == L"on" ? DashboardMode::On : value == L"off" ? DashboardMode::Off : DashboardMode::Auto);
        else if (arg(L"mock:", value)) SetMock(value == L"on");
        else if (arg(L"agent:", value)) HandleAgentCommand(value);
        else if (arg(L"render:", value)) { g_app.forceRender = value == L"on"; UpdatePaused(); }
        else if (arg(L"stack:", value))
        {
            if (value == L"open") g_app.stack.ForceFan(true);
            else if (value == L"close") g_app.stack.ForceFan(false);
            else if (value == L"auto") g_app.stack.ForceFan(std::nullopt);
            else if (value == L"drop") g_app.stack.SetDropHover(true);
            else if (value == L"nodrop") g_app.stack.SetDropHover(false);
            else if (value == L"on" || value == L"off") SetClipboardShelf(value == L"on");
        }
        else if (arg(L"ci:", value))
        {
            Mocks().ci = value == L"red" ? 1 : value == L"green" ? -1 : 0;
            if (g_app.providers.github) g_app.providers.github->Wake();
        }
        else if (arg(L"log:", value))
        {
            Mocks().logError = value == L"error";
            if (g_app.providers.logTail) g_app.providers.logTail->Wake();
        }
        else if (cmd == L"pause") { g_app.userPaused = !g_app.userPaused; UpdatePaused(); UpdateTrayTip(); }
        else if (cmd == L"stop" || cmd == L"exit") BeginExit(L"--send stop");
        else if (cmd.rfind(L"hit:", 0) == 0)
        {
            // Diagnostic: report what a click at screen x,y would do, without clicking.
            POINT pt{};
            if (swscanf_s(cmd.c_str() + 4, L"%ld,%ld", &pt.x, &pt.y) != 2) return;
            ReplyMessage(TRUE); // release the sender so the icon COM call isn't an input-synchronous callout
            float2 local = ToLocal(pt);
            auto hit = g_app.dashboard.HitTest(local);
            auto agent = g_app.scene.HitAgent(local);
            Log(L"hit %ld,%ld -> local %.0f,%.0f desktop=%d icon=%d card=%d item=%s agent=%s", pt.x, pt.y, local.x, local.y,
                IsDesktopAt(pt) ? 1 : 0, g_app.icons.HitTestIcon(pt) ? 1 : 0, g_app.dashboard.InsideCard(local) ? 1 : 0,
                hit ? hit->url.c_str() : L"-", agent ? agent->c_str() : L"-");
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
            bool onStack = over && g_app.stack.Contains(local);
            if (!g_app.buttonDown) g_app.stack.Hover(over ? std::optional<float2>(local) : std::nullopt);
            if (g_app.dashboard.Visible())
                g_app.dashboard.Hover(over && !onStack ? std::optional<float2>(local) : std::nullopt);
            if (g_app.scene.HostsAgents())
            {
                bool free = over && !onStack && !(g_app.dashboard.Visible() && g_app.dashboard.InsideCard(local));
                g_app.scene.HoverAgent(free ? std::optional<float2>(local) : std::nullopt);
            }

            // Something is being dragged towards the shelf: put the (invisible) drop window over it.
            if (g_app.buttonDown && g_app.stack.Enabled() && !g_app.fullscreenPaused &&
                (std::abs(pt.x - g_app.downAt.x) > 8 || std::abs(pt.y - g_app.downAt.y) > 8))
            {
                RECT zone = g_app.stack.DropZone();
                POINT origin{};
                MapWindowPoints(g_app.host.Hwnd(), HWND_DESKTOP, &origin, 1);
                OffsetRect(&zone, origin.x, origin.y);
                bool inZone = PtInRect(&zone, pt) != FALSE;
                if (inZone && !g_app.drop.Visible() && IsDesktopAt(pt)) g_app.drop.Show(zone);
                else if (!inZone && g_app.drop.Visible() && !g_app.drop.InDrag()) g_app.drop.Hide();
            }
            return 0;
        }
        case WM_APP_CLICK:
        {
            g_app.click.active = false;
            POINT pt{ static_cast<LONG>(wParam), static_cast<LONG>(lParam) };
            g_app.buttonDown = true;
            g_app.downAt = pt;
            KillTimer(hwnd, TIMER_DROPHIDE);
            if (!g_app.host.IsAttached() || g_app.fullscreenPaused) return 0;
            if (!IsDesktopAt(pt) || g_app.icons.HitTestIcon(pt)) return 0;
            float2 local = ToLocal(pt);
            if (g_app.stack.Contains(local))
            {
                if (auto id = g_app.stack.HitTest(local))
                    g_app.click = PendingClick{ true, pt, GetTickCount64(), {}, id };
                return 0;
            }
            if (g_app.dashboard.Visible() && g_app.dashboard.InsideCard(local))
            {
                // Cards act on button-up (a click, not a drag/rubber-band selection).
                if (auto hit = g_app.dashboard.HitTest(local))
                    g_app.click = PendingClick{ true, pt, GetTickCount64(), *hit, std::nullopt };
                return 0;
            }
            if (auto agent = g_app.scene.HitAgent(local))
            {
                // Animals open their session on button-up, like the cards.
                g_app.click = PendingClick{ true, pt, GetTickCount64(), {}, std::nullopt, agent };
                return 0;
            }
            g_app.scene.Click(local);
            return 0;
        }
        case WM_APP_RELEASE:
        {
            g_app.buttonDown = false;
            // An OLE drop completes around button-up; give it a moment, then tidy up.
            if (g_app.drop.Visible()) SetTimer(hwnd, TIMER_DROPHIDE, 500, nullptr);
            if (!g_app.click.active) return 0;
            g_app.click.active = false;
            POINT pt{ static_cast<LONG>(wParam), static_cast<LONG>(lParam) };
            if (std::abs(pt.x - g_app.click.screen.x) > 6 || std::abs(pt.y - g_app.click.screen.y) > 6 ||
                GetTickCount64() - g_app.click.tick > 600) return 0;
            if (!IsDesktopAt(pt) || g_app.icons.HitTestIcon(pt)) return 0;
            float2 local = ToLocal(pt);
            if (g_app.click.clip)
            {
                if (g_app.stack.HitTest(local) == g_app.click.clip)
                {
                    g_app.stack.Activate(*g_app.click.clip);
                    g_app.clipboard.Copy(*g_app.click.clip);
                }
                return 0;
            }
            if (g_app.click.agent)
            {
                if (g_app.scene.HitAgent(local) != g_app.click.agent) return 0;
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
            if (!g_app.dashboard.Visible()) return 0;
            auto hit = g_app.dashboard.HitTest(local);
            if (hit && *hit == g_app.click.hit) g_app.dashboard.Activate(*hit);
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
            case NIN_SELECT:
            case NIN_KEYSELECT:
                SendMessageW(hwnd, WM_COMMAND, IDM_NEXT_EFFECT, 0);
                break;
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDM_NEXT_EFFECT:
                g_app.scene.NextEffect();
                UpdateTrayTip();
                break;
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
            case IDM_DASHBOARD:
                SetDashboardMode(g_app.dashboardMode == DashboardMode::On ? DashboardMode::Auto : DashboardMode::On);
                break;
            case IDM_NEXT_SCENE:
                NextScene();
                break;
            case IDM_CLIPBOARD:
                SetClipboardShelf(!g_app.stack.Enabled());
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
                    g_app.ambient.Tick();
                    g_app.dashboard.Tick();
                    g_app.stack.Tick();
                }
                return 0;
            case TIMER_DROPHIDE:
                KillTimer(hwnd, TIMER_DROPHIDE);
                if (!g_app.drop.InDrag())
                {
                    g_app.drop.Hide();
                    g_app.stack.SetDropHover(false);
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
        wprintf(L"LiveDesktop - interactive composition wallpaper behind the desktop icons\n\n"
                L"Usage: LiveDesktop.exe [options]\n"
                L"  --effect <n>            start with effect n (0=Meadow, 1=Aurora, 2=Fireflies, 3=Ember)\n"
                L"  --mock                  use realistic fake data for every provider (no credentials needed)\n"
                L"  --scene <name>          force a scene: default | work | focus (else per virtual desktop)\n"
                L"  --dashboard             start with the dashboard forced visible (no Win+D needed)\n"
                L"  --exit-after <seconds>  exit automatically (for testing)\n"
                L"  --no-wallpaper-refresh  do not re-apply the wallpaper on exit\n"
                L"  --layered               experimental: make the host a layered child window\n"
                L"  --next                  tell the running instance to switch effect\n"
                L"  --stop                  tell the running instance to exit cleanly\n"
                L"  --send <command>        send a command to the running instance:\n"
                L"                            next-effect | effect:<n> | next-scene | scene:<name|auto>\n"
                L"                            dashboard:on|off|auto | mock:on|off | ci:red|green|mixed\n"
                L"                            log:error|ok | pause | stop | hit:<x>,<y>\n"
                L"                            time:<dawn|sunrise|morning|day|afternoon|golden|sunset|dusk|night|HH:MM|auto>\n"
                L"                            stack:open|close|auto|drop|nodrop|on|off | clip:<text>\n"
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
            if (arg == L"--effect") opts.effect = static_cast<size_t>(_wtoi(next()));
            else if (arg == L"--exit-after") opts.exitAfterSeconds = static_cast<UINT>(_wtoi(next()));
            else if (arg == L"--no-wallpaper-refresh") opts.refreshWallpaper = false;
            else if (arg == L"--layered") opts.layered = true;
            else if (arg == L"--mock") opts.mock = true;
            else if (arg == L"--scene") opts.scene = ToLower(next());
            else if (arg == L"--dashboard") opts.dashboard = true;
            else if (arg == L"--send")
            {
                commandExit = SendCommand(next());
                return false;
            }
            else if (arg == L"--stop" || arg == L"--next")
            {
                HWND controller = FindWindowW(kControllerClass, nullptr);
                if (!controller)
                {
                    wprintf(L"LiveDesktop is not running.\n");
                    commandExit = 1;
                    return false;
                }
                if (arg == L"--stop") PostMessageW(controller, WM_CLOSE, 0, 0);
                else PostMessageW(controller, WM_COMMAND, IDM_NEXT_EFFECT, 0);
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
        wprintf(L"LiveDesktop is already running (use --stop or --next).\n");
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
    g_app.scene.SetEffect(g_app.options.effect % g_app.scene.EffectCount());

    g_app.config = LoadConfig();
    g_app.mock = g_app.options.mock || g_app.config.mock;
    Log(L"Config: %s (%zu repos, mock=%d)", g_app.config.path.c_str(), g_app.config.repos.size(), g_app.mock);
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
        g_app.ambient.RedrawAll();
        g_app.dashboard.RedrawAll();
        g_app.stack.RedrawAll();
        g_app.scene.Refresh();
    };
    // The log tail is opt-in (config logTail.enabled + logPath); mock mode always demos it.
    Config providerConfig = g_app.config;
    if (g_app.mock) providerConfig.logTail.enabled = true;
    g_app.ambient.Initialize(g_app.scene, g_app.graphics, g_app.config.focusMinutes);
    g_app.scene.OnEffectChanged = [] { g_app.ambient.SetOrbsHidden(g_app.scene.HostsAgents()); };
    g_app.scene.OnEffectChanged();
    g_app.dashboard.Initialize(g_app.scene, g_app.graphics, providerConfig.logTail.enabled);
    g_app.stack.Initialize(g_app.scene, g_app.graphics);
    g_app.stack.SetEnabled(g_app.config.clipboard.enabled);
    g_app.drop.OnDragHover = [](bool active) { g_app.stack.SetDropHover(active); };
    g_app.drop.OnDrop = [](std::vector<std::wstring> files, std::wstring text) {
        Log(L"Dropped on the clipboard shelf: %zu file(s), %zu chars", files.size(), text.size());
        if (!files.empty())
        {
            g_app.stack.Dropped(files.size() == 1 ? std::wstring(L"1 file") : std::to_wstring(files.size()) + L" files");
            g_app.clipboard.AddFiles(std::move(files), L"Dropped");
        }
        else
        {
            g_app.stack.Dropped(L"text");
            g_app.clipboard.AddText(std::move(text), L"Dropped");
        }
    };
    // OLE (on top of the STA set up above) for the shelf's drop target.
    bool ole = SUCCEEDED(OleInitialize(nullptr));
    if (ole) g_app.drop.Create(g_app.instance);
    else Log(L"OleInitialize failed - dropping onto the clipboard shelf is disabled");
    g_app.sceneOverride = g_app.options.scene;
    g_app.desktop = VirtualDesktopWatcher::ReadCurrent();
    g_app.dashboardMode = g_app.options.dashboard ? DashboardMode::On : DashboardMode::Auto;
    ApplyScene(true);

    g_app.icon = CreateAppIcon(GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()));
    AddTrayIcon();

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
        p.sessions = std::make_unique<SessionsProvider>(providerConfig);
        p.github = std::make_unique<GitHubProvider>(providerConfig);
        p.system = std::make_unique<SystemProvider>(providerConfig);
        p.calendar = std::make_unique<CalendarProvider>(providerConfig);
        p.ports = std::make_unique<PortsProvider>(providerConfig);
        p.logTail = std::make_unique<LogTailProvider>(providerConfig);
        p.worktrees = std::make_unique<WorktreesProvider>(providerConfig);
        p.worktrees->OnSnapshot = [](WorktreesSnapshot const& s) { g_app.dashboard.Update(s); };
        p.sessions->OnSnapshot = [](SessionsSnapshot const& s) { g_app.sessions = s; ForwardSessions(); };
        p.github->OnSnapshot = [](PullRequestsSnapshot const& s) { g_app.ambient.Update(s); g_app.dashboard.Update(s); };
        p.system->OnSnapshot = [](SystemSnapshot const& s) { g_app.ambient.Update(s); };
        p.calendar->OnSnapshot = [](CalendarSnapshot const& s) { g_app.ambient.Update(s); g_app.dashboard.Update(s); };
        p.ports->OnSnapshot = [](PortsSnapshot const& s) { g_app.dashboard.Update(s); };
        p.logTail->OnSnapshot = [](LogTailSnapshot const& s) { g_app.dashboard.Update(s); };
        auto queue = g_app.scene.Queue();
        p.ForEach([&](Provider& provider) {
            provider.Start(queue, g_app.mock);
            Log(L"Provider '%s': %s", provider.Name().c_str(), provider.Enabled() ? (g_app.mock ? L"mock" : L"real") : L"disabled");
        });
    }
    if (g_app.config.clipboard.enabled) StartClipboard();

    g_app.desktops.OnChanged = [](VirtualDesktopInfo const& info) {
        if (info.id == g_app.desktop.id) return;
        g_app.desktop = info;
        g_app.sceneOverride.clear();
        Log(L"Virtual desktop switched to %d '%s' %s", info.index, info.name.c_str(), info.id.c_str());
        ApplyScene();
    };
    g_app.desktops.Start(g_app.scene.Queue());

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

    g_app.providers.ForEach([](Provider& p) { p.Stop(); });
    g_app.providers = Providers{};
    g_app.clipboard.Stop();
    g_app.drop.Destroy();
    g_app.stack.Shutdown();
    g_app.dashboard.Shutdown();
    g_app.ambient.Shutdown();
    g_app.graphics.Shutdown();
    g_app.scene.Shutdown();
    if (ole) OleUninitialize();
    if (g_app.icon) DestroyIcon(g_app.icon);
    Log(L"Clean shutdown complete");
    SetEvent(g_cleanupDone);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
