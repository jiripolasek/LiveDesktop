#pragma once
#include "pch.h"

namespace ld
{
    // Captures one frame of a composition visual tree (Windows.Graphics.Capture CreateFromVisual) and saves it
    // as a PNG. Works while the desktop is covered or the workstation is locked, so scripted demos can grab
    // screenshots of the scene without touching the user's screen. Desktop icons are not part of the capture.
    // `done(ok)` is invoked on an arbitrary thread.
    void SnapshotVisual(winrt::Windows::UI::Composition::Visual const& visual, std::wstring const& path,
                        std::function<void(bool)> done);
}
