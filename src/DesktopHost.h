#pragma once
#include "pch.h"

namespace ld
{
    enum class DesktopLayout
    {
        Unknown,
        // Pre-24H2: 0x052C moves SHELLDLL_DefView into a top-level WorkerW and spawns a second
        // top-level WorkerW directly behind it. We parent into that second WorkerW.
        ClassicWorkerW,
        // Windows 11 24H2+: Progman keeps SHELLDLL_DefView as a child and the wallpaper WorkerW
        // is ALSO a child of Progman. We parent into Progman between DefView and the WorkerW.
        Progman24H2,
    };

    const wchar_t* LayoutName(DesktopLayout layout);

    struct DesktopWindows
    {
        DesktopLayout layout = DesktopLayout::Unknown;
        HWND progman = nullptr;
        HWND defView = nullptr;     // SHELLDLL_DefView (hosts the SysListView32 icon layer)
        HWND shellTop = nullptr;    // top-level window that contains SHELLDLL_DefView
        HWND workerW = nullptr;     // wallpaper WorkerW
        HWND parent = nullptr;      // window our host is parented to
        HWND insertAfter = nullptr; // z-order anchor for our host inside `parent`
    };

    // Spawns the WorkerW (message 0x052C) and resolves the target for the current layout.
    bool FindDesktopTarget(DesktopWindows& out);

    // Owns the child HWND that lives behind the desktop icons and receives the composition tree.
    class DesktopHost
    {
    public:
        DesktopHost() = default;
        DesktopHost(DesktopHost const&) = delete;
        DesktopHost& operator=(DesktopHost const&) = delete;
        ~DesktopHost() { Detach(); }

        // notifyWindow receives `lostMessage` if the host is destroyed out from under us
        // (e.g. Explorer crashed and took its child windows with it).
        bool Attach(HINSTANCE instance, HWND notifyWindow, UINT lostMessage, bool layered);
        void Detach();

        bool IsAttached() const { return m_hwnd != nullptr; }
        bool IsHealthy() const;
        void EnsureZOrder();
        void UpdateBounds();

        HWND Hwnd() const { return m_hwnd; }
        DesktopWindows const& Windows() const { return m_windows; }
        SIZE Size() const { return m_size; }
        std::vector<RECT> MonitorRectsInHost() const;
        POINT ScreenToHost(POINT screen) const;

        // Fingerprint of the virtual screen + monitor layout; used to detect topology/DPI changes.
        static std::wstring DisplaySignature();

        void LogZOrder() const;

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        HWND m_hwnd = nullptr;
        HWND m_notify = nullptr;
        UINT m_lostMessage = 0;
        bool m_detaching = false;
        SIZE m_size{};
        DesktopWindows m_windows;
    };
}
