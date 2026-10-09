#include "pch.h"
#include "Visuals.h"

using namespace std::chrono_literals;

namespace ld
{
    wuc::CompositionRadialGradientBrush GlowBrush(wuc::Compositor const& c, Color color, float midStop, float midAlpha)
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

    wuc::ScalarKeyFrameAnimation Oscillate(wuc::Compositor const& c, float from, float to, float seconds, float delay)
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
}
