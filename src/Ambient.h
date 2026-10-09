#pragma once
#include "pch.h"
#include "Scene.h"
#include "Graphics.h"
#include "Model.h"
#include "Presets.h"
#include "Visuals.h"

namespace ld
{
    // The always-on ambient status layer: agent orbs, CI tide, review fireflies, system pulse,
    // next-meeting horizon and the focus scene. Everything animates in the compositor; the UI thread
    // only touches it when a snapshot changes or on a slow tick (label ages, timer minutes).
    class Ambient
    {
    public:
        void Initialize(Scene& scene, Graphics& graphics, int focusMinutes);
        void Shutdown();
        void SetLayout(SceneLayout const& layout);
        void SetPreset(ScenePreset const& preset);
        void SetDashboardVisible(bool visible);
        // The wallpaper effect draws the agents itself (Meadow animals): hide the orbs.
        void SetOrbsHidden(bool hidden);

        void Update(SessionsSnapshot const& snapshot);
        void Update(PullRequestsSnapshot const& snapshot);
        void Update(SystemSnapshot const& snapshot);
        void Update(CalendarSnapshot const& snapshot);
        void Tick();       // every few seconds while not paused
        void RedrawAll();  // after a graphics device loss

    private:
        struct GlowStops
        {
            wuc::CompositionRadialGradientBrush brush{ nullptr };
            wuc::CompositionColorGradientStop stops[3]{ nullptr, nullptr, nullptr };
        };
        struct Orb
        {
            AgentSession session;
            bool known = false;
            wuc::ContainerVisual root{ nullptr }, floater{ nullptr }, bobber{ nullptr };
            wuc::SpriteVisual halo{ nullptr }, core{ nullptr }, label{ nullptr };
            GlowStops haloBrush, coreBrush;
            wuc::CompositionDrawingSurface surface{ nullptr };
            std::wstring labelKey;
        };
        struct Firefly
        {
            wuc::ContainerVisual root{ nullptr };
            bool alive = true;
        };

        float D(float dip) const { return std::round(dip * m_layout.scale); }
        GlowStops MakeGlow(Color color, float midAlpha);
        void SetGlowColor(GlowStops& glow, Color color, float midAlpha, float seconds, float delay = 0.0f);
        void AnimateScalar(wuc::CompositionObject const& target, wchar_t const* property, float to, float seconds, float delay = 0.0f);
        void Fade(wuc::Visual const& visual, float to, float seconds, float delay = 0.0f) { AnimateScalar(visual, L"Opacity", to, seconds, delay); }
        void RemoveAfterFade(wuc::ContainerVisual const& parent, wuc::Visual const& visual, float seconds);

        void BuildStatic();
        void LayoutStatic();
        void UpdateOrbs();
        Orb CreateOrb(AgentSession const& session);
        void ApplyStatus(Orb& orb, std::optional<AgentStatus> previous);
        void DrawLabel(Orb& orb, bool force);
        float2 OrbSlot(size_t index, size_t count) const;
        void UpdateTide();
        void UpdateFireflies();
        void UpdateHorizon(bool force);
        void UpdateFocus(bool force);
        void ApplyPreset();

        Scene* m_scene = nullptr;
        Graphics* m_graphics = nullptr;
        wuc::Compositor m_c{ nullptr };
        SceneLayout m_layout;
        ScenePreset m_preset;
        bool m_dashboard = false;
        bool m_orbsHidden = false;
        int m_focusMinutes = 25;
        Rng m_rng;

        wuc::ContainerVisual m_root{ nullptr };
        wuc::SpriteVisual m_veil{ nullptr };
        // System pulse
        wuc::ContainerVisual m_dust{ nullptr };
        wuc::CompositionPropertySet m_dustProps{ nullptr };
        wuc::AnimationController m_dustController{ nullptr };
        wuc::SpriteVisual m_water{ nullptr }, m_waterLine{ nullptr }, m_ember{ nullptr };
        GlowStops m_emberBrush;
        wuc::CompositionColorGradientStop m_waterStop{ nullptr };
        // CI tide + review fireflies
        wuc::ContainerVisual m_tide{ nullptr };
        wuc::SpriteVisual m_tideSprite{ nullptr };
        wuc::CompositionColorGradientStop m_tideStops[2]{ nullptr, nullptr };
        int m_tideState = -1;
        wuc::ContainerVisual m_fireflyLayer{ nullptr };
        std::map<std::wstring, Firefly> m_fireflies;
        // Orbs
        wuc::ContainerVisual m_orbLayer{ nullptr };
        std::map<std::wstring, Orb> m_orbs;
        // Horizon
        wuc::ContainerVisual m_horizon{ nullptr };
        wuc::SpriteVisual m_horizonGlow{ nullptr }, m_horizonLine{ nullptr }, m_horizonLabel{ nullptr };
        GlowStops m_horizonBrush;
        wuc::CompositionColorGradientStop m_horizonLineStop{ nullptr };
        wuc::CompositionDrawingSurface m_horizonSurface{ nullptr };
        std::wstring m_horizonKey;
        // Focus timer
        wuc::ContainerVisual m_focus{ nullptr };
        wuc::ShapeVisual m_focusRing{ nullptr };
        wuc::CompositionEllipseGeometry m_focusArc{ nullptr };
        wuc::SpriteVisual m_focusText{ nullptr };
        wuc::CompositionDrawingSurface m_focusSurface{ nullptr };
        std::wstring m_focusKey;
        TimePoint m_focusStart{};

        SessionsSnapshot m_sessions;
        PullRequestsSnapshot m_prs;
        SystemSnapshot m_system;
        CalendarSnapshot m_calendar;
        bool m_haveSessions = false, m_havePrs = false, m_haveCalendar = false;
    };
}
