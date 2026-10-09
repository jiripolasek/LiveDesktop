#include "pch.h"
#include "Graphics.h"
#include "Log.h"
#include "Util.h"

namespace ld
{
    D2D1_COLOR_F D2DColor(uint32_t rgb, float alpha)
    {
        return D2D1::ColorF(rgb, alpha);
    }

    void Graphics::Initialize(wuc::Compositor const& compositor)
    {
        m_compositor = compositor;
        D2D1_FACTORY_OPTIONS options{};
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, m_d2dFactory.put_void()));
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwrite.put())));

        winrt::com_ptr<IDWriteFontCollection> fonts;
        m_dwrite->GetSystemFontCollection(fonts.put());
        auto has = [&](wchar_t const* family)
        {
            UINT32 index = 0;
            BOOL exists = FALSE;
            return SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
        };
        m_uiFamily = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        m_displayFamily = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
        m_iconFamily = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        m_monoFamily = has(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
        Log(L"Fonts: ui='%s' display='%s' icons='%s' mono='%s'", m_uiFamily.c_str(), m_displayFamily.c_str(), m_iconFamily.c_str(), m_monoFamily.c_str());

        CreateDevice();
        auto interop = m_compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>();
        winrt::check_hresult(interop->CreateGraphicsDevice(m_d2dDevice.get(),
            reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(m_device))));
        m_replacedToken = m_device.RenderingDeviceReplaced([this](auto&&, auto&&)
        {
            Log(L"Graphics: rendering device replaced - redrawing surfaces");
            m_noise = nullptr;
            if (OnDeviceReplaced) OnDeviceReplaced();
        });
    }

    void Graphics::CreateDevice()
    {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, m_d3d.put(), nullptr, nullptr);
        if (FAILED(hr))
        {
            m_d3d = nullptr;
            winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, m_d3d.put(), nullptr, nullptr));
        }
        m_d2dDevice = nullptr;
        winrt::check_hresult(m_d2dFactory->CreateDevice(m_d3d.as<IDXGIDevice>().get(), m_d2dDevice.put()));
        m_noise = nullptr;
    }

    void Graphics::Shutdown()
    {
        if (m_device)
        {
            m_device.RenderingDeviceReplaced(m_replacedToken);
            m_device.Close();
            m_device = nullptr;
        }
        m_noise = nullptr;
        m_fonts.clear();
        m_d2dDevice = nullptr;
        m_d3d = nullptr;
        m_compositor = nullptr;
    }

    wuc::CompositionDrawingSurface Graphics::CreateSurface(float2 sizePx)
    {
        return m_device.CreateDrawingSurface(winrt::Windows::Foundation::Size(std::max(1.0f, sizePx.x), std::max(1.0f, sizePx.y)),
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
    }

    bool Graphics::Draw(wuc::CompositionDrawingSurface const& surface, float2 sizePx, std::function<void(ID2D1DeviceContext*)> const& draw)
    {
        if (!surface) return false;
        auto size = surface.SizeInt32();
        int w = static_cast<int>(std::ceil(std::max(1.0f, sizePx.x)));
        int h = static_cast<int>(std::ceil(std::max(1.0f, sizePx.y)));
        if (size.Width != w || size.Height != h) surface.Resize({ w, h });

        auto interop = surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT offset{};
        HRESULT hr = interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset);
        if (FAILED(hr))
        {
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET)
            {
                Log(L"Graphics: device lost (0x%08X) - recreating", static_cast<uint32_t>(hr));
                CreateDevice();
                auto deviceInterop = m_device.as<ABI::Windows::UI::Composition::ICompositionGraphicsDeviceInterop>();
                deviceInterop->SetRenderingDevice(m_d2dDevice.get());
            }
            return false;
        }
        dc->SetDpi(96.0f, 96.0f);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        dc->Clear(D2D1::ColorF(0, 0.0f));
        draw(dc.get());
        interop->EndDraw();
        return true;
    }

    std::wstring Graphics::Family(FontRole role) const
    {
        switch (role)
        {
        case FontRole::Display: return m_displayFamily;
        case FontRole::Icons: return m_iconFamily;
        case FontRole::Mono: return m_monoFamily;
        default: return m_uiFamily;
        }
    }

    IDWriteTextFormat* Graphics::Font(FontRole role, float sizeDip, DWRITE_TEXT_ALIGNMENT align)
    {
        float px = sizeDip * m_scale;
        auto key = std::make_tuple(static_cast<int>(role), static_cast<int>(px * 10), static_cast<int>(align));
        if (auto it = m_fonts.find(key); it != m_fonts.end()) return it->second.get();
        DWRITE_FONT_WEIGHT weight = role == FontRole::UiSemibold ? DWRITE_FONT_WEIGHT_SEMI_BOLD :
            role == FontRole::Display ? DWRITE_FONT_WEIGHT_SEMI_LIGHT : DWRITE_FONT_WEIGHT_NORMAL;
        winrt::com_ptr<IDWriteTextFormat> format;
        winrt::check_hresult(m_dwrite->CreateTextFormat(Family(role).c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, px, L"en-us", format.put()));
        format->SetTextAlignment(align);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_fonts[key] = format;
        return format.get();
    }

    winrt::com_ptr<IDWriteTextLayout> Graphics::Layout(std::wstring const& text, IDWriteTextFormat* format, float maxWidth, float maxHeight, bool ellipsis)
    {
        winrt::com_ptr<IDWriteTextLayout> layout;
        winrt::check_hresult(m_dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, std::max(1.0f, maxWidth), maxHeight, layout.put()));
        if (ellipsis)
        {
            DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            winrt::com_ptr<IDWriteInlineObject> sign;
            m_dwrite->CreateEllipsisTrimmingSign(format, sign.put());
            layout->SetTrimming(&trimming, sign.get());
        }
        return layout;
    }

    float2 Graphics::Measure(IDWriteTextLayout* layout)
    {
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return float2(m.widthIncludingTrailingWhitespace, m.height);
    }

    ID2D1BitmapBrush* Graphics::NoiseBrush(ID2D1DeviceContext* dc)
    {
        if (m_noise) return m_noise.get();
        constexpr UINT32 kSize = 128;
        std::vector<uint32_t> pixels(kSize * kSize);
        Rng rng(1234);
        for (auto& p : pixels)
        {
            // Premultiplied white/black specks at very low alpha: reads as acrylic grain.
            float a = rng.Next(0.0f, 1.0f);
            uint8_t alpha = static_cast<uint8_t>(a * a * 18.0f);
            uint8_t v = rng.Next(0.0f, 1.0f) > 0.5f ? alpha : 0;
            p = (static_cast<uint32_t>(alpha) << 24) | (v << 16) | (v << 8) | v;
        }
        winrt::com_ptr<ID2D1Bitmap> bitmap;
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(kSize, kSize), pixels.data(), kSize * 4, &props, bitmap.put()))) return nullptr;
        auto brushProps = D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        dc->CreateBitmapBrush(bitmap.get(), brushProps, m_noise.put());
        m_noiseOwner = dc;
        return m_noise.get();
    }
}
