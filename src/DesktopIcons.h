#pragma once
#include "pch.h"

namespace ld
{
    // Uses the documented shell automation path (IShellWindows -> desktop IShellView -> IFolderView)
    // to tell whether a screen point is over a desktop icon, so ripples only fire on empty space.
    class DesktopIcons
    {
    public:
        // Returns true if `screenPt` lies over an icon. On any failure returns false (=> treat as empty).
        bool HitTestIcon(POINT screenPt);
        void Reset() { m_view = nullptr; m_listView = nullptr; }

    private:
        bool Ensure();

        winrt::com_ptr<IFolderView> m_view;
        HWND m_listView = nullptr;
        bool m_loggedFailure = false;
    };
}
