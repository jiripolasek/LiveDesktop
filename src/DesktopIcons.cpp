#include "pch.h"
#include "DesktopIcons.h"
#include "Log.h"

namespace ld
{
    bool DesktopIcons::Ensure()
    {
        if (m_view && m_listView && IsWindow(m_listView)) return true;
        Reset();
        try
        {
            auto shellWindows = winrt::create_instance<IShellWindows>(CLSID_ShellWindows, CLSCTX_LOCAL_SERVER);

            VARIANT location{};
            location.vt = VT_I4;
            location.lVal = CSIDL_DESKTOP;
            VARIANT empty{};
            long hwnd = 0;
            winrt::com_ptr<IDispatch> dispatch;
            winrt::check_hresult(shellWindows->FindWindowSW(&location, &empty, SWC_DESKTOP, &hwnd,
                SWFO_NEEDDISPATCH, dispatch.put()));
            if (!dispatch) return false;

            winrt::com_ptr<IShellBrowser> browser;
            winrt::check_hresult(dispatch.as<IServiceProvider>()->QueryService(SID_STopLevelBrowser,
                IID_PPV_ARGS(browser.put())));
            winrt::com_ptr<IShellView> view;
            winrt::check_hresult(browser->QueryActiveShellView(view.put()));

            HWND defView = nullptr;
            winrt::check_hresult(view->GetWindow(&defView));
            m_listView = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
            m_view = view.as<IFolderView>();
            return m_listView != nullptr;
        }
        catch (winrt::hresult_error const& e)
        {
            if (!m_loggedFailure)
            {
                Log(L"Desktop IFolderView unavailable (0x%08X); clicks on icons will also ripple", e.code().value);
                m_loggedFailure = true;
            }
            Reset();
            return false;
        }
    }

    bool DesktopIcons::HitTestIcon(POINT screenPt)
    {
        if (!Ensure()) return false;
        try
        {
            POINT pt = screenPt;
            ScreenToClient(m_listView, &pt);

            POINT spacing{};
            winrt::check_hresult(m_view->GetSpacing(&spacing));
            if (spacing.x <= 0 || spacing.y <= 0) return false;

            winrt::com_ptr<IEnumIDList> items;
            winrt::check_hresult(m_view->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(items.put())));
            if (!items) return false;

            PITEMID_CHILD pidl = nullptr;
            bool hit = false;
            while (!hit && items->Next(1, &pidl, nullptr) == S_OK)
            {
                POINT pos{};
                if (SUCCEEDED(m_view->GetItemPosition(pidl, &pos)))
                {
                    hit = pt.x >= pos.x && pt.x < pos.x + spacing.x && pt.y >= pos.y && pt.y < pos.y + spacing.y;
                }
                CoTaskMemFree(pidl);
            }
            return hit;
        }
        catch (winrt::hresult_error const&)
        {
            Reset();
            return false;
        }
    }
}
