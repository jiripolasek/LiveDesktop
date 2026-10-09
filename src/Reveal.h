#pragma once
#include "pch.h"

namespace ld
{
    // Tracks whether the desktop is "revealed" (Win+D, everything minimized, or the desktop itself is
    // foreground) and whether it is fully occluded by other windows (so rendering can pause).
    // WinEvent hooks (out-of-context, delivered on the UI thread's message loop) post `message` to
    // `notify`; the owner debounces and calls Evaluate().
    class RevealMonitor
    {
    public:
        struct State
        {
            bool revealed = false;
            bool occluded = false;
            std::wstring reason;   // why revealed / what blocks (for the log)
        };

        void Start(HWND notify, UINT message);
        void Stop();
        // Call when the posted message has been handled, to allow the next post.
        void Acknowledge();
        // `primaryWork` and `workAreas` are in screen coordinates.
        State Evaluate(RECT const& primaryWork, std::vector<RECT> const& workAreas) const;

    private:
        std::vector<HWINEVENTHOOK> m_hooks;
    };
}
