// LiveDesktop - interactive Windows.UI.Composition wallpaper living behind the desktop icons.
#include "pch.h"
#include "Log.h"
#include "DesktopHost.h"
#include "DesktopIcons.h"
#include "Scene.h"

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

    constexpr UINT_PTR TIMER_HEALTH = 1;
    constexpr UINT_PTR TIMER_REATTACH = 2;
    constexpr UINT_PTR TIMER_RELAYOUT = 3;
    constexpr UINT_PTR TIMER_AUTOEXIT = 4;

    constexpr UINT IDM_NEXT_EFFECT = 100;
    constexpr UINT IDM_PAUSE = 101;
    constexpr UINT IDM_EXIT = 102;
    constexpr UINT kTrayId = 1;

    struct Options
    {
        size_t effect = 0;
        bool layered = false;
        bool refreshWallpaper = true;
        UINT exitAfterSeconds = 0;
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
        bool fullscreenPaused = false;
        int reattachAttempts = 0;
        std::wstring displaySignature;
        HANDLE hookThread = nullptr;
        DWORD hookThreadId = 0;
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

    void UpdatePaused()
    {
        bool paused = g_app.userPaused || g_app.fullscreenPaused;
        g_app.scene.SetPaused(paused);
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
        swprintf_s(nid.szTip, L"LiveDesktop - %s%s", g_app.scene.CurrentEffectName(),
            g_app.userPaused ? L" (paused)" : L"");
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
        StopHookThread();
        Detach();
        RemoveTrayIcon();
        RestoreWallpaper();
        DestroyWindow(g_app.controller);
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
            POINT local = g_app.host.ScreenToHost(pt);
            g_app.scene.PointerMoved(float2(static_cast<float>(local.x), static_cast<float>(local.y)), over);
            return 0;
        }
        case WM_APP_CLICK:
        {
            if (!g_app.host.IsAttached() || g_app.fullscreenPaused) return 0;
            POINT pt{ static_cast<LONG>(wParam), static_cast<LONG>(lParam) };
            if (!IsDesktopAt(pt) || g_app.icons.HitTestIcon(pt)) return 0;
            POINT local = g_app.host.ScreenToHost(pt);
            g_app.scene.Click(float2(static_cast<float>(local.x), static_cast<float>(local.y)));
            return 0;
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
                return 0;
            }
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
                L"  --effect <n>            start with effect n (0=Aurora, 1=Fireflies, 2=Ember)\n"
                L"  --exit-after <seconds>  exit automatically (for testing)\n"
                L"  --no-wallpaper-refresh  do not re-apply the wallpaper on exit\n"
                L"  --layered               experimental: make the host a layered child window\n"
                L"  --next                  tell the running instance to switch effect\n"
                L"  --stop                  tell the running instance to exit cleanly\n"
                L"Ctrl+C, the tray menu 'Exit', or WM_CLOSE exit cleanly.\n");
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
    SetTimer(g_app.controller, TIMER_HEALTH, 2000, nullptr);
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

    g_app.scene.Shutdown();
    if (g_app.icon) DestroyIcon(g_app.icon);
    Log(L"Clean shutdown complete");
    SetEvent(g_cleanupDone);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
