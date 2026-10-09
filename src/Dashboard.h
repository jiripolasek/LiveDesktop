#pragma once
#include "pch.h"
#include "Scene.h"
#include "Graphics.h"
#include "Model.h"
#include "Presets.h"

namespace ld
{
    // The Win+D productivity layer: glassy cards drawn with Direct2D into composition surfaces,
    // laid out away from the desktop icons. Input never reaches us directly; main.cpp feeds hover
    // and click positions from the low-level mouse hook (host-pixel coordinates).
    class Dashboard
    {
    public:
        struct Hit
        {
            int card = -1;
            int item = -1;
            std::wstring url;
            bool operator==(Hit const& o) const { return card == o.card && item == o.item; }
        };

        void Initialize(Scene& scene, Graphics& graphics, bool logTailEnabled);
        void Shutdown();
        void SetLayout(SceneLayout const& layout);
        void SetReserved(RECT const& r) { m_reserved = r; } // call before SetLayout
        void SetPreset(ScenePreset const& preset);
        void Show(bool visible);
        bool Visible() const { return m_visible; }

        void Update(SessionsSnapshot const& snapshot);
        void Update(PullRequestsSnapshot const& snapshot);
        void Update(CalendarSnapshot const& snapshot);
        void Update(PortsSnapshot const& snapshot);
        void Update(LogTailSnapshot const& snapshot);
        void Update(WorktreesSnapshot const& snapshot);
        void Tick();
        void RedrawAll();

        void Hover(std::optional<float2> pos);
        bool InsideCard(float2 pos) const;
        std::optional<Hit> HitTest(float2 pos) const;
        void Activate(Hit const& hit);

    private:
        enum class Kind { Agents, PullRequests, Today, Ports, Worktrees, Log };
        enum class RowStyle { Section, Two, One, Mono, Empty };
        struct Row
        {
            RowStyle style = RowStyle::Two;
            std::wstring glyph;
            uint32_t glyphColor = 0xFFFFFF;
            std::optional<uint32_t> dot;
            std::wstring lead;
            std::wstring primary;
            std::wstring secondary;
            std::wstring trailing;
            uint32_t trailingColor = 0xFFFFFF;
            float trailingAlpha = 0.6f;
            std::wstring url;
            bool dim = false;
            bool error = false;
            bool accent = false;
        };
        struct Content
        {
            std::wstring icon, title, meta, footer;
            uint32_t accent = 0x60CDFF;
            uint32_t metaColor = 0xFFFFFF;
            std::vector<Row> rows;
            std::wstring Key() const;
        };
        struct Item
        {
            D2D1_RECT_F rect{}; // card-local px
            std::wstring url;
        };
        struct Card
        {
            Kind kind{};
            wuc::SpriteVisual visual{ nullptr };
            wuc::ShapeVisual flash{ nullptr };
            wuc::CompositionRoundedRectangleGeometry flashGeometry{ nullptr };
            wuc::CompositionDrawingSurface surface{ nullptr };
            float2 pos{}, size{};
            std::vector<Item> items;
            std::wstring key;
        };

        float D(float dip) const { return std::round(dip * m_layout.scale); }
        Content Build(Kind kind) const;
        bool Draw(Card& card, bool force);
        float RowHeight(RowStyle style) const;
        void Relayout(bool animate);
        RECT FreeRegion() const;
        std::vector<Kind> ActiveKinds() const;
        void Changed(Kind kind);
        Card* Find(Kind kind);
        void ApplyVisibility();
        void FlashCard(Card& card, uint32_t rgb);

        Scene* m_scene = nullptr;
        Graphics* m_graphics = nullptr;
        wuc::Compositor m_c{ nullptr };
        SceneLayout m_layout;
        RECT m_reserved{};
        ScenePreset m_preset;
        bool m_logTail = false;
        bool m_requested = false;
        bool m_visible = false;
        uint32_t m_showSerial = 0;
        float m_cardW = 0;

        wuc::ContainerVisual m_root{ nullptr };
        wuc::ShapeVisual m_hover{ nullptr };
        wuc::CompositionRoundedRectangleGeometry m_hoverGeometry{ nullptr };
        std::optional<Hit> m_hovered;
        std::vector<Card> m_cards;

        SessionsSnapshot m_sessions;
        PullRequestsSnapshot m_prs;
        CalendarSnapshot m_calendar;
        PortsSnapshot m_ports;
        LogTailSnapshot m_log;
        WorktreesSnapshot m_trees;
        bool m_haveSessions = false, m_havePrs = false, m_haveCalendar = false, m_havePorts = false, m_haveLog = false, m_haveTrees = false;
        TimePoint m_prsAt{};
        uint64_t m_logErrorSerial = 0;
    };
}
