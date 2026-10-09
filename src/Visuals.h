#pragma once
#include "pch.h"
#include "Scene.h"
#include "Util.h"

// Small composition building blocks shared by the wallpaper effects and the ambient layer.
namespace ld
{
    using winrt::Windows::UI::Color;

    // Soft "blurred" disc without a blur effect: radial gradient from `color` to transparent.
    wuc::CompositionRadialGradientBrush GlowBrush(wuc::Compositor const& c, Color color, float midStop = 0.4f, float midAlpha = 0.35f);
    // Transparent center, bright band, transparent edge.
    wuc::CompositionRadialGradientBrush RingBrush(wuc::Compositor const& c, Color color);
    // Looping wander around `base` that ends where it starts.
    wuc::Vector3KeyFrameAnimation Drift(wuc::Compositor const& c, Rng& rng, float3 base, float range, int keys, float seconds);
    wuc::ScalarKeyFrameAnimation Oscillate(wuc::Compositor const& c, float from, float to, float seconds, float delay = 0.0f);
    wuc::ColorKeyFrameAnimation CycleColors(wuc::Compositor const& c, std::initializer_list<Color> colors, float seconds);
    // Expanding ring that removes itself when done.
    void SpawnRipple(SceneContext const& ctx, wuc::ContainerVisual const& layer, float2 pos, Color color, float diameter, float delay);
}
