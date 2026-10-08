#include "pch.h"
#include "DesktopHost.h"
#include "Log.h"

namespace ld
{
    namespace
    {
        constexpr wchar_t kHostClass[] = L"LiveDesktop.Host";
        constexpr UINT kSpawnWorkerW = 0x052C;

        std::wstring ClassOf(HWND hwnd)
        {
            wchar_t name[128]{};
            if (hwnd) GetClassNameW(hwnd, name, ARRAYSIZE(name));
            return name;
        }

        BOOL CALLBACK FindShellTopProc(HWND top, LPARAM lParam)
        {
            if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr))
            {
                *reinterpret_cast<HWND*>(lParam) = top;
                return FALSE;
            }
            return TRUE;
        }

        RECT VirtualScreen()
        {
            int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
            return RECT{ x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
        }
    }

    const wchar_t* LayoutName(DesktopLayout layout)
    {
        switch (layout)
        {
        case DesktopLayout::ClassicWorkerW: return L"Classic (top-level WorkerW behind SHELLDLL_DefView)";
        case DesktopLayout::Progman24H2: return L"24H2+ (WorkerW + SHELLDLL_DefView are children of Progman)";
        default: return L"Unknown";
        }
    }

    bool FindDesktopTarget(DesktopWindows& out)
    {
        out = {};
        out.progman = FindWindowW(L"Progman", nullptr);
        if (!out.progman)
        {
            Log(L"Progman not found (Explorer not running yet?)");
            return false;
        }

        // Ask Progman to create the WorkerW that sits behind the icons. 0xD/0x1 is the variant used
        // by modern Explorer; 0/0 is the historical form - send both, they are idempotent.
        DWORD_PTR result = 0;
        LRESULT r1 = SendMessageTimeoutW(out.progman, kSpawnWorkerW, 0xD, 0x1, SMTO_NORMAL, 1000, &result);
        LRESULT r2 = SendMessageTimeoutW(out.progman, kSpawnWorkerW, 0, 0, SMTO_NORMAL, 1000, &result);
        if (!r1 && !r2)
        {
            Log(L"SendMessageTimeout(Progman, 0x052C) failed/timed out (err %lu)", GetLastError());
        }

        // (b) Windows 11 24H2+: everything lives inside Progman.
        HWND defInProgman = FindWindowExW(out.progman, nullptr, L"SHELLDLL_DefView", nullptr);
        HWND workerChild = FindWindowExW(out.progman, nullptr, L"WorkerW", nullptr);
        if (defInProgman && workerChild)
        {
            out.layout = DesktopLayout::Progman24H2;
            out.shellTop = out.progman;
            out.defView = defInProgman;
            out.workerW = workerChild;
            out.parent = out.progman;
            out.insertAfter = defInProgman; // directly below the icons, above the wallpaper WorkerW
            return true;
        }

        // (a) Classic: find the top-level window hosting SHELLDLL_DefView, then the WorkerW after it.
        HWND shellTop = nullptr;
        EnumWindows(FindShellTopProc, reinterpret_cast<LPARAM>(&shellTop));
        if (shellTop)
        {
            HWND worker = FindWindowExW(nullptr, shellTop, L"WorkerW", nullptr);
            if (worker && IsWindowVisible(worker))
            {
                out.layout = DesktopLayout::ClassicWorkerW;
                out.shellTop = shellTop;
                out.defView = FindWindowExW(shellTop, nullptr, L"SHELLDLL_DefView", nullptr);
                out.workerW = worker;
                out.parent = worker;
                out.insertAfter = HWND_TOP;
                return true;
            }
        }

        Log(L"Desktop target not found yet: Progman=0x%p defViewInProgman=0x%p workerChild=0x%p shellTop=0x%p (%s)",
            out.progman, defInProgman, workerChild, shellTop, ClassOf(shellTop).c_str());
        return false;
    }

    bool DesktopHost::Attach(HINSTANCE instance, HWND notifyWindow, UINT lostMessage, bool layered)
    {
        Detach();
        m_notify = notifyWindow;
        m_lostMessage = lostMessage;

        DesktopWindows windows;
        if (!FindDesktopTarget(windows)) return false;

        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = WndProc;
            wc.hInstance = instance;
            wc.lpszClassName = kHostClass;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            registered = RegisterClassExW(&wc) != 0;
            if (!registered)
            {
                Log(L"RegisterClassEx(host) failed: %lu", GetLastError());
                return false;
            }
        }

        // Created as a popup (no parent yet), then converted to WS_CHILD before SetParent as documented.
        // WS_EX_NOREDIRECTIONBITMAP: all content comes from the composition visual tree.
        HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kHostClass, L"LiveDesktop", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (!hwnd)
        {
            Log(L"CreateWindowEx(host) failed: %lu", GetLastError());
            return false;
        }

        if (layered)
        {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
            SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        }

        // WS_DISABLED: never takes focus/input. Clicks that could ever reach it fall through to the
        // parent (Progman/WorkerW), i.e. normal desktop behaviour.
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED);
        if (!SetParent(hwnd, windows.parent))
        {
            Log(L"SetParent(host -> 0x%p) failed: %lu", windows.parent, GetLastError());
            m_detaching = true;
            DestroyWindow(hwnd);
            m_detaching = false;
            return false;
        }

        m_hwnd = hwnd;
        m_windows = windows;
        UpdateBounds();
        SetWindowPos(m_hwnd, m_windows.insertAfter, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);

        Log(L"Detected desktop layout: %s", LayoutName(m_windows.layout));
        Log(L"  Progman=0x%p SHELLDLL_DefView=0x%p (in %s 0x%p) WorkerW=0x%p",
            m_windows.progman, m_windows.defView, ClassOf(m_windows.shellTop).c_str(), m_windows.shellTop, m_windows.workerW);
        Log(L"  Host HWND=0x%p parented to 0x%p (%s), size %ldx%ld%s",
            m_hwnd, m_windows.parent, ClassOf(m_windows.parent).c_str(), m_size.cx, m_size.cy, layered ? L" [layered]" : L"");
        LogZOrder();
        return true;
    }

    void DesktopHost::Detach()
    {
        if (!m_hwnd) return;
        HWND parent = m_windows.parent;
        m_detaching = true;
        if (IsWindow(m_hwnd)) DestroyWindow(m_hwnd);
        m_detaching = false;
        m_hwnd = nullptr;
        // Classic layout: make the WorkerW repaint the area we covered. Async (no RDW_UPDATENOW) so we
        // never block on Explorer.
        if (parent && IsWindow(parent))
        {
            RedrawWindow(parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
        }
        m_windows = {};
    }

    bool DesktopHost::IsHealthy() const
    {
        if (!m_hwnd || !IsWindow(m_hwnd)) return false;
        if (!IsWindow(m_windows.parent) || GetParent(m_hwnd) != m_windows.parent) return false;
        if (FindWindowW(L"Progman", nullptr) != m_windows.progman) return false;
        if (m_windows.layout == DesktopLayout::Progman24H2 &&
            !FindWindowExW(m_windows.progman, nullptr, L"SHELLDLL_DefView", nullptr)) return false;
        if (m_windows.layout == DesktopLayout::ClassicWorkerW && !IsWindowVisible(m_windows.parent)) return false;
        return true;
    }

    void DesktopHost::EnsureZOrder()
    {
        if (!m_hwnd || m_windows.layout != DesktopLayout::Progman24H2) return;
        if (GetWindow(m_hwnd, GW_HWNDPREV) != m_windows.defView)
        {
            Log(L"Host z-order drifted; re-inserting below SHELLDLL_DefView");
            SetWindowPos(m_hwnd, m_windows.defView, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    void DesktopHost::UpdateBounds()
    {
        if (!m_hwnd) return;
        RECT vs = VirtualScreen();
        POINT pts[2] = { { vs.left, vs.top }, { vs.right, vs.bottom } };
        // Screen -> parent client coordinates (Progman/WorkerW origin is the virtual-screen origin,
        // but do not rely on that).
        MapWindowPoints(HWND_DESKTOP, m_windows.parent, pts, 2);
        m_size = { pts[1].x - pts[0].x, pts[1].y - pts[0].y };
        SetWindowPos(m_hwnd, nullptr, pts[0].x, pts[0].y, m_size.cx, m_size.cy,
            SWP_NOZORDER | SWP_NOACTIVATE);
        Log(L"Host bounds: virtual screen (%ld,%ld)-(%ld,%ld) -> parent client (%ld,%ld) %ldx%ld",
            vs.left, vs.top, vs.right, vs.bottom, pts[0].x, pts[0].y, m_size.cx, m_size.cy);
    }

    std::vector<RECT> DesktopHost::MonitorRectsInHost() const
    {
        std::vector<RECT> screens;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT rc, LPARAM lp) -> BOOL
        {
            reinterpret_cast<std::vector<RECT>*>(lp)->push_back(*rc);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&screens));

        for (auto& rc : screens)
        {
            if (m_hwnd) MapWindowPoints(HWND_DESKTOP, m_hwnd, reinterpret_cast<POINT*>(&rc), 2);
        }
        if (screens.empty()) screens.push_back(RECT{ 0, 0, m_size.cx, m_size.cy });
        return screens;
    }

    POINT DesktopHost::ScreenToHost(POINT screen) const
    {
        if (m_hwnd) MapWindowPoints(HWND_DESKTOP, m_hwnd, &screen, 1);
        return screen;
    }

    std::wstring DesktopHost::DisplaySignature()
    {
        std::wstring sig;
        RECT vs = VirtualScreen();
        sig += std::to_wstring(vs.left) + L"," + std::to_wstring(vs.top) + L"," +
               std::to_wstring(vs.right) + L"," + std::to_wstring(vs.bottom);
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT rc, LPARAM lp) -> BOOL
        {
            UINT dpiX = 96, dpiY = 96;
            using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
            static auto getDpi = reinterpret_cast<GetDpiForMonitorFn>(
                GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor"));
            if (getDpi) getDpi(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY);
            auto& s = *reinterpret_cast<std::wstring*>(lp);
            s += L"|" + std::to_wstring(rc->left) + L"," + std::to_wstring(rc->top) + L"," +
                 std::to_wstring(rc->right) + L"," + std::to_wstring(rc->bottom) + L"@" + std::to_wstring(dpiX);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&sig));
        return sig;
    }

    void DesktopHost::LogZOrder() const
    {
        if (!m_windows.parent) return;
        Log(L"  Children of 0x%p (%s), top to bottom:", m_windows.parent, ClassOf(m_windows.parent).c_str());
        int i = 0;
        for (HWND child = GetWindow(m_windows.parent, GW_CHILD); child && i < 16; child = GetWindow(child, GW_HWNDNEXT), ++i)
        {
            Log(L"    [%d] 0x%p %s%s%s", i, child, ClassOf(child).c_str(),
                child == m_hwnd ? L"  <-- LiveDesktop host" : L"",
                IsWindowVisible(child) ? L"" : L" (hidden)");
        }
    }

    LRESULT CALLBACK DesktopHost::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto self = reinterpret_cast<DesktopHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg)
        {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
            break;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;
        case WM_DESTROY:
            if (self && !self->m_detaching && self->m_hwnd == hwnd)
            {
                // Destroyed externally (parent went away with Explorer).
                self->m_hwnd = nullptr;
                if (self->m_notify) PostMessageW(self->m_notify, self->m_lostMessage, 0, 0);
            }
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
