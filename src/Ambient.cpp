#include "pch.h"
#include "Ambient.h"
#include "Log.h"

using namespace std::chrono_literals;

namespace ld
{
    namespace
    {
        constexpr size_t kDustCount = 34;
        constexpr size_t kMaxFireflies = 8;

        Color WithAlpha(Color c, float a)
        {
            c.A = static_cast<uint8_t>(std::clamp(c.A * a, 0.0f, 255.0f));
            return c;
        }

        Color Lerp(Color a, Color b, float t)
        {
            auto mix = [t](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + (y - x) * t); };
            return Color{ mix(a.A, b.A), mix(a.R, b.R), mix(a.G, b.G), mix(a.B, b.B) };
        }

        wuc::CompositionEasingFunction Smooth(wuc::Compositor const& c)
        {
            return c.CreateCubicBezierEasingFunction(float2(0.25f, 0.1f), float2(0.25f, 1.0f));
        }

        wuc::ImplicitAnimationCollection MakeImplicit(wuc::Compositor const& c, float seconds, bool offset, bool size, bool opacity)
        {
            auto collection = c.CreateImplicitAnimationCollection();
            auto ease = Smooth(c);
            if (offset)
            {
                auto a = c.CreateVector3KeyFrameAnimation();
                a.Target(L"Offset");
                a.InsertExpressionKeyFrame(1.0f, L"this.FinalValue", ease);
                a.Duration(Seconds(seconds));
                collection.Insert(L"Offset", a);
            }
            if (size)
            {
                auto a = c.CreateVector2KeyFrameAnimation();
                a.Target(L"Size");
                a.InsertExpressionKeyFrame(1.0f, L"this.FinalValue", ease);
                a.Duration(Seconds(seconds));
                collection.Insert(L"Size", a);
            }
            if (opacity)
            {
                auto a = c.CreateScalarKeyFrameAnimation();
                a.Target(L"Opacity");
                a.InsertExpressionKeyFrame(1.0f, L"this.FinalValue", ease);
                a.Duration(Seconds(seconds));
                collection.Insert(L"Opacity", a);
            }
            return collection;
        }

        std::wstring Age(TimePoint from)
        {
            return FormatAge(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - from));
        }
    }

    // ---------------------------------------------------------------- helpers

    Ambient::GlowStops Ambient::MakeGlow(Color color, float midAlpha)
    {
        GlowStops g;
        g.brush = m_c.CreateRadialGradientBrush();
        g.stops[0] = m_c.CreateColorGradientStop(0.0f, color);
        g.stops[1] = m_c.CreateColorGradientStop(0.4f, WithAlpha(color, midAlpha));
        g.stops[2] = m_c.CreateColorGradientStop(1.0f, WithAlpha(color, 0.0f));
        for (auto const& s : g.stops) g.brush.ColorStops().Append(s);
        return g;
    }

    void Ambient::SetGlowColor(GlowStops& glow, Color color, float midAlpha, float seconds, float delay)
    {
        Color targets[3] = { color, WithAlpha(color, midAlpha), WithAlpha(color, 0.0f) };
        for (int i = 0; i < 3; ++i)
        {
            auto a = m_c.CreateColorKeyFrameAnimation();
            a.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
            a.InsertKeyFrame(1.0f, targets[i], Smooth(m_c));
            a.Duration(Seconds(std::max(0.05f, seconds)));
            a.DelayTime(Seconds(delay));
            glow.stops[i].StartAnimation(L"Color", a);
        }
    }

    void Ambient::AnimateScalar(wuc::CompositionObject const& target, wchar_t const* property, float to, float seconds, float delay)
    {
        auto a = m_c.CreateScalarKeyFrameAnimation();
        a.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
        a.InsertKeyFrame(1.0f, to, Smooth(m_c));
        a.Duration(Seconds(std::max(0.05f, seconds)));
        a.DelayTime(Seconds(delay));
        target.StartAnimation(property, a);
    }

    void Ambient::RemoveAfterFade(wuc::ContainerVisual const& parent, wuc::Visual const& visual, float seconds)
    {
        auto batch = m_c.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        Fade(visual, 0.0f, seconds);
        batch.End();
        batch.Completed([parent, visual](auto&&, auto&&)
        {
            parent.Children().Remove(visual);
            visual.Close();
        });
    }

    // ---------------------------------------------------------------- lifetime

    void Ambient::Initialize(Scene& scene, Graphics& graphics, int focusMinutes)
    {
        m_scene = &scene;
        m_graphics = &graphics;
        m_c = scene.Compositor();
        m_focusMinutes = std::max(1, focusMinutes);
        m_focusStart = Clock::now();
        m_root = m_c.CreateContainerVisual();
        scene.Overlay().Children().InsertAtBottom(m_root);
        BuildStatic();
    }

    void Ambient::Shutdown()
    {
        if (m_dustController) m_dustController.Pause();
        m_orbs.clear();
        m_fireflies.clear();
        *this = Ambient{};
    }

    void Ambient::BuildStatic()
    {
        auto const& c = m_c;

        // Focus veil (dims the wallpaper effect underneath).
        m_veil = c.CreateSpriteVisual();
        m_veil.Brush(c.CreateColorBrush(Argb(255, 0x05080F)));
        m_veil.Opacity(0.0f);
        m_root.Children().InsertAtTop(m_veil);

        // Memory "water level": a faint tinted band rising from the bottom of the work area.
        auto waterBrush = c.CreateLinearGradientBrush();
        waterBrush.StartPoint(float2(0.0f, 0.0f));
        waterBrush.EndPoint(float2(0.0f, 1.0f));
        waterBrush.ColorStops().Append(c.CreateColorGradientStop(0.0f, Argb(0, 0x3AA0C8)));
        m_waterStop = c.CreateColorGradientStop(1.0f, Argb(0x26, 0x3AA0C8));
        waterBrush.ColorStops().Append(m_waterStop);
        m_water = c.CreateSpriteVisual();
        m_water.Brush(waterBrush);
        m_water.ImplicitAnimations(MakeImplicit(c, 4.0f, true, true, false));
        m_root.Children().InsertAtTop(m_water);

        auto lineBrush = c.CreateLinearGradientBrush();
        lineBrush.StartPoint(float2(0.0f, 0.0f));
        lineBrush.EndPoint(float2(1.0f, 0.0f));
        lineBrush.ColorStops().Append(c.CreateColorGradientStop(0.0f, Argb(0, 0xBFE8FF)));
        lineBrush.ColorStops().Append(c.CreateColorGradientStop(0.5f, Argb(0x2A, 0xBFE8FF)));
        lineBrush.ColorStops().Append(c.CreateColorGradientStop(1.0f, Argb(0, 0xBFE8FF)));
        m_waterLine = c.CreateSpriteVisual();
        m_waterLine.Brush(lineBrush);
        m_waterLine.ImplicitAnimations(MakeImplicit(c, 4.0f, true, true, false));
        m_waterLine.Opacity(0.7f);
        m_waterLine.StartAnimation(L"Opacity", Oscillate(c, 0.35f, 0.9f, 7.0f));
        m_root.Children().InsertAtTop(m_waterLine);

        // GPU ember: warm glow from below when the GPU is busy.
        m_emberBrush = MakeGlow(Argb(0xB0, 0xFF6A2B), 0.28f);
        m_ember = c.CreateSpriteVisual();
        m_ember.Brush(m_emberBrush.brush);
        m_ember.Opacity(0.0f);
        m_ember.ImplicitAnimations(MakeImplicit(c, 3.0f, false, false, true));
        m_root.Children().InsertAtTop(m_ember);

        // CI tide: soft red (failing) / amber (pending) glow rising from the bottom edge.
        m_tide = c.CreateContainerVisual();
        m_tide.Opacity(0.0f);
        m_tide.Scale(float3(1.0f, 0.3f, 1.0f));
        auto tideBrush = c.CreateLinearGradientBrush();
        tideBrush.StartPoint(float2(0.0f, 0.0f));
        tideBrush.EndPoint(float2(0.0f, 1.0f));
        tideBrush.ColorStops().Append(c.CreateColorGradientStop(0.0f, Argb(0, palette::Failure)));
        m_tideStops[0] = c.CreateColorGradientStop(0.55f, Argb(0x22, palette::Failure));
        m_tideStops[1] = c.CreateColorGradientStop(1.0f, Argb(0x88, palette::Failure));
        tideBrush.ColorStops().Append(m_tideStops[0]);
        tideBrush.ColorStops().Append(m_tideStops[1]);
        m_tideSprite = c.CreateSpriteVisual();
        m_tideSprite.Brush(tideBrush);
        m_tide.Children().InsertAtTop(m_tideSprite);
        m_root.Children().InsertAtTop(m_tide);

        // Dust: CPU load drives the playback rate of one shared "Phase" clock.
        m_dust = c.CreateContainerVisual();
        m_dustProps = c.CreatePropertySet();
        m_dustProps.InsertScalar(L"Phase", 0.0f);
        m_dustProps.InsertScalar(L"Left", 0.0f);
        m_dustProps.InsertScalar(L"Top", 0.0f);
        m_dustProps.InsertScalar(L"W", 1.0f);
        m_dustProps.InsertScalar(L"H", 1.0f);
        auto phase = c.CreateScalarKeyFrameAnimation();
        phase.InsertKeyFrame(0.0f, 0.0f);
        phase.InsertKeyFrame(1.0f, 86400.0f, c.CreateLinearEasingFunction());
        phase.Duration(Seconds(86400.0f));
        phase.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
        m_dustProps.StartAnimation(L"Phase", phase);
        m_dustController = m_dustProps.TryGetAnimationController(L"Phase");

        auto dustBrush = GlowBrush(c, Argb(0xC8, 0xEAF4FF), 0.35f, 0.3f);
        for (size_t i = 0; i < kDustCount; ++i)
        {
            auto mote = c.CreateSpriteVisual();
            float size = m_rng.Next(5.0f, 11.0f);
            mote.Size(float2(size, size));
            mote.AnchorPoint(float2(0.5f, 0.5f));
            mote.Brush(dustBrush);
            mote.Opacity(m_rng.Next(0.18f, 0.55f));
            auto expr = c.CreateExpressionAnimation(
                L"Vector3(p.Left + Mod(bx * p.W + p.Phase * wx, p.W) + Sin(p.Phase * fw + ph) * amp,"
                L" p.Top + p.H - Mod(p.Phase * vy + by * p.H, p.H), 0)");
            expr.SetReferenceParameter(L"p", m_dustProps);
            expr.SetScalarParameter(L"bx", m_rng.Next(0.0f, 1.0f));
            expr.SetScalarParameter(L"by", m_rng.Next(0.0f, 1.0f));
            expr.SetScalarParameter(L"wx", m_rng.Next(2.0f, 7.0f));
            expr.SetScalarParameter(L"vy", m_rng.Next(5.0f, 14.0f));
            expr.SetScalarParameter(L"fw", m_rng.Next(0.05f, 0.18f));
            expr.SetScalarParameter(L"ph", m_rng.Next(0.0f, 6.28f));
            expr.SetScalarParameter(L"amp", m_rng.Next(8.0f, 30.0f));
            mote.StartAnimation(L"Offset", expr);
            m_dust.Children().InsertAtTop(mote);
        }
        m_root.Children().InsertAtTop(m_dust);

        m_fireflyLayer = c.CreateContainerVisual();
        m_root.Children().InsertAtTop(m_fireflyLayer);
        m_orbLayer = c.CreateContainerVisual();
        m_root.Children().InsertAtTop(m_orbLayer);

        // Next-meeting horizon.
        m_horizon = c.CreateContainerVisual();
        m_horizon.Opacity(0.0f);
        m_horizonBrush = MakeGlow(Argb(0x70, palette::Meeting), 0.22f);
        m_horizonGlow = c.CreateSpriteVisual();
        m_horizonGlow.Brush(m_horizonBrush.brush);
        m_horizonGlow.AnchorPoint(float2(0.5f, 0.5f));
        auto hl = c.CreateLinearGradientBrush();
        hl.StartPoint(float2(0.0f, 0.0f));
        hl.EndPoint(float2(1.0f, 0.0f));
        hl.ColorStops().Append(c.CreateColorGradientStop(0.0f, Argb(0, palette::Meeting)));
        m_horizonLineStop = c.CreateColorGradientStop(0.5f, Argb(0xA0, palette::Meeting));
        hl.ColorStops().Append(m_horizonLineStop);
        hl.ColorStops().Append(c.CreateColorGradientStop(1.0f, Argb(0, palette::Meeting)));
        m_horizonLine = c.CreateSpriteVisual();
        m_horizonLine.Brush(hl);
        m_horizonLine.AnchorPoint(float2(0.5f, 0.5f));
        m_horizonLabel = c.CreateSpriteVisual();
        m_horizonLabel.AnchorPoint(float2(0.5f, 1.0f));
        m_horizon.Children().InsertAtTop(m_horizonGlow);
        m_horizon.Children().InsertAtTop(m_horizonLine);
        m_horizon.Children().InsertAtTop(m_horizonLabel);
        m_root.Children().InsertAtTop(m_horizon);

        // Focus timer.
        m_focus = c.CreateContainerVisual();
        m_focus.Opacity(0.0f);
        m_focusRing = c.CreateShapeVisual();
        m_focusText = c.CreateSpriteVisual();
        m_focusText.AnchorPoint(float2(0.5f, 0.5f));
        m_focus.Children().InsertAtTop(m_focusRing);
        m_focus.Children().InsertAtTop(m_focusText);
        m_root.Children().InsertAtTop(m_focus);
    }

    void Ambient::SetLayout(SceneLayout const& layout)
    {
        m_layout = layout;
        LayoutStatic();
    }

    void Ambient::LayoutStatic()
    {
        if (!m_root || m_layout.size.x <= 0) return;
        RECT const& wk = m_layout.primaryWork;
        float W = RectW(wk), H = RectH(wk);
        m_root.Size(m_layout.size);
        m_veil.Size(m_layout.size);

        Update(m_system);

        float emberW = W * 0.6f, emberH = H * 0.5f;
        m_ember.Size(float2(emberW, emberH));
        m_ember.Offset(float3(wk.left + (W - emberW) * 0.5f, wk.bottom - emberH * 0.5f, 0.0f));

        float tideH = H * 0.42f;
        m_tide.Offset(float3(static_cast<float>(wk.left), wk.bottom - tideH, 0.0f));
        m_tide.Size(float2(W, tideH));
        m_tide.CenterPoint(float3(W * 0.5f, tideH, 0.0f));
        m_tideSprite.Size(float2(W, tideH));

        m_dustProps.InsertScalar(L"Left", static_cast<float>(wk.left));
        m_dustProps.InsertScalar(L"Top", static_cast<float>(wk.top));
        m_dustProps.InsertScalar(L"W", W);
        m_dustProps.InsertScalar(L"H", H);

        float horizonY = wk.bottom - H * 0.11f;
        float cx = wk.left + W * 0.5f;
        m_horizonGlow.Size(float2(W * 0.75f, H * 0.18f));
        m_horizonGlow.Offset(float3(cx, horizonY, 0.0f));
        m_horizonLine.Size(float2(W * 0.62f, std::max(1.0f, D(1.5f))));
        m_horizonLine.Offset(float3(cx, horizonY, 0.0f));
        m_horizonLabel.Offset(float3(cx, horizonY - D(12), 0.0f));

        float ring = D(320);
        float cy = wk.top + H * 0.5f;
        m_focusRing.Size(float2(ring, ring));
        m_focusRing.Offset(float3(cx - ring * 0.5f, cy - ring * 0.5f, 0.0f));
        m_focusRing.CenterPoint(float3(ring * 0.5f, ring * 0.5f, 0.0f));
        m_focusRing.RotationAngleInDegrees(-90.0f);
        m_focusRing.Shapes().Clear();
        float thickness = D(5);
        auto track = m_c.CreateEllipseGeometry();
        track.Center(float2(ring * 0.5f, ring * 0.5f));
        track.Radius(float2(ring * 0.5f - thickness, ring * 0.5f - thickness));
        auto trackShape = m_c.CreateSpriteShape(track);
        trackShape.StrokeBrush(m_c.CreateColorBrush(Argb(0x26, 0xFFFFFF)));
        trackShape.StrokeThickness(thickness);
        m_focusRing.Shapes().Append(trackShape);
        m_focusArc = m_c.CreateEllipseGeometry();
        m_focusArc.Center(track.Center());
        m_focusArc.Radius(track.Radius());
        auto arcShape = m_c.CreateSpriteShape(m_focusArc);
        arcShape.StrokeBrush(m_c.CreateColorBrush(Argb(0xE6, 0x8FD8FF)));
        arcShape.StrokeThickness(thickness);
        arcShape.StrokeStartCap(wuc::CompositionStrokeCap::Round);
        arcShape.StrokeEndCap(wuc::CompositionStrokeCap::Round);
        m_focusRing.Shapes().Append(arcShape);
        m_focusText.Offset(float3(cx, cy, 0.0f));

        // Re-place everything that depends on geometry.
        for (auto& [id, fly] : m_fireflies) m_fireflyLayer.Children().Remove(fly.root);
        m_fireflies.clear();
        UpdateFireflies();
        UpdateOrbs();
        m_horizonKey.clear();
        UpdateHorizon(true);
        m_focusKey.clear();
        UpdateFocus(true);
    }

    // ---------------------------------------------------------------- scene / dashboard state

    void Ambient::SetPreset(ScenePreset const& preset)
    {
        bool focusStarted = preset.focus && !m_preset.focus;
        m_preset = preset;
        if (focusStarted) m_focusStart = Clock::now();
        ApplyPreset();
    }

    void Ambient::ApplyPreset()
    {
        if (!m_root) return;
        Fade(m_veil, m_preset.focus ? 0.5f : 0.0f, 1.2f);
        Fade(m_dust, m_preset.dust ? 1.0f : 0.0f, 1.0f);
        if (m_dustController)
        {
            if (m_preset.dust) m_dustController.Resume();
            else m_dustController.Pause();
        }
        m_tideState = -1;
        UpdateTide();
        UpdateFireflies();
        UpdateOrbs();
        m_focusKey.clear();
        UpdateFocus(true);
    }

    void Ambient::SetDashboardVisible(bool visible)
    {
        if (!m_root) return;
        m_dashboard = visible;
        Fade(m_orbLayer, m_orbsHidden ? 0.0f : visible ? 0.2f : 1.0f, visible ? 0.35f : 0.6f);
        Fade(m_fireflyLayer, visible ? 0.3f : 1.0f, 0.5f);
        if (m_preset.focus) Fade(m_focus, visible ? 0.0f : 1.0f, 0.4f);
    }

    void Ambient::SetOrbsHidden(bool hidden)
    {
        if (!m_root || hidden == m_orbsHidden) return;
        m_orbsHidden = hidden;
        Fade(m_orbLayer, hidden ? 0.0f : m_dashboard ? 0.2f : 1.0f, 0.8f);
    }

    // ---------------------------------------------------------------- system pulse

    void Ambient::Update(SystemSnapshot const& snapshot)
    {
        m_system = snapshot;
        if (!m_root || m_layout.size.x <= 0) return;
        RECT const& wk = m_layout.primaryWork;
        float W = RectW(wk), H = RectH(wk);

        if (m_dustController)
        {
            float rate = std::clamp(0.5f + snapshot.cpu * 3.5f, 0.2f, 6.0f);
            m_dustController.PlaybackRate(rate);
        }

        float mem = std::clamp(snapshot.memory, 0.0f, 1.0f);
        float waterH = H * (0.03f + mem * 0.09f);
        m_water.Size(float2(W, waterH));
        m_water.Offset(float3(static_cast<float>(wk.left), wk.bottom - waterH, 0.0f));
        m_waterLine.Size(float2(W, std::max(1.0f, D(1))));
        m_waterLine.Offset(float3(static_cast<float>(wk.left), wk.bottom - waterH, 0.0f));
        float t = std::clamp((mem - 0.65f) / 0.25f, 0.0f, 1.0f);
        Color water = Lerp(Argb(0x22, 0x3AA0C8), Argb(0x3A, 0x8A6CFF), t);
        auto a = m_c.CreateColorKeyFrameAnimation();
        a.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
        a.InsertKeyFrame(1.0f, water);
        a.Duration(3s);
        m_waterStop.StartAnimation(L"Color", a);

        float ember = snapshot.gpuAvailable ? std::clamp((snapshot.gpu - 0.45f) / 0.45f, 0.0f, 1.0f) : 0.0f;
        m_ember.Opacity(ember * 0.55f);
    }

    // ---------------------------------------------------------------- CI tide + fireflies

    void Ambient::Update(PullRequestsSnapshot const& snapshot)
    {
        m_prs = snapshot;
        m_havePrs = true;
        UpdateTide();
        UpdateFireflies();
    }

    void Ambient::UpdateTide()
    {
        if (!m_root) return;
        int state = (!m_preset.tide || !m_havePrs) ? 0 : m_prs.AnyFailing() ? 2 : m_prs.AnyPending() ? 1 : 0;
        if (state == m_tideState) return;
        int previous = m_tideState;
        m_tideState = state;
        if (previous >= 0) Log(L"CI tide: %s", state == 2 ? L"failing (red)" : state == 1 ? L"pending (amber)" : L"calm");

        if (state == 0)
        {
            auto batch = m_c.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
            Fade(m_tide, 0.0f, 2.5f);
            AnimateScalar(m_tide, L"Scale.Y", 0.3f, 2.5f);
            batch.End();
            auto sprite = m_tideSprite;
            auto self = this;
            batch.Completed([sprite, self](auto&&, auto&&)
            {
                if (self->m_tideState == 0 && sprite) { sprite.StopAnimation(L"Opacity"); sprite.Opacity(1.0f); }
            });
            return;
        }

        uint32_t rgb = state == 2 ? palette::Failure : palette::Pending;
        Color colors[2] = { Argb(state == 2 ? 0x22 : 0x14, rgb), Argb(state == 2 ? 0x8C : 0x4A, rgb) };
        for (int i = 0; i < 2; ++i)
        {
            auto a = m_c.CreateColorKeyFrameAnimation();
            a.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
            a.InsertKeyFrame(1.0f, colors[i]);
            a.Duration(2s);
            m_tideStops[i].StartAnimation(L"Color", a);
        }
        Fade(m_tide, 1.0f, 3.0f);
        AnimateScalar(m_tide, L"Scale.Y", state == 2 ? 1.0f : 0.55f, 3.5f);
        m_tideSprite.StartAnimation(L"Opacity", Oscillate(m_c, 0.7f, 1.0f, state == 2 ? 4.0f : 6.0f));
    }

    void Ambient::UpdateFireflies()
    {
        if (!m_root || m_layout.size.x <= 0) return;
        std::set<std::wstring> wanted;
        if (m_preset.fireflies && m_havePrs)
        {
            for (auto const& pr : m_prs.reviews)
            {
                if (wanted.size() >= kMaxFireflies) break;
                wanted.insert(pr.url.empty() ? pr.title : pr.url);
            }
        }

        for (auto it = m_fireflies.begin(); it != m_fireflies.end();)
        {
            if (!wanted.count(it->first))
            {
                RemoveAfterFade(m_fireflyLayer, it->second.root, 1.5f);
                it = m_fireflies.erase(it);
            }
            else ++it;
        }

        RECT const& wk = m_layout.primaryWork;
        float W = RectW(wk), H = RectH(wk);
        for (auto const& key : wanted)
        {
            if (m_fireflies.count(key)) continue;
            Rng rng(Hash(key));
            float3 target(wk.left + W * rng.Next(0.42f, 0.95f), wk.top + H * rng.Next(0.06f, 0.55f), 0.0f);

            auto root = m_c.CreateContainerVisual();
            root.Offset(float3(wk.right + D(60), target.y + H * rng.Next(-0.12f, 0.12f), 0.0f));
            auto enter = m_c.CreateVector3KeyFrameAnimation();
            enter.InsertKeyFrame(1.0f, target, m_c.CreateCubicBezierEasingFunction(float2(0.2f, 0.6f), float2(0.3f, 1.0f)));
            enter.Duration(Seconds(rng.Next(5.0f, 8.0f)));
            enter.DelayTime(Seconds(rng.Next(0.0f, 1.5f)));
            root.StartAnimation(L"Offset", enter);

            auto drifter = m_c.CreateContainerVisual();
            drifter.StartAnimation(L"Offset", Drift(m_c, rng, float3(0.0f), D(70), 5, rng.Next(22.0f, 36.0f)));
            drifter.StartAnimation(L"Opacity", Oscillate(m_c, 0.3f, 1.0f, rng.Next(1.4f, 3.0f), rng.Next(0.0f, 2.0f)));
            auto glow = m_c.CreateSpriteVisual();
            glow.Size(float2(D(52), D(52)));
            glow.AnchorPoint(float2(0.5f, 0.5f));
            glow.Brush(GlowBrush(m_c, Argb(0x66, palette::Firefly), 0.3f, 0.3f));
            auto core = m_c.CreateSpriteVisual();
            core.Size(float2(D(10), D(10)));
            core.AnchorPoint(float2(0.5f, 0.5f));
            core.Brush(GlowBrush(m_c, Argb(0xFF, 0xF8FFC8), 0.5f, 0.7f));
            drifter.Children().InsertAtTop(glow);
            drifter.Children().InsertAtTop(core);
            root.Children().InsertAtTop(drifter);
            m_fireflyLayer.Children().InsertAtTop(root);
            m_fireflies[key] = Firefly{ root, true };
        }
    }

    // ---------------------------------------------------------------- agent orbs

    void Ambient::Update(SessionsSnapshot const& snapshot)
    {
        m_sessions = snapshot;
        m_haveSessions = true;
        UpdateOrbs();
    }

    float2 Ambient::OrbSlot(size_t index, size_t count) const
    {
        RECT const& wk = m_layout.primaryWork;
        float W = RectW(wk), H = RectH(wk);
        float x0 = wk.left + W * 0.30f, x1 = wk.right - W * 0.08f;
        float spacing = std::min((x1 - x0) / std::max<size_t>(1, count), D(250));
        float start = (x0 + x1) * 0.5f - spacing * count * 0.5f;
        float baseY = wk.top + H * (m_preset.focus ? 0.13f : 0.19f);
        float y = baseY + ((index % 2) ? H * 0.075f : 0.0f);
        return float2(start + (index + 0.5f) * spacing, y);
    }

    Ambient::Orb Ambient::CreateOrb(AgentSession const& session)
    {
        Orb o;
        o.session = session;
        Rng rng(Hash(session.id));
        o.root = m_c.CreateContainerVisual();
        o.floater = m_c.CreateContainerVisual();
        o.floater.StartAnimation(L"Offset", Drift(m_c, rng, float3(0.0f), D(24), 4, rng.Next(26.0f, 40.0f)));
        o.bobber = m_c.CreateContainerVisual();

        Color color = Argb(0xFF, StatusRgb(session.status));
        o.haloBrush = MakeGlow(WithAlpha(color, 0.55f), 0.3f);
        o.coreBrush = MakeGlow(color, 0.75f);

        o.halo = m_c.CreateSpriteVisual();
        float halo = D(130);
        o.halo.Size(float2(halo, halo));
        o.halo.AnchorPoint(float2(0.5f, 0.5f));
        o.halo.CenterPoint(float3(halo * 0.5f, halo * 0.5f, 0.0f));
        o.halo.Brush(o.haloBrush.brush);

        o.core = m_c.CreateSpriteVisual();
        float core = D(30);
        o.core.Size(float2(core, core));
        o.core.AnchorPoint(float2(0.5f, 0.5f));
        o.core.CenterPoint(float3(core * 0.5f, core * 0.5f, 0.0f));
        o.core.Brush(o.coreBrush.brush);

        o.label = m_c.CreateSpriteVisual();
        o.label.AnchorPoint(float2(0.5f, 0.0f));
        o.label.Offset(float3(0.0f, D(30), 0.0f));

        o.bobber.Children().InsertAtTop(o.halo);
        o.bobber.Children().InsertAtTop(o.core);
        o.floater.Children().InsertAtTop(o.bobber);
        o.floater.Children().InsertAtTop(o.label);
        o.root.Children().InsertAtTop(o.floater);
        o.root.Opacity(0.0f);
        return o;
    }

    void Ambient::UpdateOrbs()
    {
        if (!m_root || !m_haveSessions || m_layout.size.x <= 0) return;
        std::vector<AgentSession const*> visible;
        for (auto const& s : m_sessions.sessions)
        {
            if (m_preset.allOrbs || s.status == AgentStatus::Waiting) visible.push_back(&s);
        }

        std::set<std::wstring> keep;
        for (size_t i = 0; i < visible.size(); ++i)
        {
            auto const& s = *visible[i];
            keep.insert(s.id);
            float2 slot = OrbSlot(i, visible.size());
            auto it = m_orbs.find(s.id);
            if (it == m_orbs.end())
            {
                Orb o = CreateOrb(s);
                o.root.Offset(float3(slot.x, slot.y, 0.0f));
                o.root.ImplicitAnimations(MakeImplicit(m_c, 1.6f, true, false, false));
                m_orbLayer.Children().InsertAtTop(o.root);
                ApplyStatus(o, std::nullopt);
                m_orbs.emplace(s.id, std::move(o));
            }
            else
            {
                Orb& o = it->second;
                AgentStatus previous = o.session.status;
                o.session = s;
                o.root.Offset(float3(slot.x, slot.y, 0.0f));
                if (previous != s.status)
                {
                    Log(L"Agent '%s': %s -> %s", Truncate(s.title, 40).c_str(), AgentStatusName(previous), AgentStatusName(s.status));
                    ApplyStatus(o, previous);
                }
                else
                {
                    DrawLabel(o, false);
                }
            }
        }

        for (auto it = m_orbs.begin(); it != m_orbs.end();)
        {
            if (!keep.count(it->first))
            {
                RemoveAfterFade(m_orbLayer, it->second.root, 1.2f);
                it = m_orbs.erase(it);
            }
            else ++it;
        }
    }

    void Ambient::ApplyStatus(Orb& o, std::optional<AgentStatus> previous)
    {
        auto const status = o.session.status;
        o.core.StopAnimation(L"Scale.X");
        o.core.StopAnimation(L"Scale.Y");
        o.halo.StopAnimation(L"Opacity");
        o.bobber.StopAnimation(L"Offset.Y");
        o.bobber.Offset(float3(0.0f));
        o.halo.Scale(float3(1.0f));
        o.core.Scale(float3(1.0f));

        Color color = Argb(0xFF, StatusRgb(status));
        auto pulse = [&](float lo, float hi, float seconds)
        {
            auto a = Oscillate(m_c, lo, hi, seconds);
            o.core.StartAnimation(L"Scale.X", a);
            o.core.StartAnimation(L"Scale.Y", a);
        };

        switch (status)
        {
        case AgentStatus::Working:
            SetGlowColor(o.haloBrush, WithAlpha(color, 0.55f), 0.3f, 0.8f);
            SetGlowColor(o.coreBrush, color, 0.75f, 0.8f);
            pulse(0.9f, 1.08f, 1.8f);
            o.halo.StartAnimation(L"Opacity", Oscillate(m_c, 0.35f, 0.8f, 1.8f));
            Fade(o.root, 1.0f, 0.8f);
            break;

        case AgentStatus::Waiting:
            SetGlowColor(o.haloBrush, WithAlpha(color, 0.7f), 0.35f, 0.8f);
            SetGlowColor(o.coreBrush, color, 0.8f, 0.8f);
            o.halo.Scale(float3(1.35f, 1.35f, 1.0f));
            pulse(0.94f, 1.06f, 1.2f);
            o.halo.StartAnimation(L"Opacity", Oscillate(m_c, 0.6f, 1.0f, 1.2f));
            o.bobber.StartAnimation(L"Offset.Y", Oscillate(m_c, 0.0f, -D(12), 1.7f));
            Fade(o.root, 1.0f, 0.8f);
            break;

        case AgentStatus::Done:
        {
            bool fresh = previous ? *previous != AgentStatus::Done
                                  : (Clock::now() - o.session.since) < 90s;
            Color dim = Argb(0xFF, palette::DoneDim);
            if (fresh)
            {
                SetGlowColor(o.haloBrush, WithAlpha(color, 0.8f), 0.35f, 0.2f);
                SetGlowColor(o.coreBrush, color, 0.8f, 0.2f);
                auto const& ctx = m_scene->Context();
                SpawnRipple(ctx, o.bobber, float2(0.0f), Argb(0xD0, palette::Done), D(240), 0.0f);
                SpawnRipple(ctx, o.bobber, float2(0.0f), Argb(0x90, palette::Done), D(380), 0.2f);
                auto burst = m_c.CreateScalarKeyFrameAnimation();
                burst.InsertKeyFrame(0.0f, 1.0f);
                burst.InsertKeyFrame(0.18f, 1.8f, Smooth(m_c));
                burst.InsertKeyFrame(0.6f, 0.95f, Smooth(m_c));
                burst.InsertKeyFrame(1.0f, 0.85f, Smooth(m_c));
                burst.Duration(1600ms);
                o.core.StartAnimation(L"Scale.X", burst);
                o.core.StartAnimation(L"Scale.Y", burst);
                auto haloFade = m_c.CreateScalarKeyFrameAnimation();
                haloFade.InsertKeyFrame(0.0f, 1.0f);
                haloFade.InsertKeyFrame(1.0f, 0.2f, Smooth(m_c));
                haloFade.Duration(6s);
                o.halo.StartAnimation(L"Opacity", haloFade);
                Fade(o.root, 1.0f, 0.2f);
                SetGlowColor(o.haloBrush, WithAlpha(dim, 0.5f), 0.3f, 5.0f, 3.0f);
                SetGlowColor(o.coreBrush, dim, 0.7f, 5.0f, 3.0f);
                Fade(o.root, 0.62f, 5.0f, 3.0f);
            }
            else
            {
                SetGlowColor(o.haloBrush, WithAlpha(dim, 0.5f), 0.3f, 0.8f);
                SetGlowColor(o.coreBrush, dim, 0.7f, 0.8f);
                o.halo.Opacity(0.2f);
                o.core.Scale(float3(0.85f, 0.85f, 1.0f));
                Fade(o.root, 0.62f, 1.0f);
            }
            break;
        }

        default:
            SetGlowColor(o.haloBrush, WithAlpha(color, 0.45f), 0.3f, 1.2f);
            SetGlowColor(o.coreBrush, color, 0.6f, 1.2f);
            o.halo.Opacity(0.14f);
            o.core.Scale(float3(0.78f, 0.78f, 1.0f));
            Fade(o.root, 0.45f, 1.2f);
            break;
        }
        DrawLabel(o, true);
    }

    void Ambient::DrawLabel(Orb& o, bool force)
    {
        auto const& s = o.session;
        std::wstring sub;
        switch (s.status)
        {
        case AgentStatus::Waiting: sub = L"Needs your input \u00B7 " + Age(s.since); break;
        case AgentStatus::Working: sub = L"Working \u00B7 " + Age(s.since); break;
        case AgentStatus::Done: sub = L"Finished " + Age(s.since) + L" ago"; break;
        default: sub = L"Idle \u00B7 " + Age(s.lastActivity); break;
        }
        std::wstring title = s.title.empty() ? s.repo : s.title;
        std::wstring key = title + L"\n" + sub + L"\n" + std::to_wstring(m_layout.scale);
        if (!force && key == o.labelKey && o.surface) return;
        o.labelKey = key;

        auto& g = *m_graphics;
        float padX = D(10), padY = D(6), dot = D(6), gap = D(6);
        auto titleLayout = g.Layout(title, g.Font(FontRole::UiSemibold, 12.5f), D(230));
        auto subLayout = g.Layout(sub, g.Font(FontRole::Ui, 11.5f), D(230) - dot - gap);
        float2 ts = g.Measure(titleLayout.get());
        float2 ss = g.Measure(subLayout.get());
        float w = std::ceil(std::max(ts.x, ss.x + dot + gap) + padX * 2);
        float h = std::ceil(ts.y + ss.y + padY * 2);
        if (!o.surface)
        {
            o.surface = g.CreateSurface(float2(w, h));
            o.label.Brush(m_c.CreateSurfaceBrush(o.surface));
        }
        uint32_t statusRgb = s.status == AgentStatus::Done ? palette::DoneDim : StatusRgb(s.status);
        g.Draw(o.surface, float2(w, h), [&](ID2D1DeviceContext* dc)
        {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(D2DColor(0x0B0F17, 0.5f), brush.put());
            auto pill = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), D(9), D(9));
            dc->FillRoundedRectangle(pill, brush.get());
            brush->SetColor(D2DColor(0xFFFFFF, 0.08f));
            dc->DrawRoundedRectangle(pill, brush.get(), 1.0f);
            brush->SetColor(D2DColor(0xFFFFFF, 0.95f));
            dc->DrawTextLayout(D2D1::Point2F(padX, padY), titleLayout.get(), brush.get());
            brush->SetColor(D2DColor(statusRgb, 1.0f));
            float dotY = padY + ts.y + ss.y * 0.5f;
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(padX + dot * 0.5f, dotY), dot * 0.5f, dot * 0.5f), brush.get());
            brush->SetColor(D2DColor(0xFFFFFF, 0.72f));
            dc->DrawTextLayout(D2D1::Point2F(padX + dot + gap, padY + ts.y), subLayout.get(), brush.get());
        });
        o.label.Size(float2(w, h));
    }

    // ---------------------------------------------------------------- horizon (next meeting)

    void Ambient::Update(CalendarSnapshot const& snapshot)
    {
        m_calendar = snapshot;
        m_haveCalendar = true;
        UpdateHorizon(false);
    }

    void Ambient::UpdateHorizon(bool force)
    {
        if (!m_root || m_layout.size.x <= 0) return;
        auto now = Clock::now();
        Meeting const* meeting = nullptr;
        bool inProgress = false;
        if (m_haveCalendar)
        {
            for (auto const& m : m_calendar.meetings)
            {
                if (m.allDay) continue;
                if (m.start <= now && now < m.end) { meeting = &m; inProgress = true; break; }
                if (m.start > now && m.start - now <= 60min) { meeting = &m; break; }
            }
        }
        if (!meeting)
        {
            if (!m_horizonKey.empty() || force)
            {
                m_horizonKey.clear();
                Fade(m_horizon, 0.0f, 2.0f);
            }
            return;
        }

        int minutes = static_cast<int>(std::ceil(std::chrono::duration<double>(meeting->start - now).count() / 60.0));
        std::wstring text = inProgress
            ? L"Now: " + Truncate(meeting->title, 48) + L" \u00B7 until " + FormatClock(meeting->end)
            : Truncate(meeting->title, 48) + L" in " + std::to_wstring(std::max(1, minutes)) + L" min";
        bool soon = !inProgress && minutes <= 5;
        uint32_t rgb = inProgress ? palette::MeetingNow : soon ? palette::MeetingSoon : palette::Meeting;
        std::wstring key = text + std::to_wstring(rgb) + std::to_wstring(m_layout.scale);
        if (!force && key == m_horizonKey) return;
        bool colorChanged = m_horizonKey.empty() || m_horizonKey.substr(text.size()) != key.substr(text.size());
        m_horizonKey = key;

        auto& g = *m_graphics;
        auto icon = g.Layout(L"\uE787", g.Font(FontRole::Icons, 13.0f), D(20), 10000.0f, false);
        auto layout = g.Layout(text, g.Font(FontRole::Ui, 13.0f), D(560));
        float2 is = g.Measure(icon.get());
        float2 ts = g.Measure(layout.get());
        float gap = D(8);
        float w = std::ceil(is.x + gap + ts.x + D(4)), h = std::ceil(std::max(is.y, ts.y) + D(4));
        if (!m_horizonSurface)
        {
            m_horizonSurface = g.CreateSurface(float2(w, h));
            m_horizonLabel.Brush(m_c.CreateSurfaceBrush(m_horizonSurface));
        }
        g.Draw(m_horizonSurface, float2(w, h), [&](ID2D1DeviceContext* dc)
        {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(D2DColor(rgb, 0.95f), brush.put());
            dc->DrawTextLayout(D2D1::Point2F(D(2), (h - is.y) * 0.5f), icon.get(), brush.get());
            brush->SetColor(D2DColor(0xFFFFFF, 0.88f));
            dc->DrawTextLayout(D2D1::Point2F(D(2) + is.x + gap, (h - ts.y) * 0.5f), layout.get(), brush.get());
        });
        m_horizonLabel.Size(float2(w, h));

        if (colorChanged)
        {
            SetGlowColor(m_horizonBrush, Argb(0x70, rgb), 0.22f, 2.0f);
            auto a = m_c.CreateColorKeyFrameAnimation();
            a.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
            a.InsertKeyFrame(1.0f, Argb(0xA0, rgb));
            a.Duration(2s);
            m_horizonLineStop.StartAnimation(L"Color", a);
            if (soon) m_horizonGlow.StartAnimation(L"Opacity", Oscillate(m_c, 0.55f, 1.0f, 1.4f));
            else { m_horizonGlow.StopAnimation(L"Opacity"); m_horizonGlow.Opacity(1.0f); }
        }
        float closeness = inProgress ? 0.55f : std::clamp(1.0f - minutes / 60.0f, 0.0f, 1.0f);
        Fade(m_horizon, 0.35f + 0.65f * closeness, 2.0f);
    }

    // ---------------------------------------------------------------- focus timer

    void Ambient::UpdateFocus(bool force)
    {
        if (!m_root || m_layout.size.x <= 0) return;
        if (!m_preset.focus)
        {
            if (force)
            {
                Fade(m_focus, 0.0f, 0.8f);
                if (m_focusArc) m_focusArc.StopAnimation(L"TrimEnd");
            }
            return;
        }

        auto now = Clock::now();
        double total = m_focusMinutes * 60.0;
        double elapsed = std::chrono::duration<double>(now - m_focusStart).count();
        if (elapsed >= total + 300.0)
        {
            m_focusStart = now;
            elapsed = 0;
            force = true;
            Log(L"Focus: new %d minute block", m_focusMinutes);
        }
        double remaining = total - elapsed;
        std::wstring big = remaining > 0 ? std::to_wstring(static_cast<int>(std::ceil(remaining / 60.0))) : L"Break";
        std::wstring caption = remaining > 0 ? L"min of focus left" : L"focus block complete";
        std::wstring key = big + L"|" + caption + std::to_wstring(m_layout.scale);
        if (force && m_focusArc)
        {
            float frac = static_cast<float>(std::clamp(remaining / total, 0.0, 1.0));
            m_focusArc.TrimEnd(frac);
            if (remaining > 0)
            {
                auto a = m_c.CreateScalarKeyFrameAnimation();
                a.InsertKeyFrame(0.0f, frac);
                a.InsertKeyFrame(1.0f, 0.0f, m_c.CreateLinearEasingFunction());
                a.Duration(Seconds(static_cast<float>(remaining)));
                m_focusArc.StartAnimation(L"TrimEnd", a);
            }
            if (!m_dashboard) Fade(m_focus, 1.0f, 1.2f);
        }
        if (!force && key == m_focusKey) return;
        m_focusKey = key;

        auto& g = *m_graphics;
        float w = D(300), h = D(170);
        auto bigLayout = g.Layout(big, g.Font(FontRole::Display, 84.0f, DWRITE_TEXT_ALIGNMENT_CENTER), w, 10000.0f, false);
        auto captionLayout = g.Layout(caption, g.Font(FontRole::Ui, 14.0f, DWRITE_TEXT_ALIGNMENT_CENTER), w, 10000.0f, false);
        float2 bs = g.Measure(bigLayout.get());
        float2 ss = g.Measure(captionLayout.get());
        if (!m_focusSurface)
        {
            m_focusSurface = g.CreateSurface(float2(w, h));
            m_focusText.Brush(m_c.CreateSurfaceBrush(m_focusSurface));
        }
        g.Draw(m_focusSurface, float2(w, h), [&](ID2D1DeviceContext* dc)
        {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(D2DColor(0xFFFFFF, 0.92f), brush.put());
            float top = (h - bs.y - ss.y) * 0.5f;
            dc->DrawTextLayout(D2D1::Point2F(0, top), bigLayout.get(), brush.get());
            brush->SetColor(D2DColor(0xBFE6FF, 0.75f));
            dc->DrawTextLayout(D2D1::Point2F(0, top + bs.y), captionLayout.get(), brush.get());
        });
        m_focusText.Size(float2(w, h));
    }

    // ---------------------------------------------------------------- periodic

    void Ambient::Tick()
    {
        if (!m_root) return;
        for (auto& [id, orb] : m_orbs) DrawLabel(orb, false);
        UpdateHorizon(false);
        UpdateFocus(false);
    }

    void Ambient::RedrawAll()
    {
        if (!m_root) return;
        for (auto& [id, orb] : m_orbs) DrawLabel(orb, true);
        UpdateHorizon(true);
        m_focusKey.clear();
        UpdateFocus(false);
    }
}
