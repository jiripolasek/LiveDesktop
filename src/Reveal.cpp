#include "pch.h"
#include "Reveal.h"
#include "Log.h"

namespace ld
{
    namespace
    {
        HWND g_notify = nullptr;
        UINT g_message = 0;
        std::atomic<bool> g_pending{ false };

        void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
        {
            if (event >= EVENT_OBJECT_CREATE)
            {
                // Object events fire for every control; only top-level windows matter here.
                if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
                if (GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow()) return;
            }
            if (g_notify && !g_pending.exchange(true)) PostMessageW(g_notify, g_message, 0, 0);
        }

        std::wstring ClassName(HWND hwnd)
        {
            wchar_t name[128]{};
            GetClassNameW(hwnd, name, ARRAYSIZE(name));
            return name;
        }

        bool IsShellClass(std::wstring const& cls)
        {
            static const wchar_t* const kShell[] = {
                L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
                L"TopLevelWindowForOverflowXamlIsland", L"XamlExplorerHostIslandWindow", L"Windows.UI.Core.CoreWindow",
                L"Shell_InputSwitchTopLevelWindow", L"ForegroundStaging", L"MultitaskingViewFrame", L"TaskListThumbnailWnd",
                L"IME", L"MSCTFIME UI", L"tooltips_class32", L"SysShadow", L"#32768",
            };
            for (auto const* name : kShell) if (cls == name) return true;
            return false;
        }

        struct Scan
        {
            DWORD self = GetCurrentProcessId();
            std::vector<std::pair<RECT, std::wstring>> windows;
        };

        BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM param)
        {
            auto& scan = *reinterpret_cast<Scan*>(param);
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == scan.self) return TRUE;
            LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if (ex & (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) return TRUE;
            DWORD cloaked = 0;
            if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
            auto cls = ClassName(hwnd);
            if (IsShellClass(cls)) return TRUE;
            RECT r{};
            if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(hwnd, &r);
            if (r.right - r.left < 48 || r.bottom - r.top < 48) return TRUE;
            scan.windows.emplace_back(r, std::move(cls));
            return TRUE;
        }

        double RegionArea(HRGN rgn)
        {
            DWORD bytes = GetRegionData(rgn, 0, nullptr);
            if (!bytes) return 0;
            std::vector<uint8_t> buffer(bytes);
            auto* data = reinterpret_cast<RGNDATA*>(buffer.data());
            if (!GetRegionData(rgn, bytes, data)) return 0;
            auto const* rects = reinterpret_cast<RECT const*>(data->Buffer);
            double area = 0;
            for (DWORD i = 0; i < data->rdh.nCount; ++i)
                area += static_cast<double>(rects[i].right - rects[i].left) * (rects[i].bottom - rects[i].top);
            return area;
        }
    }

    void RevealMonitor::Start(HWND notify, UINT message)
    {
        g_notify = notify;
        g_message = message;
        g_pending = false;
        const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
        const std::pair<DWORD, DWORD> ranges[] = {
            { EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND },
            { EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND },
            { EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND },
            { EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE },
            { EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED },
        };
        for (auto const& [lo, hi] : ranges)
        {
            if (HWINEVENTHOOK hook = SetWinEventHook(lo, hi, nullptr, WinEventProc, 0, 0, flags)) m_hooks.push_back(hook);
            else Log(L"SetWinEventHook(0x%04X..0x%04X) failed: %lu", lo, hi, GetLastError());
        }
        Log(L"Reveal monitor: %zu WinEvent hooks installed", m_hooks.size());
    }

    void RevealMonitor::Stop()
    {
        for (auto hook : m_hooks) UnhookWinEvent(hook);
        m_hooks.clear();
        g_notify = nullptr;
    }

    void RevealMonitor::Acknowledge()
    {
        g_pending = false;
    }

    RevealMonitor::State RevealMonitor::Evaluate(RECT const& primaryWork, std::vector<RECT> const& workAreas) const
    {
        State state;
        HWND fg = GetForegroundWindow();
        std::wstring fgClass = fg ? ClassName(GetAncestor(fg, GA_ROOT)) : L"";

        Scan scan;
        EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&scan));

        std::wstring blocker;
        for (auto const& [r, cls] : scan.windows)
        {
            RECT overlap{};
            if (IntersectRect(&overlap, &r, &primaryWork)) { blocker = cls; break; }
        }

        if (fgClass == L"Progman" || fgClass == L"WorkerW")
        {
            state.revealed = true;
            state.reason = L"desktop is foreground (" + fgClass + L")";
        }
        else if (blocker.empty())
        {
            state.revealed = true;
            state.reason = L"no windows on the primary monitor";
        }
        else
        {
            state.reason = L"covered by '" + blocker + L"'";
        }

        if (!state.revealed && !workAreas.empty())
        {
            HRGN visible = CreateRectRgn(0, 0, 0, 0);
            double total = 0;
            for (auto const& w : workAreas)
            {
                HRGN part = CreateRectRgnIndirect(&w);
                CombineRgn(visible, visible, part, RGN_OR);
                DeleteObject(part);
                total += static_cast<double>(w.right - w.left) * (w.bottom - w.top);
            }
            for (auto const& [r, cls] : scan.windows)
            {
                HRGN part = CreateRectRgnIndirect(&r);
                CombineRgn(visible, visible, part, RGN_DIFF);
                DeleteObject(part);
            }
            double remaining = RegionArea(visible);
            DeleteObject(visible);
            state.occluded = total > 0 && remaining / total < 0.005;
        }
        return state;
    }
}
