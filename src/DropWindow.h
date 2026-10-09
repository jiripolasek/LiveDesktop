#pragma once
#include "pch.h"

namespace ld
{
    // A transient, invisible (alpha 1) top-most window shown over the clipboard shelf only while a drag
    // is in progress there. It is the OLE drop target for the shelf: the shelf itself lives behind the
    // desktop icons and can't receive drops. Only DROPEFFECT_COPY is ever returned, so the drag source
    // never deletes anything.
    class DropWindow
    {
    public:
        bool Create(HINSTANCE instance);
        void Destroy();
        void Show(RECT const& screen);
        void Hide();
        bool Visible() const { return m_visible; }
        bool InDrag() const { return m_inDrag; }
        HWND Hwnd() const { return m_hwnd; }

        std::function<void(bool active)> OnDragHover;
        std::function<void(std::vector<std::wstring> files, std::wstring text)> OnDrop;

    private:
        friend struct DropTarget;
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        HWND m_hwnd = nullptr;
        bool m_visible = false;
        bool m_inDrag = false;
        winrt::com_ptr<::IDropTarget> m_target;
    };
}
