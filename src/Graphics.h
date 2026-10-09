#pragma once
#include "pch.h"
#include "Scene.h"

namespace ld
{
    enum class FontRole { Ui, UiSemibold, Display, Icons, Mono };

    // Direct2D/DirectWrite rendering into CompositionDrawingSurfaces (labels, cards, timer text).
    // All drawing is in physical pixels; callers scale font sizes with Scale().
    class Graphics
    {
    public:
        void Initialize(wuc::Compositor const& compositor);
        void Shutdown();

        float Scale() const { return m_scale; }
        void SetScale(float scale) { m_scale = scale; }
        float Px(float dip) const { return std::round(dip * m_scale); }

        wuc::CompositionDrawingSurface CreateSurface(float2 sizePx);
        // Resizes (if needed), clears and draws. Returns false if the device was lost (the surfaces will be
        // redrawn after OnDeviceReplaced fires).
        bool Draw(wuc::CompositionDrawingSurface const& surface, float2 sizePx, std::function<void(ID2D1DeviceContext*)> const& draw);

        IDWriteTextFormat* Font(FontRole role, float sizeDip, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
        winrt::com_ptr<IDWriteTextLayout> Layout(std::wstring const& text, IDWriteTextFormat* format, float maxWidth, float maxHeight = 10000.0f, bool ellipsis = true);
        float2 Measure(IDWriteTextLayout* layout);
        ID2D1BitmapBrush* NoiseBrush(ID2D1DeviceContext* dc);
        IDWriteFactory* DWrite() const { return m_dwrite.get(); }

        std::function<void()> OnDeviceReplaced;

    private:
        void CreateDevice();
        std::wstring Family(FontRole role) const;

        wuc::Compositor m_compositor{ nullptr };
        wuc::CompositionGraphicsDevice m_device{ nullptr };
        winrt::event_token m_replacedToken{};
        winrt::com_ptr<ID3D11Device> m_d3d;
        winrt::com_ptr<ID2D1Factory1> m_d2dFactory;
        winrt::com_ptr<ID2D1Device> m_d2dDevice;
        winrt::com_ptr<IDWriteFactory> m_dwrite;
        winrt::com_ptr<ID2D1BitmapBrush> m_noise;
        ID2D1DeviceContext* m_noiseOwner = nullptr;
        std::map<std::tuple<int, int, int>, winrt::com_ptr<IDWriteTextFormat>> m_fonts;
        std::wstring m_uiFamily, m_displayFamily, m_monoFamily, m_iconFamily;
        float m_scale = 1.0f;
    };

    D2D1_COLOR_F D2DColor(uint32_t rgb, float alpha = 1.0f);
}
