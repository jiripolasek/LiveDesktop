#include "pch.h"
#include "Scene.h"

using namespace std::chrono_literals;
using winrt::Windows::UI::Color;
using winrt::Windows::Foundation::TimeSpan;

namespace ld
{
    namespace
    {
        Color Argb(uint8_t a, uint32_t rgb)
        {
            return Color{ a, static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb) };
        }

        TimeSpan Seconds(float s)
        {
            return std::chrono::duration_cast<TimeSpan>(std::chrono::duration<float>(s));
        }

        struct Rng
        {
            std::mt19937 gen{ std::random_device{}() };
            float Next(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(gen); }
            size_t Index(size_t count) { return std::uniform_int_distribution<size_t>(0, count - 1)(gen); }
        };

        float Area(RECT const& rc) { return static_cast<float>(rc.right - rc.left) * static_cast<float>(rc.bottom - rc.top); }

        RECT const& PickMonitor(SceneContext const& ctx, Rng& rng)
        {
            float total = 0;
            for (auto const& rc : ctx.monitors) total += Area(rc);
            float pick = rng.Next(0.0f, total);
            for (auto const& rc : ctx.monitors)
            {
                pick -= Area(rc);
                if (pick <= 0) return rc;
            }
            return ctx.monitors.back();
        }

        float2 RandomPointIn(RECT const& rc, Rng& rng, float margin = 0.0f)
        {
            return float2(rng.Next(rc.left - margin, rc.right + margin), rng.Next(rc.top - margin, rc.bottom + margin));
        }

        // Soft "blurred" disc without any blur effect: radial gradient from color to fully transparent.
        wuc::CompositionRadialGradientBrush GlowBrush(wuc::Compositor const& c, Color color, float midStop = 0.4f, float midAlpha = 0.35f)
        {
            auto brush = c.CreateRadialGradientBrush();
            brush.EllipseCenter(float2(0.5f, 0.5f));
            brush.EllipseRadius(float2(0.5f, 0.5f));
            Color mid = color;
            mid.A = static_cast<uint8_t>(color.A * midAlpha);
            Color end = color;
            end.A = 0;
            brush.ColorStops().Append(c.CreateColorGradientStop(0.0f, color));
            brush.ColorStops().Append(c.CreateColorGradientStop(midStop, mid));
            brush.ColorStops().Append(c.CreateColorGradientStop(1.0f, end));
            return brush;
        }

        // Transparent center, bright band, transparent edge -> a ring.
        wuc::CompositionRadialGradientBrush RingBrush(wuc::Compositor const& c, Color color)
        {
            auto brush = c.CreateRadialGradientBrush();
            Color clear = color;
            clear.A = 0;
            brush.ColorStops().Append(c.CreateColorGradientStop(0.0f, clear));
            brush.ColorStops().Append(c.CreateColorGradientStop(0.62f, clear));
            brush.ColorStops().Append(c.CreateColorGradientStop(0.82f, color));
            brush.ColorStops().Append(c.CreateColorGradientStop(1.0f, clear));
            return brush;
        }

        // Looping wander around `base` (ends where it starts so the loop is seamless).
        wuc::Vector3KeyFrameAnimation Drift(wuc::Compositor const& c, Rng& rng, float3 base, float range, int keys, float seconds)
        {
            auto ease = c.CreateCubicBezierEasingFunction(float2(0.45f, 0.0f), float2(0.55f, 1.0f));
            auto anim = c.CreateVector3KeyFrameAnimation();
            anim.InsertKeyFrame(0.0f, base);
            for (int i = 1; i < keys; ++i)
            {
                anim.InsertKeyFrame(static_cast<float>(i) / keys,
                    base + float3(rng.Next(-range, range), rng.Next(-range, range), 0.0f), ease);
            }
            anim.InsertKeyFrame(1.0f, base, ease);
            anim.Duration(Seconds(seconds));
            anim.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
            return anim;
        }

        wuc::ScalarKeyFrameAnimation Oscillate(wuc::Compositor const& c, float from, float to, float seconds, float delay = 0.0f)
        {
            auto ease = c.CreateCubicBezierEasingFunction(float2(0.45f, 0.0f), float2(0.55f, 1.0f));
            auto anim = c.CreateScalarKeyFrameAnimation();
            anim.InsertKeyFrame(0.0f, from);
            anim.InsertKeyFrame(1.0f, to, ease);
            anim.Duration(Seconds(seconds));
            anim.DelayTime(Seconds(delay));
            anim.Direction(wuc::AnimationDirection::Alternate);
            anim.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
            return anim;
        }

        wuc::ColorKeyFrameAnimation CycleColors(wuc::Compositor const& c, std::initializer_list<Color> colors, float seconds)
        {
            auto ease = c.CreateCubicBezierEasingFunction(float2(0.45f, 0.0f), float2(0.55f, 1.0f));
            auto anim = c.CreateColorKeyFrameAnimation();
            std::vector<Color> list(colors);
            for (size_t i = 0; i < list.size(); ++i)
            {
                anim.InsertKeyFrame(static_cast<float>(i) / list.size(), list[i], ease);
            }
            anim.InsertKeyFrame(1.0f, list.front(), ease);
            anim.Duration(Seconds(seconds));
            anim.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
            return anim;
        }

        // Cursor-following light shared by all effects.
        void AddCursorGlow(SceneContext const& ctx, wuc::ContainerVisual const& layer, Color color, float diameter)
        {
            auto const& c = ctx.compositor;
            auto glow = c.CreateSpriteVisual();
            glow.Size(float2(diameter, diameter));
            glow.Brush(GlowBrush(c, color, 0.3f, 0.3f));
            auto offset = c.CreateExpressionAnimation(L"pointer.Position - Vector3(half, half, 0.0)");
            offset.SetReferenceParameter(L"pointer", ctx.pointer);
            offset.SetScalarParameter(L"half", diameter * 0.5f);
            glow.StartAnimation(L"Offset", offset);
            auto opacity = c.CreateExpressionAnimation(L"pointer.Strength");
            opacity.SetReferenceParameter(L"pointer", ctx.pointer);
            glow.StartAnimation(L"Opacity", opacity);
            layer.Children().InsertAtTop(glow);
        }

        // Expression: push the child away from the pointer, falling off linearly to 0 at pointer.Radius.
        // `anchor` is the parent container whose Offset is drifting; `half` is the child's radius.
        constexpr wchar_t kRepelExpression[] =
            L"(anchor.Offset + Vector3(half, half, 0.0) - pointer.Position)"
            L" * (push * pointer.Strength"
            L"    * Max(0.0, 1.0 - Length(anchor.Offset + Vector3(half, half, 0.0) - pointer.Position) / pointer.Radius)"
            L"    / Max(Length(anchor.Offset + Vector3(half, half, 0.0) - pointer.Position), 1.0))";

        // Expression: pull the child toward the pointer (fireflies swarm the cursor).
        constexpr wchar_t kAttractExpression[] =
            L"(pointer.Position - anchor.Offset - Vector3(half, half, 0.0))"
            L" * (pull * pointer.Strength"
            L"    * Max(0.0, 1.0 - Length(pointer.Position - anchor.Offset - Vector3(half, half, 0.0)) / (pointer.Radius * 1.3)))";

        void SpawnRipple(SceneContext const& ctx, wuc::ContainerVisual const& layer, float2 pos, Color color, float diameter, float delay)
        {
            auto const& c = ctx.compositor;
            auto ring = c.CreateSpriteVisual();
            ring.Size(float2(diameter, diameter));
            ring.AnchorPoint(float2(0.5f, 0.5f));
            ring.Offset(float3(pos.x, pos.y, 0.0f));
            ring.CenterPoint(float3(diameter * 0.5f, diameter * 0.5f, 0.0f));
            ring.Scale(float3(0.02f, 0.02f, 1.0f));
            ring.Opacity(0.0f);
            ring.Brush(RingBrush(c, color));
            layer.Children().InsertAtTop(ring);

            auto easeOut = c.CreateCubicBezierEasingFunction(float2(0.1f, 0.7f), float2(0.3f, 1.0f));
            auto scale = c.CreateVector3KeyFrameAnimation();
            scale.InsertKeyFrame(0.0f, float3(0.02f, 0.02f, 1.0f));
            scale.InsertKeyFrame(1.0f, float3(1.0f, 1.0f, 1.0f), easeOut);
            scale.Duration(1300ms);
            scale.DelayTime(Seconds(delay));
            auto fade = c.CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(0.0f, 1.0f);
            fade.InsertKeyFrame(1.0f, 0.0f, c.CreateCubicBezierEasingFunction(float2(0.5f, 0.0f), float2(0.9f, 0.6f)));
            fade.Duration(1300ms);
            fade.DelayTime(Seconds(delay));

            auto batch = c.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
            ring.StartAnimation(L"Scale", scale);
            ring.StartAnimation(L"Opacity", fade);
            batch.End();
            batch.Completed([layer, ring](auto&&, auto&&)
            {
                layer.Children().Remove(ring);
                ring.Close();
            });
        }

        // ------------------------------------------------------------------------------------------
        // Aurora: rotating gradient sky, swaying aurora curtains, drifting glow orbs that dodge the
        // cursor, a cursor light, and expanding rings on click.
        // ------------------------------------------------------------------------------------------
        struct AuroraPalette
        {
            uint32_t sky[4];
            uint32_t curtains[3];
            uint32_t orbs[5];
            uint32_t cursor;
            uint32_t ripple;
        };

        class AuroraEffect final : public Effect
        {
        public:
            AuroraEffect(const wchar_t* name, AuroraPalette palette) : m_name(name), m_p(palette) {}
            const wchar_t* Name() const override { return m_name; }

            void Build(SceneContext const& ctx, wuc::ContainerVisual const& layer) override
            {
                auto const& c = ctx.compositor;
                float W = ctx.size.x, H = ctx.size.y;

                // Sky: linear gradient whose stops cycle colors and whose axis slowly rotates.
                auto sky = c.CreateSpriteVisual();
                sky.Size(ctx.size);
                auto gradient = c.CreateLinearGradientBrush();
                auto s0 = c.CreateColorGradientStop(0.0f, Argb(255, m_p.sky[0]));
                auto s1 = c.CreateColorGradientStop(0.5f, Argb(255, m_p.sky[1]));
                auto s2 = c.CreateColorGradientStop(1.0f, Argb(255, m_p.sky[2]));
                gradient.ColorStops().Append(s0);
                gradient.ColorStops().Append(s1);
                gradient.ColorStops().Append(s2);
                s0.StartAnimation(L"Color", CycleColors(c, { Argb(255, m_p.sky[0]), Argb(255, m_p.sky[3]), Argb(255, m_p.sky[1]) }, 34.0f));
                s1.StartAnimation(L"Color", CycleColors(c, { Argb(255, m_p.sky[1]), Argb(255, m_p.sky[2]), Argb(255, m_p.sky[3]) }, 27.0f));
                s2.StartAnimation(L"Color", CycleColors(c, { Argb(255, m_p.sky[2]), Argb(255, m_p.sky[0]), Argb(255, m_p.sky[3]) }, 41.0f));
                s1.StartAnimation(L"Offset", Oscillate(c, 0.3f, 0.7f, 13.0f));

                auto linear = c.CreateLinearEasingFunction();
                auto start = c.CreateVector2KeyFrameAnimation();
                auto end = c.CreateVector2KeyFrameAnimation();
                constexpr int kSteps = 16;
                for (int i = 0; i <= kSteps; ++i)
                {
                    float t = static_cast<float>(i) / kSteps;
                    float a = t * 6.2831853f;
                    float2 dir(std::cos(a) * 0.5f, std::sin(a) * 0.5f);
                    start.InsertKeyFrame(t, float2(0.5f, 0.5f) - dir, linear);
                    end.InsertKeyFrame(t, float2(0.5f, 0.5f) + dir, linear);
                }
                for (auto anim : { start, end })
                {
                    anim.Duration(90s);
                    anim.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
                }
                gradient.StartAnimation(L"StartPoint", start);
                gradient.StartAnimation(L"EndPoint", end);
                sky.Brush(gradient);
                layer.Children().InsertAtTop(sky);

                // Aurora curtains: wide soft ellipses that sway, stretch and breathe (per monitor).
                for (auto const& mon : ctx.monitors)
                {
                    float mw = static_cast<float>(mon.right - mon.left), mh = static_cast<float>(mon.bottom - mon.top);
                    for (int i = 0; i < 3; ++i)
                    {
                        auto curtain = c.CreateSpriteVisual();
                        float2 sz(mw * m_rng.Next(1.0f, 1.4f), mh * m_rng.Next(0.28f, 0.42f));
                        curtain.Size(sz);
                        curtain.AnchorPoint(float2(0.5f, 0.5f));
                        curtain.CenterPoint(float3(sz.x * 0.5f, sz.y * 0.5f, 0.0f));
                        float3 center(mon.left + mw * m_rng.Next(0.35f, 0.65f), mon.top + mh * (0.22f + 0.24f * i), 0.0f);
                        curtain.Offset(center);
                        curtain.Brush(GlowBrush(c, Argb(120, m_p.curtains[i]), 0.45f, 0.45f));
                        curtain.RotationAngleInDegrees(m_rng.Next(-8.0f, 8.0f));
                        curtain.StartAnimation(L"RotationAngleInDegrees", Oscillate(c, m_rng.Next(-14.0f, -4.0f), m_rng.Next(4.0f, 14.0f), m_rng.Next(16.0f, 26.0f)));
                        curtain.StartAnimation(L"Offset", Drift(c, m_rng, center, mw * 0.12f, 4, m_rng.Next(30.0f, 50.0f)));
                        auto stretch = c.CreateVector3KeyFrameAnimation();
                        auto ease = c.CreateCubicBezierEasingFunction(float2(0.45f, 0.0f), float2(0.55f, 1.0f));
                        stretch.InsertKeyFrame(0.0f, float3(1.0f, 1.0f, 1.0f));
                        stretch.InsertKeyFrame(0.5f, float3(m_rng.Next(1.05f, 1.25f), m_rng.Next(0.55f, 0.8f), 1.0f), ease);
                        stretch.InsertKeyFrame(1.0f, float3(1.0f, 1.0f, 1.0f), ease);
                        stretch.Duration(Seconds(m_rng.Next(9.0f, 15.0f)));
                        stretch.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
                        curtain.StartAnimation(L"Scale", stretch);
                        curtain.StartAnimation(L"Opacity", Oscillate(c, 0.45f, 1.0f, m_rng.Next(5.0f, 9.0f), m_rng.Next(0.0f, 3.0f)));
                        layer.Children().InsertAtTop(curtain);
                    }
                }

                // Orbs: anchor (drifts) -> orb (repelled from cursor via expression) -> glow brush.
                for (auto const& mon : ctx.monitors)
                {
                    float mw = static_cast<float>(mon.right - mon.left), mh = static_cast<float>(mon.bottom - mon.top);
                    float minDim = std::min(mw, mh);
                    for (int i = 0; i < 8; ++i)
                    {
                        float d = minDim * m_rng.Next(0.16f, 0.36f);
                        float2 p = RandomPointIn(mon, m_rng);
                        float3 base(p.x - d * 0.5f, p.y - d * 0.5f, 0.0f);

                        auto anchor = c.CreateContainerVisual();
                        anchor.Size(float2(d, d));
                        anchor.Offset(base);
                        anchor.StartAnimation(L"Offset", Drift(c, m_rng, base, d * 0.9f, 5, m_rng.Next(28.0f, 55.0f)));

                        auto orb = c.CreateSpriteVisual();
                        orb.Size(float2(d, d));
                        orb.CenterPoint(float3(d * 0.5f, d * 0.5f, 0.0f));
                        orb.Brush(GlowBrush(c, Argb(static_cast<uint8_t>(m_rng.Next(120.0f, 190.0f)), m_p.orbs[m_rng.Index(5)])));
                        auto breathe = Oscillate(c, 0.85f, 1.15f, m_rng.Next(5.0f, 11.0f), m_rng.Next(0.0f, 4.0f));
                        auto scale = c.CreateExpressionAnimation(L"Vector3(s.Value, s.Value, 1.0)");
                        auto scalar = c.CreatePropertySet();
                        scalar.InsertScalar(L"Value", 1.0f);
                        scalar.StartAnimation(L"Value", breathe);
                        scale.SetReferenceParameter(L"s", scalar);
                        orb.StartAnimation(L"Scale", scale);

                        auto repel = c.CreateExpressionAnimation(kRepelExpression);
                        repel.SetReferenceParameter(L"anchor", anchor);
                        repel.SetReferenceParameter(L"pointer", ctx.pointer);
                        repel.SetScalarParameter(L"half", d * 0.5f);
                        repel.SetScalarParameter(L"push", d * 0.75f);
                        orb.StartAnimation(L"Offset", repel);

                        anchor.Children().InsertAtTop(orb);
                        layer.Children().InsertAtTop(anchor);
                    }
                }

                AddCursorGlow(ctx, layer, Argb(105, m_p.cursor), std::max(360.0f, std::min(W, H) * 0.35f));
            }

            void OnClick(SceneContext const& ctx, wuc::ContainerVisual const& layer, float2 pos) override
            {
                float d = std::max(420.0f, std::min(ctx.size.x, ctx.size.y) * 0.45f);
                SpawnRipple(ctx, layer, pos, Argb(220, m_p.ripple), d, 0.0f);
                SpawnRipple(ctx, layer, pos, Argb(150, m_p.cursor), d * 0.7f, 0.15f);
            }

        private:
            const wchar_t* m_name;
            AuroraPalette m_p;
            Rng m_rng;
        };

        // ------------------------------------------------------------------------------------------
        // Fireflies: deep night gradient, nebula glows, hundreds of twinkling sparks that swarm toward
        // the cursor, and a spark burst on click.
        // ------------------------------------------------------------------------------------------
        class FirefliesEffect final : public Effect
        {
        public:
            const wchar_t* Name() const override { return L"Fireflies"; }

            void Build(SceneContext const& ctx, wuc::ContainerVisual const& layer) override
            {
                auto const& c = ctx.compositor;
                float W = ctx.size.x, H = ctx.size.y;

                auto sky = c.CreateSpriteVisual();
                sky.Size(ctx.size);
                auto bg = c.CreateRadialGradientBrush();
                bg.EllipseCenter(float2(0.5f, 0.7f));
                bg.EllipseRadius(float2(0.85f, 0.95f));
                auto inner = c.CreateColorGradientStop(0.0f, Argb(255, 0x1D2B53));
                bg.ColorStops().Append(inner);
                bg.ColorStops().Append(c.CreateColorGradientStop(0.55f, Argb(255, 0x0B1026)));
                bg.ColorStops().Append(c.CreateColorGradientStop(1.0f, Argb(255, 0x04050C)));
                inner.StartAnimation(L"Color", CycleColors(c, { Argb(255, 0x1D2B53), Argb(255, 0x2B1D53), Argb(255, 0x13354A) }, 30.0f));
                sky.Brush(bg);
                layer.Children().InsertAtTop(sky);

                // Nebula glows.
                const uint32_t nebula[] = { 0x4B3B8F, 0x1F6F8B, 0x7A2E6E };
                for (auto const& mon : ctx.monitors)
                {
                    float minDim = static_cast<float>(std::min(mon.right - mon.left, mon.bottom - mon.top));
                    for (int i = 0; i < 3; ++i)
                    {
                        float d = minDim * m_rng.Next(0.6f, 0.95f);
                        float2 p = RandomPointIn(mon, m_rng);
                        float3 base(p.x - d * 0.5f, p.y - d * 0.5f, 0.0f);
                        auto glow = c.CreateSpriteVisual();
                        glow.Size(float2(d, d));
                        glow.Offset(base);
                        glow.Brush(GlowBrush(c, Argb(70, nebula[i]), 0.5f, 0.4f));
                        glow.StartAnimation(L"Offset", Drift(c, m_rng, base, d * 0.25f, 4, m_rng.Next(40.0f, 70.0f)));
                        layer.Children().InsertAtTop(glow);
                    }
                }

                // Shared brushes keep this cheap even with hundreds of sprites.
                const uint32_t sparkColors[] = { 0xFFD27A, 0xFFB347, 0xFFF3C4, 0x9BF6FF };
                std::vector<wuc::CompositionBrush> sparks;
                for (auto col : sparkColors) sparks.push_back(GlowBrush(c, Argb(255, col), 0.22f, 0.45f));

                float totalArea = 0;
                for (auto const& mon : ctx.monitors) totalArea += Area(mon);
                int count = std::clamp(static_cast<int>(totalArea / 11000.0f), 90, 320);
                for (int i = 0; i < count; ++i)
                {
                    bool bokeh = (i % 9) == 0;
                    float d = bokeh ? m_rng.Next(26.0f, 54.0f) : m_rng.Next(7.0f, 18.0f);
                    float2 p = RandomPointIn(PickMonitor(ctx, m_rng), m_rng);
                    float3 base(p.x - d * 0.5f, p.y - d * 0.5f, 0.0f);

                    auto anchor = c.CreateContainerVisual();
                    anchor.Size(float2(d, d));
                    anchor.Offset(base);
                    anchor.StartAnimation(L"Offset", Drift(c, m_rng, base, m_rng.Next(50.0f, 170.0f), 5, m_rng.Next(14.0f, 36.0f)));

                    auto spark = c.CreateSpriteVisual();
                    spark.Size(float2(d, d));
                    spark.Brush(sparks[m_rng.Index(sparks.size())]);
                    spark.Opacity(0.2f);
                    float hi = bokeh ? 0.35f : 1.0f;
                    spark.StartAnimation(L"Opacity", Oscillate(c, hi * 0.15f, hi, m_rng.Next(1.6f, 5.5f), m_rng.Next(0.0f, 5.0f)));

                    auto attract = c.CreateExpressionAnimation(kAttractExpression);
                    attract.SetReferenceParameter(L"anchor", anchor);
                    attract.SetReferenceParameter(L"pointer", ctx.pointer);
                    attract.SetScalarParameter(L"half", d * 0.5f);
                    attract.SetScalarParameter(L"pull", bokeh ? 0.12f : m_rng.Next(0.25f, 0.6f));
                    spark.StartAnimation(L"Offset", attract);

                    anchor.Children().InsertAtTop(spark);
                    layer.Children().InsertAtTop(anchor);
                }
                m_sparkBrush = sparks[0];

                AddCursorGlow(ctx, layer, Argb(80, 0xFFC86B), std::max(320.0f, std::min(W, H) * 0.3f));
            }

            void OnClick(SceneContext const& ctx, wuc::ContainerVisual const& layer, float2 pos) override
            {
                auto const& c = ctx.compositor;
                SpawnRipple(ctx, layer, pos, Argb(170, 0xFFD27A), 360.0f, 0.0f);

                auto easeOut = c.CreateCubicBezierEasingFunction(float2(0.05f, 0.7f), float2(0.25f, 1.0f));
                auto burst = c.CreateContainerVisual();
                layer.Children().InsertAtTop(burst);
                auto batch = c.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
                for (int i = 0; i < 28; ++i)
                {
                    float d = m_rng.Next(8.0f, 16.0f);
                    float angle = m_rng.Next(0.0f, 6.2831853f);
                    float dist = m_rng.Next(80.0f, 280.0f);
                    float3 from(pos.x - d * 0.5f, pos.y - d * 0.5f, 0.0f);
                    float3 to = from + float3(std::cos(angle) * dist, std::sin(angle) * dist, 0.0f);

                    auto spark = c.CreateSpriteVisual();
                    spark.Size(float2(d, d));
                    spark.Brush(m_sparkBrush);
                    spark.Offset(from);
                    burst.Children().InsertAtTop(spark);

                    float secs = m_rng.Next(0.8f, 1.5f);
                    auto move = c.CreateVector3KeyFrameAnimation();
                    move.InsertKeyFrame(0.0f, from);
                    move.InsertKeyFrame(1.0f, to, easeOut);
                    move.Duration(Seconds(secs));
                    auto fade = c.CreateScalarKeyFrameAnimation();
                    fade.InsertKeyFrame(0.0f, 1.0f);
                    fade.InsertKeyFrame(1.0f, 0.0f);
                    fade.Duration(Seconds(secs));
                    spark.StartAnimation(L"Offset", move);
                    spark.StartAnimation(L"Opacity", fade);
                }
                batch.End();
                batch.Completed([layer, burst](auto&&, auto&&)
                {
                    layer.Children().Remove(burst);
                    burst.Children().RemoveAll();
                    burst.Close();
                });
            }

        private:
            Rng m_rng;
            wuc::CompositionBrush m_sparkBrush{ nullptr };
        };
    }

    std::vector<std::unique_ptr<Effect>> CreateEffects()
    {
        std::vector<std::unique_ptr<Effect>> effects;
        effects.push_back(std::make_unique<AuroraEffect>(L"Aurora", AuroraPalette{
            { 0x06122E, 0x0E3B5C, 0x1B0F3B, 0x0B2A3F },
            { 0x2EF2B0, 0x3FA9F5, 0x9B5CF6 },
            { 0x22D3EE, 0x8B5CF6, 0xEC4899, 0x10B981, 0x3B82F6 },
            0xBDF4FF, 0x7DF9FF }));
        effects.push_back(std::make_unique<FirefliesEffect>());
        effects.push_back(std::make_unique<AuroraEffect>(L"Ember", AuroraPalette{
            { 0x1A0612, 0x4A1420, 0x2B0A2E, 0x3D1A0A },
            { 0xFF7A3D, 0xFF3D7F, 0xFFC23D },
            { 0xF97316, 0xEF4444, 0xF59E0B, 0xDB2777, 0xFB7185 },
            0xFFD6A5, 0xFFB86B }));
        return effects;
    }
}
