#pragma once
#include "pch.h"
#include "Scene.h"
#include "Graphics.h"
#include "Presets.h"
#include "Clipboard.h"

namespace ld
{
    // The clipboard shelf: a small stack of glass cards in the bottom-right corner of the primary
    // work area. Hovering fans it out; clicking a card puts it back on the clipboard; dragging files
    // or text onto it keeps them (main.cpp shows a transient OLE drop window over DropZone()).
    // Like the dashboard, input arrives from the low-level mouse hook in host-pixel coordinates.
    class ClipboardStack
    {
    public:
        void Initialize(Scene& scene, Graphics& graphics);
        void Shutdown();
        void SetLayout(SceneLayout const& layout);
        void SetPreset(ScenePreset const& preset);
        void SetEnabled(bool enabled);
        bool Enabled() const { return m_enabled; }
        void Update(ClipboardSnapshot const& snapshot);
        void Tick();
        void RedrawAll();

        void Hover(std::optional<float2> pos);
        // Forces the fanned state (scripted demos); nullopt = follow the pointer.
        void ForceFan(std::optional<bool> fan);
        bool Contains(float2 pos) const;
        std::optional<uint64_t> HitTest(float2 pos) const;
        void Activate(uint64_t id);
        void SetDropHover(bool active);
        void Dropped(std::wstring const& what);
        RECT Footprint() const; // collapsed stack incl. caption (the dashboard keeps clear of it)
        RECT DropZone() const;  // host px

    private:
        struct Card
        {
            uint64_t id = 0;
            ClipItem item;
            wuc::SpriteVisual visual{ nullptr };
            wuc::CompositionDrawingSurface surface{ nullptr };
            wuc::ShapeVisual flash{ nullptr };
            std::wstring key;
            bool removing = false;
        };

        float D(float dip) const { return std::round(dip * m_layout.scale); }
        void Place();
        void Arrange(bool animate);
        void Reorder();
        bool DrawCard(Card& card, bool force);
        void DrawCaption();
        void DrawToast(std::wstring const& text);
        void DrawDropZone();
        void SetFanned(bool fanned);
        void ShowHover(std::optional<uint64_t> id);
        D2D1_RECT_F CardRect(size_t index, bool fanned) const; // host px
        size_t VisibleCount() const;
        bool Shown() const { return m_enabled && m_layout.size.x > 0; }

        Scene* m_scene = nullptr;
        Graphics* m_graphics = nullptr;
        wuc::Compositor m_c{ nullptr };
        SceneLayout m_layout;
        ScenePreset m_preset;
        bool m_enabled = true;
        bool m_fanned = false;
        std::optional<bool> m_forced;
        bool m_dropHover = false;
        bool m_mock = false;
        float2 m_base{};   // top-left of the front card (host px)
        float m_w = 0, m_h = 0;

        wuc::ContainerVisual m_root{ nullptr };
        wuc::ContainerVisual m_cardsRoot{ nullptr };
        wuc::SpriteVisual m_caption{ nullptr };
        wuc::CompositionDrawingSurface m_captionSurface{ nullptr };
        std::wstring m_captionKey;
        wuc::SpriteVisual m_toast{ nullptr };
        wuc::CompositionDrawingSurface m_toastSurface{ nullptr };
        wuc::SpriteVisual m_drop{ nullptr };
        wuc::CompositionDrawingSurface m_dropSurface{ nullptr };
        wuc::ShapeVisual m_hover{ nullptr };
        wuc::CompositionRoundedRectangleGeometry m_hoverGeometry{ nullptr };
        std::optional<uint64_t> m_hovered;
        std::vector<Card> m_cards; // snapshot order (newest first), plus cards fading out
    };
}
