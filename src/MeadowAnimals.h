#pragma once
#include "pch.h"
#include "Scene.h"
#include "Model.h"
#include "Util.h"

namespace ld
{
    class Graphics;

    // Light falling on the animals, derived from the Meadow palette for the current time of day.
    struct MeadowLight
    {
        float night = 0.0f;       // 0 day .. 1 deep night (eyes catch the light)
        float bright = 1.0f;      // overall exposure
        uint32_t tint = 0xFFFFFF; // sky colour the animals are tinted with
        float tintAmount = 0.1f;
        uint32_t warm = 0xFFFFFF; // low sun
        float warmth = 0.0f;
        uint32_t haze = 0xEAF4FA; // atmospheric haze for distant animals
    };

    // Where the animals live: one monitor of the Meadow, with layer containers interleaved between the
    // hills and the grass rows so that depth sorting is free.
    struct MeadowStage
    {
        RECT rc{};                 // monitor in host coordinates
        float visibleBottom = 0;   // monitor-local y of the work-area bottom (taskbar top)
        std::function<float(float)> backY;   // back hill ridge (monitor-local x -> y)
        std::function<float(float)> groundY; // ground ridge
        // back: on the back hills (behind the ground hill); rest: behind grass row 1;
        // mid: behind row 2; front: behind row 3; top: labels above everything.
        wuc::ContainerVisual back{ nullptr }, rest{ nullptr }, mid{ nullptr }, front{ nullptr }, top{ nullptr };
        float parallax[4]{};       // cursor parallax factor of back/rest/mid/front
    };

    // Each Copilot session becomes an animal: working ones busy on the back hills, waiting ones come to
    // the front and ask for attention, finished ones celebrate and lie down in the mid-ground.
    // Rigs are a handful of D2D-drawn parts animated with composition expressions; the CPU only plans
    // walks and swaps behaviours when a session changes state.
    class MeadowAnimals : public std::enable_shared_from_this<MeadowAnimals>
    {
    public:
        MeadowAnimals(SceneContext const& ctx, MeadowStage stage, wuc::CompositionPropertySet const& wind, MeadowLight const& light);
        ~MeadowAnimals();

        void SetLight(MeadowLight const& light, float seconds);
        void Update(SessionsSnapshot const& snapshot);
        void Tick();
        // Host coordinates. Returns the session id of the animal under the point.
        std::optional<std::wstring> Hit(float2 host) const;
        void Hover(std::optional<float2> host);
        // Happy reaction; returns how long to wait (ms) before opening the session.
        int Activate(std::wstring const& id);

        struct Animal;
        enum class Mode { Busy, Attention, Resting, Sleeping };

    private:
        using Step = std::function<float(Animal&)>; // returns seconds until the next step

        Animal& Create(AgentSession const& s);
        void Transition(Animal& a, bool celebrate);
        void Run(Animal& a, std::vector<Step> steps);
        void Next(std::wstring const& id, uint64_t token, std::shared_ptr<std::vector<Step>> steps, size_t index);
        void After(float seconds, std::function<void()> fn);
        void StartLoop(Animal& a);
        void BusyLoop(Animal& a);
        void AttentionLoop(Animal& a);
        void RestLoop(Animal& a, bool sleep);
        void Journey(Animal& a, float x0, float d0, float x1, float d1, float speedK, std::vector<Step>& steps);
        void Place(Animal& a, float x, float d);
        void Reparent(Animal& a, int layer);
        void Freeze(Animal& a);
        void Remove(std::wstring const& id);
        Step Prepare(bool lying);
        Step Celebrate();
        void Happy(Animal& a);
        void HopIn(Animal& a);   // small "just arrived" hop + wave for a newly spawned child animal
        void HopOut(Animal& a);  // small "goodbye" wave played as a child animal despawns
        void Sparkles(Animal& a, int count, bool petals);
        void DrawParts(Animal& a);
        void DrawLabel(Animal& a);
        void ShowLabel(Animal& a);
        void ShowZz(Animal& a, bool show);
        void FollowLayer(Animal& a);
        float SlotFor(Animal const& a, Mode mode) const;
        Mode Desired(AgentSession const& s) const;
        int Escalation(Animal const& a) const;
        uint32_t Lit(uint32_t rgb) const;

        float Y(float d, float x) const;
        float ScaleAt(float d) const;
        float HazeAt(float d) const;
        int LayerOf(float d) const;
        float DepthOf(Mode mode) const;
        wuc::ExpressionAnimation Expr(Animal const& a, wchar_t const* text) const;
        void To(wuc::CompositionObject const& target, wchar_t const* property, float value, float seconds) const;
        void To(wuc::CompositionObject const& target, wchar_t const* property, float3 value, float seconds) const;
        void Keys(wuc::CompositionObject const& target, wchar_t const* property, std::initializer_list<std::pair<float, float>> keys, float seconds, bool loop) const;
        wuc::ContainerVisual LayerVisual(int layer) const;
        float2 PoseAt(Animal const& a) const; // (x, d) right now
        float Px(float dip) const;

        wuc::Compositor m_c{ nullptr };
        Graphics* m_graphics = nullptr;
        winrt::Windows::System::DispatcherQueue m_queue{ nullptr };
        wuc::CompositionPropertySet m_pointer{ nullptr };
        wuc::CompositionPropertySet m_wind{ nullptr };
        wuc::CompositionPropertySet m_light{ nullptr };
        wuc::CompositionColorBrush m_hazeBrush{ nullptr };
        wuc::CompositionBrush m_shadowBrush{ nullptr }, m_sparkle{ nullptr };
        std::vector<wuc::CompositionBrush> m_petals;
        MeadowStage m_stage;
        MeadowLight m_lit{};
        MeadowLight m_drawn{};
        float m_W = 0, m_H = 0, m_U = 0;
        std::map<std::wstring, std::unique_ptr<Animal>> m_animals;
        std::vector<winrt::Windows::System::DispatcherQueueTimer> m_timers;
        std::wstring m_hovered;
        std::optional<float2> m_pointerAt;
        uint64_t m_nextToken = 1;
        float m_tf = 0.62f;       // front row depth (fraction from the ground ridge to the bottom edge)
        wuc::CompositionSurfaceBrush m_zzBrush{ nullptr };
        float2 m_zzSize{};
        Rng m_rng{ 0x5EEDu };
    };
}
