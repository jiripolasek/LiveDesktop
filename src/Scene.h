#pragma once
#include "pch.h"

namespace ld
{
    namespace wuc = winrt::Windows::UI::Composition;
    using winrt::Windows::Foundation::Numerics::float2;
    using winrt::Windows::Foundation::Numerics::float3;

    class Graphics;

    struct SceneContext
    {
        wuc::Compositor compositor{ nullptr };
        Graphics* graphics = nullptr; // D2D/DirectWrite surfaces (may be null before initialization)
        float timeOverride = -1.0f;   // local hour [0,24) forced via --send time:..., < 0 = real clock
        // Shared pointer state driven from the mouse hook:
        //   Position (Vector3, host pixels), Strength (0..1, 1 while hovering the desktop), Radius (px)
        wuc::CompositionPropertySet pointer{ nullptr };
        float2 size{};
        std::vector<RECT> monitors; // host coordinates
    };

    class Effect
    {
    public:
        virtual ~Effect() = default;
        virtual const wchar_t* Name() const = 0;
        // Populate `layer` with visuals/animations. Everything should be composition-driven.
        virtual void Build(SceneContext const& ctx, wuc::ContainerVisual const& layer) = 0;
        virtual void OnClick(SceneContext const& ctx, wuc::ContainerVisual const& layer, float2 pos) = 0;
        // Slow periodic update (every few seconds, not per frame). `jump` = the clock was overridden.
        virtual void Tick(SceneContext const&, wuc::ContainerVisual const&, bool /*jump*/) {}
    };

    std::vector<std::unique_ptr<Effect>> CreateEffects();
    std::unique_ptr<Effect> CreateMeadowEffect();
    // Local hour for a named Meadow phase ("dawn", "golden", "night", "HH:MM"...). -1 = auto, -2 = invalid.
    float MeadowPhaseHour(std::wstring const& name);

    class Scene
    {
    public:
        void Initialize();
        void AttachToWindow(HWND hwnd, float2 size, std::vector<RECT> monitors);
        void DetachFromWindow();
        void Resize(float2 size, std::vector<RECT> monitors);
        void NextEffect();
        void SetEffect(size_t index);
        size_t EffectCount() const { return m_effects.size(); }
        const wchar_t* CurrentEffectName() const;
        void PointerMoved(float2 pos, bool overDesktop);
        void Click(float2 pos);
        void SetPaused(bool paused);
        bool Paused() const { return m_paused; }
        void SetGraphics(Graphics* graphics) { m_ctx.graphics = graphics; }
        void Tick();
        // Rebuilds the current effect in place (e.g. after the graphics device was replaced).
        void Refresh() { Rebuild(false); }
        void SetTimeOverride(float hour);
        void Shutdown();

        wuc::Compositor Compositor() const { return m_ctx.compositor; }
        SceneContext const& Context() const { return m_ctx; }
        // Above the wallpaper effect: ambient layer and dashboard live here and survive effect switches.
        wuc::ContainerVisual Overlay() const { return m_overlay; }
        // The wallpaper effect container (used to tint/dim the whole effect, e.g. focus scene).
        wuc::ContainerVisual EffectRoot() const { return m_effectRoot; }
        winrt::Windows::System::DispatcherQueue Queue() const { return m_dispatcher ? m_dispatcher.DispatcherQueue() : nullptr; }
        std::function<void(float2)> OnResized;

    private:
        void Rebuild(bool crossfade);

        winrt::Windows::System::DispatcherQueueController m_dispatcher{ nullptr };
        SceneContext m_ctx;
        wuc::ContainerVisual m_root{ nullptr };
        wuc::ContainerVisual m_effectRoot{ nullptr };
        wuc::ContainerVisual m_overlay{ nullptr };
        wuc::ContainerVisual m_layer{ nullptr };
        wuc::Desktop::DesktopWindowTarget m_target{ nullptr };
        wuc::Vector3KeyFrameAnimation m_followAnimation{ nullptr };
        wuc::ScalarKeyFrameAnimation m_strengthAnimation{ nullptr };
        std::vector<std::unique_ptr<Effect>> m_effects;
        size_t m_current = 0;
        bool m_pointerOver = false;
        bool m_paused = false;
    };
}
