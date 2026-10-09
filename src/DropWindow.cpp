#include "pch.h"
#include "DropWindow.h"
#include "Log.h"

namespace ld
{
    namespace
    {
        constexpr wchar_t kDropClass[] = L"LiveDesktop.Drop";

        bool HasFormat(::IDataObject* data, CLIPFORMAT format)
        {
            FORMATETC fe{ format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            return data && data->QueryGetData(&fe) == S_OK;
        }
    }

    struct DropTarget : winrt::implements<DropTarget, ::IDropTarget>
    {
        explicit DropTarget(DropWindow* owner) : m_owner(owner)
        {
            m_helper = winrt::try_create_instance<::IDropTargetHelper>(CLSID_DragDropHelper);
        }

        HRESULT __stdcall DragEnter(::IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
        {
            m_accept = HasFormat(data, CF_HDROP) || HasFormat(data, CF_UNICODETEXT);
            *effect = Effect(*effect);
            POINT p{ pt.x, pt.y };
            if (m_helper) m_helper->DragEnter(m_owner->m_hwnd, data, &p, *effect);
            m_owner->m_inDrag = true;
            if (m_accept && m_owner->OnDragHover) m_owner->OnDragHover(true);
            return S_OK;
        }

        HRESULT __stdcall DragOver(DWORD, POINTL pt, DWORD* effect) override
        {
            *effect = Effect(*effect);
            POINT p{ pt.x, pt.y };
            if (m_helper) m_helper->DragOver(&p, *effect);
            return S_OK;
        }

        HRESULT __stdcall DragLeave() override
        {
            if (m_helper) m_helper->DragLeave();
            m_owner->m_inDrag = false;
            if (m_owner->OnDragHover) m_owner->OnDragHover(false);
            m_owner->Hide();
            return S_OK;
        }

        HRESULT __stdcall Drop(::IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
        {
            *effect = Effect(*effect);
            POINT p{ pt.x, pt.y };
            if (m_helper) m_helper->Drop(data, &p, *effect);
            m_owner->m_inDrag = false;
            std::vector<std::wstring> files;
            std::wstring text;
            if (*effect != DROPEFFECT_NONE)
            {
                STGMEDIUM medium{};
                FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
                if (SUCCEEDED(data->GetData(&fe, &medium)))
                {
                    if (auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal)))
                    {
                        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                        for (UINT i = 0; i < count && i < 512; ++i)
                        {
                            UINT len = DragQueryFileW(drop, i, nullptr, 0);
                            std::wstring path(len, L'\0');
                            DragQueryFileW(drop, i, path.data(), len + 1);
                            files.push_back(std::move(path));
                        }
                        GlobalUnlock(medium.hGlobal);
                    }
                    ReleaseStgMedium(&medium);
                }
                if (files.empty())
                {
                    fe.cfFormat = CF_UNICODETEXT;
                    if (SUCCEEDED(data->GetData(&fe, &medium)))
                    {
                        if (auto chars = static_cast<wchar_t const*>(GlobalLock(medium.hGlobal)))
                        {
                            size_t max = GlobalSize(medium.hGlobal) / sizeof(wchar_t);
                            text.assign(chars, wcsnlen(chars, std::min<size_t>(max, 2'000'000)));
                            GlobalUnlock(medium.hGlobal);
                        }
                        ReleaseStgMedium(&medium);
                    }
                }
            }
            if (m_owner->OnDragHover) m_owner->OnDragHover(false);
            m_owner->Hide();
            if ((!files.empty() || !text.empty()) && m_owner->OnDrop) m_owner->OnDrop(std::move(files), std::move(text));
            return S_OK;
        }

    private:
        DWORD Effect(DWORD allowed) const
        {
            return m_accept && (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }

        DropWindow* m_owner;
        winrt::com_ptr<::IDropTargetHelper> m_helper;
        bool m_accept = false;
    };

    bool DropWindow::Create(HINSTANCE instance)
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kDropClass;
        RegisterClassExW(&wc);
        m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, kDropClass, L"LiveDesktop drop",
            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
        if (!m_hwnd)
        {
            Log(L"Drop window: CreateWindowEx failed (%lu)", GetLastError());
            return false;
        }
        // Practically invisible, but still hit-testable (alpha 0 would let input fall through).
        SetLayeredWindowAttributes(m_hwnd, 0, 1, LWA_ALPHA);
        m_target = winrt::make<DropTarget>(this).as<::IDropTarget>();
        HRESULT hr = RegisterDragDrop(m_hwnd, m_target.get());
        if (FAILED(hr))
        {
            Log(L"Drop window: RegisterDragDrop failed 0x%08X", hr);
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
            m_target = nullptr;
            return false;
        }
        Log(L"Drop window ready (HWND 0x%p)", m_hwnd);
        return true;
    }

    void DropWindow::Destroy()
    {
        if (!m_hwnd) return;
        RevokeDragDrop(m_hwnd);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        m_target = nullptr;
        m_visible = false;
    }

    void DropWindow::Show(RECT const& r)
    {
        if (!m_hwnd) return;
        SetWindowPos(m_hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!m_visible) Log(L"Drop window shown at (%ld,%ld)-(%ld,%ld)", r.left, r.top, r.right, r.bottom);
        m_visible = true;
    }

    void DropWindow::Hide()
    {
        if (!m_hwnd || !m_visible) return;
        ShowWindow(m_hwnd, SW_HIDE);
        m_visible = false;
        m_inDrag = false;
    }

    LRESULT CALLBACK DropWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTCLIENT;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
