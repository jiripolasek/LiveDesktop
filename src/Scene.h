#pragma once
#include "pch.h"

namespace ld
{
    namespace wuc = winrt::Windows::UI::Composition;
    using winrt::Windows::Foundation::Numerics::float2;
    using winrt::Windows::Foundation::Numerics::float3;

    struct SceneContext
    {
        wuc::Compositor compositor{ nullptr };
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
    };

    std::vector<std::unique_ptr<Effect>> CreateEffects();

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
        void Shutdown();

    private:
        void Rebuild(bool crossfade);

        winrt::Windows::System::DispatcherQueueController m_dispatcher{ nullptr };
        SceneContext m_ctx;
        wuc::ContainerVisual m_root{ nullptr };
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
