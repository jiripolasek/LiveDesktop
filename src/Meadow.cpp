#include "pch.h"
#include "Scene.h"
#include "Util.h"
#include "Visuals.h"
#include "Graphics.h"
#include "Log.h"

// ------------------------------------------------------------------------------------------------
// Meadow: a living landscape that follows the real time of day. Layered hills with atmospheric
// haze, a sky that moves through night, blue hour, golden hour and day, drifting clouds, stars and
// shooting stars, and hundreds of grass tufts and wildflowers swaying in traveling gusts of wind.
// The cursor brushes through the grass, a click sends a gust ring and releases dandelion seeds.
//
// Everything that moves is a composition animation (DWM evaluates it); the CPU only re-tints the
// palette every few seconds and redraws the small grass textures when the light changes.
// ------------------------------------------------------------------------------------------------
namespace ld
{
    namespace
    {
        using winrt::Windows::UI::Color;
        constexpr float kPi = 3.14159265f;
        constexpr float kTau = 6.2831853f;

        // ---------------------------------------------------------------- color

        struct Rgbf { float r, g, b; };
        Rgbf Unpack(uint32_t c) { return { ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f }; }
        uint32_t Pack(Rgbf c)
        {
            auto ch = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
            return (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
        }
        uint32_t MixRgb(uint32_t a, uint32_t b, float t)
        {
            Rgbf x = Unpack(a), y = Unpack(b);
            return Pack({ x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t });
        }
        uint32_t ScaleRgb(uint32_t a, float k)
        {
            Rgbf x = Unpack(a);
            return Pack({ x.r * k, x.g * k, x.b * k });
        }
        uint32_t Warm(uint32_t a)
        {
            Rgbf x = Unpack(a);
            return Pack({ x.r * 1.3f + 0.05f, x.g * 1.04f + 0.02f, x.b * 0.72f });
        }
        float Diff(uint32_t a, uint32_t b)
        {
            Rgbf x = Unpack(a), y = Unpack(b);
            return 255.0f * std::max({ std::abs(x.r - y.r), std::abs(x.g - y.g), std::abs(x.b - y.b) });
        }
        Color WithAlpha(uint32_t rgb, float a)
        {
            return Argb(static_cast<uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f)), rgb);
        }
        float Smooth(float t)
        {
            t = std::clamp(t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // ---------------------------------------------------------------- looks (palette per light)

        enum Slot
        {
            SkyTop, SkyUpper, SkyLower, SkyHorizon, SunGlow, SunCore, HorizonGlow, Cloud,
            FarTop, FarBottom, MidTop, MidBottom, NearTop, NearBottom, Mist,
            Grass0Base, Grass0Tip, Grass1Base, Grass1Tip, Grass2Base, Grass2Tip, Grass3Base, Grass3Tip,
            SlotCount
        };
        enum Level { SunAlpha, HorizonAlpha, CloudAlpha, MistAlpha, Stars, Fireflies, Pollen, FlowerLight, CursorAlpha, LevelCount };

        struct Look
        {
            std::array<uint32_t, SlotCount> color{};
            std::array<float, LevelCount> level{};
        };

        Look Blend(Look const& a, Look const& b, float t)
        {
            Look r;
            for (int i = 0; i < SlotCount; ++i) r.color[i] = MixRgb(a.color[i], b.color[i], t);
            for (int i = 0; i < LevelCount; ++i) r.level[i] = a.level[i] + (b.level[i] - a.level[i]) * t;
            return r;
        }

        float LookDistance(Look const& a, Look const& b, int firstSlot = 0, int lastSlot = SlotCount - 1)
        {
            float d = 0;
            for (int i = firstSlot; i <= lastSlot; ++i) d = std::max(d, Diff(a.color[i], b.color[i]));
            return d;
        }

        float LevelDistance(Look const& a, Look const& b)
        {
            float d = 0;
            for (int i = 0; i < LevelCount; ++i) d = std::max(d, std::abs(a.level[i] - b.level[i]));
            return d;
        }

        const Look kNight{
            { 0x050816, 0x0B1230, 0x17204A, 0x26315E, 0x9FB4FF, 0xEEF0FF, 0x2A3570, 0x2E3760,
              0x1A2248, 0x222B56, 0x111933, 0x17203F, 0x0C1426, 0x05080F, 0x404A7A,
              0x0A1222, 0x1E2C46, 0x08101C, 0x1D3040, 0x060C16, 0x1B3238, 0x04080E, 0x1A3530 },
            { 0.45f, 0.25f, 0.35f, 0.20f, 1.0f, 1.0f, 0.0f, 0.30f, 0.20f } };

        const Look kBlueDawn{
            { 0x17214C, 0x37427C, 0x8070A8, 0xE89C8E, 0xFFB08A, 0xFFE2C8, 0xFF9C80, 0xC890AA,
              0x5A5786, 0x6E6A98, 0x3C3A64, 0x4A4672, 0x262A42, 0x12151F, 0xC0A0C0,
              0x1C2232, 0x4A5470, 0x161C28, 0x45585E, 0x10161E, 0x3F5A4E, 0x0B1016, 0x3E5E44 },
            { 0.55f, 0.60f, 0.55f, 0.40f, 0.35f, 0.5f, 0.1f, 0.55f, 0.14f } };

        Look MakeBlueDusk()
        {
            Look l = kBlueDawn;
            l.color[SkyLower] = 0x9A6A9E;
            l.color[SkyHorizon] = 0xF2876A;
            l.color[HorizonGlow] = 0xFF8060;
            l.color[Cloud] = 0xD88C9C;
            l.color[Mist] = 0xB890B0;
            return l;
        }
        const Look kBlueDusk = MakeBlueDusk();

        const Look kGoldenDawn{
            { 0x3567A8, 0x7FA2CF, 0xF0C8A6, 0xFFC08A, 0xFFC88A, 0xFFF1D8, 0xFFB070, 0xFFDCC4,
              0xA698B4, 0xC4ABB0, 0x7E8473, 0x968E78, 0x58723F, 0x2A3A1C, 0xFFE4CC,
              0x3F5530, 0xA9B070, 0x34482A, 0xB5BC66, 0x2A3D20, 0xC2C866, 0x1E2E16, 0xD4D46C },
            { 0.95f, 0.80f, 0.70f, 0.45f, 0.0f, 0.08f, 1.0f, 0.92f, 0.08f } };

        Look MakeGoldenDusk()
        {
            Look l = kGoldenDawn;
            l.color[SkyUpper] = 0x8C96C6;
            l.color[SkyLower] = 0xF2AE8A;
            l.color[SkyHorizon] = 0xFF9E62;
            l.color[SunGlow] = 0xFFAA66;
            l.color[SunCore] = 0xFFE6C0;
            l.color[HorizonGlow] = 0xFF9050;
            l.color[Cloud] = 0xFFC6A8;
            l.color[FarTop] = 0xA08AAE;
            l.color[FarBottom] = 0xC49AA0;
            l.color[Mist] = 0xFFD0B0;
            return l;
        }
        const Look kGoldenDusk = MakeGoldenDusk();

        const Look kDay{
            { 0x3B78D0, 0x6AA4E8, 0xA9D0F2, 0xDCEDF7, 0xFFF2D0, 0xFFFCF0, 0xFFF6E6, 0xFFFFFF,
              0x8FAFCB, 0xA9C4D6, 0x6E9C86, 0x86AE94, 0x5C8E46, 0x2E5222, 0xEAF4FA,
              0x3E6E38, 0x86B860, 0x346230, 0x92C45C, 0x2A5426, 0x9DCE5C, 0x1E4419, 0xAAD860 },
            { 0.70f, 0.15f, 0.80f, 0.25f, 0.0f, 0.0f, 0.7f, 1.0f, 0.05f } };

        // ---------------------------------------------------------------- sun model

        struct Sun
        {
            float elevation = 0;   // -1 (midnight) .. 0 (horizon) .. 1 (high noon)
            float progress = 0;    // 0..1 across the day (sun) or the night (moon)
            bool day = true;
            bool dusk = false;     // evening side of the day/night
        };

        float DayLength(int dayOfYear) { return 12.0f + 3.0f * std::cos(kTau * (dayOfYear - 172) / 365.0f); }
        constexpr float kSolarNoon = 12.75f; // rough local solar noon (daylight saving included)

        int DayOfYear(SYSTEMTIME const& t)
        {
            static const int cumulative[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
            return cumulative[std::clamp<int>(t.wMonth, 1, 12) - 1] + t.wDay;
        }

        Sun SunAt(float hour, int dayOfYear)
        {
            float len = DayLength(dayOfYear);
            float rise = kSolarNoon - len * 0.5f, set = kSolarNoon + len * 0.5f;
            Sun s;
            if (hour >= rise && hour <= set)
            {
                s.progress = (hour - rise) / len;
                s.elevation = std::sin(kPi * s.progress);
                s.day = true;
                s.dusk = s.progress > 0.5f;
            }
            else
            {
                float since = hour > set ? hour - set : hour + 24.0f - set;
                s.progress = since / (24.0f - len);
                s.elevation = -std::sin(kPi * s.progress);
                s.day = false;
                s.dusk = s.progress < 0.5f;
            }
            return s;
        }

        Look LookFor(Sun const& sun)
        {
            float e = sun.elevation;
            Look const& blue = sun.dusk ? kBlueDusk : kBlueDawn;
            Look const& golden = sun.dusk ? kGoldenDusk : kGoldenDawn;
            if (e <= -0.30f) return kNight;
            if (e <= -0.06f) return Blend(kNight, blue, Smooth((e + 0.30f) / 0.24f));
            if (e <= 0.08f) return Blend(blue, golden, Smooth((e + 0.06f) / 0.14f));
            if (e <= 0.40f) return Blend(golden, kDay, Smooth((e - 0.08f) / 0.32f));
            return kDay;
        }

        const wchar_t* PhaseName(Sun const& sun)
        {
            float e = sun.elevation;
            if (e <= -0.30f) return L"night";
            if (e <= -0.06f) return sun.dusk ? L"dusk (blue hour)" : L"dawn (blue hour)";
            if (e <= 0.08f) return sun.dusk ? L"sunset" : L"sunrise";
            if (e <= 0.40f) return L"golden hour";
            return L"day";
        }

        // ---------------------------------------------------------------- shapes

        // A smooth ridge line: three layered sines (monitor-local x -> y).
        struct Ridge
        {
            float base = 0;
            float a[3]{}, k[3]{}, p[3]{};
            float At(float x) const
            {
                return base - (a[0] * std::sin(x * k[0] + p[0]) + a[1] * std::sin(x * k[1] + p[1]) + a[2] * std::sin(x * k[2] + p[2]));
            }
            float Top() const { return base - (a[0] + a[1] + a[2]); }
        };

        Ridge MakeRidge(Rng& rng, float base, float W, float a0, float a1, float a2, float w0, float w1, float w2)
        {
            Ridge r;
            r.base = base;
            r.a[0] = a0; r.a[1] = a1; r.a[2] = a2;
            r.k[0] = kTau / (W * w0); r.k[1] = kTau / (W * w1); r.k[2] = kTau / (W * w2);
            for (auto& p : r.p) p = rng.Next(0.0f, kTau);
            return r;
        }

        winrt::com_ptr<ID2D1PathGeometry> NewPath(ID2D1DeviceContext* dc)
        {
            winrt::com_ptr<ID2D1Factory> factory;
            dc->GetFactory(factory.put());
            winrt::com_ptr<ID2D1PathGeometry> path;
            factory->CreatePathGeometry(path.put());
            return path;
        }

        winrt::com_ptr<ID2D1LinearGradientBrush> LinearBrush(ID2D1DeviceContext* dc, D2D1_POINT_2F from, D2D1_POINT_2F to,
            uint32_t c0, uint32_t c1, float a0 = 1.0f, float a1 = 1.0f)
        {
            D2D1_GRADIENT_STOP stops[2] = { { 0.0f, D2DColor(c0, a0) }, { 1.0f, D2DColor(c1, a1) } };
            winrt::com_ptr<ID2D1GradientStopCollection> collection;
            winrt::com_ptr<ID2D1LinearGradientBrush> brush;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, collection.put())))
                dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to), collection.get(), brush.put());
            return brush;
        }

        // One grass blade: a tapered, curved sliver from (bx, h) to the tip.
        void FillBlade(ID2D1DeviceContext* dc, float bx, float baseY, float height, float lean, float width, ID2D1Brush* brush)
        {
            auto path = NewPath(dc);
            winrt::com_ptr<ID2D1GeometrySink> sink;
            if (!path || FAILED(path->Open(sink.put()))) return;
            D2D1_POINT_2F tip = D2D1::Point2F(bx + lean, baseY - height);
            sink->BeginFigure(D2D1::Point2F(bx - width * 0.5f, baseY), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(bx - width * 0.25f + lean * 0.12f, baseY - height * 0.58f), tip));
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(bx + width * 0.3f + lean * 0.2f, baseY - height * 0.5f), D2D1::Point2F(bx + width * 0.5f, baseY)));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            dc->FillGeometry(path.get(), brush);
        }

        D2D1_POINT_2F Bezier(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, float s)
        {
            float u = 1.0f - s;
            return D2D1::Point2F(u * u * a.x + 2 * u * s * b.x + s * s * c.x, u * u * a.y + 2 * u * s * b.y + s * s * c.y);
        }

        void StrokeCurve(ID2D1DeviceContext* dc, D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, float width, ID2D1Brush* brush)
        {
            auto path = NewPath(dc);
            winrt::com_ptr<ID2D1GeometrySink> sink;
            if (!path || FAILED(path->Open(sink.put()))) return;
            sink->BeginFigure(a, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(b, c));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            dc->DrawGeometry(path.get(), brush, width);
        }

        void FillRotatedEllipse(ID2D1DeviceContext* dc, D2D1_POINT_2F center, float rx, float ry, float degrees, ID2D1Brush* brush)
        {
            D2D1_MATRIX_3X2_F old;
            dc->GetTransform(&old);
            dc->SetTransform(D2D1::Matrix3x2F::Rotation(degrees, center) * *D2D1::Matrix3x2F::ReinterpretBaseType(&old));
            dc->FillEllipse(D2D1::Ellipse(center, rx, ry), brush);
            dc->SetTransform(old);
        }

        // Depth rows of grass, back (0) to front (3). Sizes are fractions of the monitor height.
        struct RowSpec
        {
            float t;        // 0 = on the ground ridge, 1 = bottom edge of the screen
            float h, w;     // tuft size
            float spacing;  // average distance between tufts
            float amp;      // wind sway (radians)
            float parallax; // cursor parallax factor
            float bend;     // cursor brushing (radians)
            int blades;
            float bladeW;
        };
        constexpr RowSpec kRows[4] = {
            { 0.00f, 0.036f, 0.030f, 0.0115f, 0.08f, 0.020f, 0.30f, 9, 0.0028f },
            { 0.30f, 0.066f, 0.045f, 0.0140f, 0.13f, 0.024f, 0.42f, 8, 0.0042f },
            { 0.62f, 0.096f, 0.055f, 0.0170f, 0.17f, 0.028f, 0.52f, 8, 0.0055f },
            { 1.00f, 0.136f, 0.065f, 0.0210f, 0.21f, 0.032f, 0.60f, 7, 0.0072f },
        };
        constexpr int kTuftVariants = 8;

        void DrawTuft(ID2D1DeviceContext* dc, float2 size, uint32_t seed, int blades, float bladeW, uint32_t base, uint32_t tip, bool seedHead)
        {
            Rng rng(seed);
            float w = size.x, h = size.y;
            for (int i = 0; i < blades; ++i)
            {
                float depth = blades > 1 ? static_cast<float>(i) / (blades - 1) : 1.0f;
                float bx = w * 0.5f + rng.Next(-0.26f, 0.26f) * w;
                float height = h * rng.Next(0.5f, 1.0f) * (0.88f + 0.12f * depth);
                float lean = rng.Next(-0.3f, 0.3f) * height;
                lean = std::clamp(bx + lean, 2.0f, w - 2.0f) - bx;
                float bw = bladeW * rng.Next(0.75f, 1.25f);
                float dry = rng.Next(0.0f, 1.0f) < 0.14f ? rng.Next(0.25f, 0.6f) : 0.0f;
                float shade = (1.0f - depth) * 0.45f;
                uint32_t b = MixRgb(base, 0x000000, shade * 0.35f);
                uint32_t t = MixRgb(tip, base, shade);
                if (dry > 0) t = MixRgb(t, Warm(tip), dry);
                auto brush = LinearBrush(dc, D2D1::Point2F(bx, h), D2D1::Point2F(bx + lean, h - height), b, t);
                if (brush) FillBlade(dc, bx, h + 1.0f, height, lean, bw, brush.get());
            }
            if (seedHead)
            {
                // A wild-oat stalk with a few seeds near its tip.
                float bx = w * 0.5f + rng.Next(-0.15f, 0.15f) * w;
                float height = h * rng.Next(0.95f, 1.0f) - bladeW * 2;
                float lean = rng.Next(-0.2f, 0.2f) * w;
                D2D1_POINT_2F a = D2D1::Point2F(bx, h + 1), c = D2D1::Point2F(bx + lean, h - height);
                D2D1_POINT_2F b = D2D1::Point2F(bx + lean * 0.1f, h - height * 0.5f);
                uint32_t stalk = MixRgb(tip, Warm(tip), 0.3f);
                auto brush = LinearBrush(dc, a, c, base, stalk, 1.0f, 0.85f);
                if (brush) StrokeCurve(dc, a, b, c, std::max(1.0f, bladeW * 0.24f), brush.get());
                winrt::com_ptr<ID2D1SolidColorBrush> seedBrush;
                dc->CreateSolidColorBrush(D2DColor(stalk, 0.7f), seedBrush.put());
                for (int j = 0; j < 5; ++j)
                {
                    // Drooping spikelets hanging off the top of the stalk.
                    float s = 0.80f + 0.045f * j;
                    auto p = Bezier(a, b, c, s);
                    float side = (j % 2) ? 1.0f : -1.0f;
                    FillRotatedEllipse(dc, D2D1::Point2F(p.x + side * bladeW * 0.7f, p.y + bladeW * 0.5f), bladeW * 0.24f, bladeW * 0.75f, side * 38.0f, seedBrush.get());
                }
            }
        }

        enum FlowerKind { Daisy, Poppy, Cornflower, Buttercup, Lavender, FlowerKinds };

        void DrawFlower(ID2D1DeviceContext* dc, float2 size, uint32_t seed, int kind, uint32_t stemBase, uint32_t stemTip, float light, uint32_t nightTint)
        {
            Rng rng(seed);
            float w = size.x, h = size.y;
            float head = w * 0.3f;
            float lean = rng.Next(-0.16f, 0.16f) * w;
            D2D1_POINT_2F a = D2D1::Point2F(w * 0.5f, h + 1);
            D2D1_POINT_2F c = D2D1::Point2F(w * 0.5f + lean, kind == Lavender ? h * 0.05f : head * 1.25f);
            D2D1_POINT_2F b = D2D1::Point2F(w * 0.5f + lean * 0.1f, h * 0.5f);
            uint32_t tip = MixRgb(stemTip, stemBase, 0.25f);

            if (auto brush = LinearBrush(dc, a, c, stemBase, tip)) StrokeCurve(dc, a, b, c, std::max(1.2f, w * 0.045f), brush.get());
            for (int i = 0; i < 2; ++i)
            {
                float lh = h * rng.Next(0.22f, 0.36f);
                float ll = (i ? 1.0f : -1.0f) * w * rng.Next(0.18f, 0.32f);
                if (auto leaf = LinearBrush(dc, a, D2D1::Point2F(a.x + ll, a.y - lh), stemBase, tip)) FillBlade(dc, a.x, a.y, lh, ll, w * 0.1f, leaf.get());
            }

            auto lit = [&](uint32_t col) { return MixRgb(ScaleRgb(col, 0.35f + 0.65f * light), nightTint, (1.0f - light) * 0.35f); };
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(D2DColor(0xFFFFFF, 1.0f), brush.put());
            auto fill = [&](uint32_t col, float alpha = 1.0f) { brush->SetColor(D2DColor(lit(col), alpha)); return brush.get(); };
            float spin = rng.Next(0.0f, 360.0f);
            switch (kind)
            {
            case Daisy:
                for (int k = 0; k < 12; ++k)
                {
                    float deg = spin + k * 30.0f, rad = deg * kPi / 180.0f;
                    D2D1_POINT_2F p = D2D1::Point2F(c.x + std::cos(rad) * head * 0.55f, c.y + std::sin(rad) * head * 0.55f * 0.8f);
                    FillRotatedEllipse(dc, p, head * 0.5f, head * 0.17f, deg, fill(k % 2 ? 0xEDE8DA : 0xFAF7EE));
                }
                dc->FillEllipse(D2D1::Ellipse(c, head * 0.3f, head * 0.27f), fill(0xF2B824));
                break;
            case Poppy:
                for (int k = 0; k < 5; ++k)
                {
                    float rad = (spin + k * 72.0f) * kPi / 180.0f;
                    D2D1_POINT_2F p = D2D1::Point2F(c.x + std::cos(rad) * head * 0.36f, c.y + std::sin(rad) * head * 0.32f);
                    dc->FillEllipse(D2D1::Ellipse(p, head * 0.56f, head * 0.5f), fill(k % 2 ? 0xE8452F : 0xD43A2A, 0.94f));
                }
                dc->FillEllipse(D2D1::Ellipse(c, head * 0.2f, head * 0.18f), fill(0x2B1B1E));
                break;
            case Cornflower:
                for (int k = 0; k < 10; ++k)
                {
                    float deg = spin + k * 36.0f, rad = deg * kPi / 180.0f;
                    D2D1_POINT_2F p = D2D1::Point2F(c.x + std::cos(rad) * head * 0.5f, c.y + std::sin(rad) * head * 0.42f);
                    FillRotatedEllipse(dc, p, head * 0.5f, head * 0.14f, deg, fill(0x5784E6));
                }
                for (int k = 0; k < 6; ++k)
                {
                    float deg = spin + 15.0f + k * 60.0f, rad = deg * kPi / 180.0f;
                    D2D1_POINT_2F p = D2D1::Point2F(c.x + std::cos(rad) * head * 0.22f, c.y + std::sin(rad) * head * 0.2f);
                    FillRotatedEllipse(dc, p, head * 0.24f, head * 0.1f, deg, fill(0x3D5FC4));
                }
                dc->FillEllipse(D2D1::Ellipse(c, head * 0.13f, head * 0.12f), fill(0x2A3570));
                break;
            case Buttercup:
                for (int k = 0; k < 5; ++k)
                {
                    float rad = (spin + k * 72.0f) * kPi / 180.0f;
                    D2D1_POINT_2F p = D2D1::Point2F(c.x + std::cos(rad) * head * 0.34f, c.y + std::sin(rad) * head * 0.3f);
                    dc->FillEllipse(D2D1::Ellipse(p, head * 0.42f, head * 0.38f), fill(0xF7D12E));
                }
                dc->FillEllipse(D2D1::Ellipse(c, head * 0.2f, head * 0.18f), fill(0xD99A18));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - head * 0.25f, c.y - head * 0.22f), head * 0.12f, head * 0.08f), fill(0xFFF6C0, 0.8f));
                break;
            default: // Lavender: florets along the top of the stem
                for (int k = 0; k < 14; ++k)
                {
                    float s = 0.64f + 0.026f * k;
                    auto p = Bezier(a, b, c, s);
                    float side = (k % 2) ? 1.0f : -1.0f;
                    FillRotatedEllipse(dc, D2D1::Point2F(p.x + side * w * 0.06f, p.y), w * 0.065f, w * 0.1f, side * 25.0f, fill(k % 3 ? 0x9A7AD6 : 0xB89AEC));
                }
                break;
            }
        }

        // Fireflies drift around an anchor and are drawn toward the cursor.
        constexpr wchar_t kAttract[] =
            L"(pointer.Position - anchor.Offset - org - Vector3(half, half, 0.0))"
            L" * (pull * pointer.Strength"
            L"    * Max(0.0, 1.0 - Length(pointer.Position - anchor.Offset - org - Vector3(half, half, 0.0)) / (pointer.Radius * 1.1)))";
        // Pollen is nudged away from the cursor.
        constexpr wchar_t kRepel[] =
            L"(anchor.Offset + org + Vector3(half, half, 0.0) - pointer.Position)"
            L" * (push * pointer.Strength"
            L"    * Max(0.0, 1.0 - Length(anchor.Offset + org + Vector3(half, half, 0.0) - pointer.Position) / (pointer.Radius * 0.6))"
            L"    * 60.0 / Max(Length(anchor.Offset + org + Vector3(half, half, 0.0) - pointer.Position), 60.0))";

        // Grass/flower rotation around the base: traveling gusts + flutter, the cursor parting the
        // blades, and a click gust ring expanding outward. Angles in radians, positive = lean right.
        constexpr wchar_t kSway[] =
            L"lean + amp * wind.Strength * ("
            L"    (0.5 + 0.5 * Sin(wind.Phase * 47.0 - x * 0.0057)) * (0.55 + 0.45 * Sin(wind.Phase * 11.0 - x * 0.0024)) * 0.9"
            L"    + 0.16 * Sin(wind.Phase * 89.0 + seed) + 0.06 * Sin(wind.Phase * 151.0 + seed * 1.7))"
            L" + bend * pointer.Strength"
            L"    * ((x + row.Offset.X - pointer.Position.X) / Max(Abs(x + row.Offset.X - pointer.Position.X), 1.0))"
            L"    * Pow(Clamp(1.0 - Abs(x + row.Offset.X - pointer.Position.X) / reach, 0.0, 1.0), 2.0)"
            L"    * Clamp(1.0 - Abs(pointer.Position.Y - midY) / reachY, 0.0, 1.0)"
            L" + gust * ((x - wind.GustX) / Max(Abs(x - wind.GustX), 1.0))"
            L"    * Clamp(1.0 - Abs(Abs(x - wind.GustX) - wind.GustT * 1800.0) / 220.0, 0.0, 1.0) * (1.0 - wind.GustT)";

        class MeadowEffect final : public Effect
        {
        public:
            const wchar_t* Name() const override { return L"Meadow"; }
            void Build(SceneContext const& ctx, wuc::ContainerVisual const& layer) override;
            void OnClick(SceneContext const& ctx, wuc::ContainerVisual const& layer, float2 pos) override;
            void Tick(SceneContext const& ctx, wuc::ContainerVisual const& layer, bool jump) override;

        private:
            using ColorFn = std::function<Color(Look const&)>;
            using LevelFn = std::function<float(Look const&)>;
            struct ColorBinding { wuc::CompositionObject target{ nullptr }; ColorFn get; };
            struct OpacityBinding { wuc::Visual target{ nullptr }; LevelFn get; };
            struct Texture
            {
                wuc::CompositionDrawingSurface surface{ nullptr };
                float2 size{};
                int row = 0;
                int flower = -1;
                uint32_t seed = 0;
                bool seedHead = false;
            };
            struct Sky
            {
                RECT rc{};
                wuc::SpriteVisual glow{ nullptr }, core{ nullptr }, horizon{ nullptr };
                float glowD = 0, coreD = 0;
                float2 horizonSize{};
                wuc::ContainerVisual fx{ nullptr };
            };

            static ColorFn Col(int slot, float alpha = 1.0f, int level = -1)
            {
                return [=](Look const& l) { return WithAlpha(l.color[slot], alpha * (level >= 0 ? l.level[level] : 1.0f)); };
            }
            wuc::CompositionColorGradientStop Stop(float offset, ColorFn get);
            wuc::CompositionRadialGradientBrush Radial(std::initializer_list<std::pair<float, ColorFn>> stops);
            void BindOpacity(wuc::Visual const& v, LevelFn get);
            Sun CurrentSun(SceneContext const& ctx) const;
            void ApplyLook(Look const& look, float seconds);
            void DrawTextures(Look const& look);
            float3 LuminaryCenter(RECT const& rc, Sun const& sun) const;
            void PlaceLuminaries(Sun const& sun, float seconds);

            void BuildSky(SceneContext const& ctx, wuc::ContainerVisual const& layer, RECT const& rc);
            void BuildHills(SceneContext const& ctx, wuc::ContainerVisual const& layer, RECT const& rc, Ridge& ground);
            void BuildGrass(SceneContext const& ctx, wuc::ContainerVisual const& layer, RECT const& rc, Ridge const& ground);
            void BuildCreatures(SceneContext const& ctx, wuc::ContainerVisual const& layer, RECT const& rc, Ridge const& ground);
            void SpawnShootingStar(Sky const& sky);

            wuc::Compositor m_c{ nullptr };
            Graphics* m_graphics = nullptr;
            Rng m_rng;
            Look m_look{};
            Look m_drawn{};
            std::wstring m_phase;
            bool m_wasDay = true;
            float m_scaleH = 1.0f;
            std::vector<ColorBinding> m_colors;
            std::vector<OpacityBinding> m_opacities;
            std::vector<Texture> m_textures;
            std::vector<Sky> m_skies;
            wuc::CompositionPropertySet m_wind{ nullptr };
            wuc::ContainerVisual m_fx{ nullptr };
            float m_sunProgress = -1.0f;
            wuc::CompositionBrush m_seedBrush{ nullptr };
            wuc::CompositionBrush m_starTrail{ nullptr };
        };

        // ------------------------------------------------------------ bindings

        wuc::CompositionColorGradientStop MeadowEffect::Stop(float offset, ColorFn get)
        {
            auto stop = m_c.CreateColorGradientStop(offset, get(m_look));
            m_colors.push_back({ stop, std::move(get) });
            return stop;
        }

        wuc::CompositionRadialGradientBrush MeadowEffect::Radial(std::initializer_list<std::pair<float, ColorFn>> stops)
        {
            auto brush = m_c.CreateRadialGradientBrush();
            for (auto const& [offset, get] : stops) brush.ColorStops().Append(Stop(offset, get));
            return brush;
        }

        void MeadowEffect::BindOpacity(wuc::Visual const& v, LevelFn get)
        {
            v.Opacity(std::clamp(get(m_look), 0.0f, 1.0f));
            m_opacities.push_back({ v, std::move(get) });
        }

        Sun MeadowEffect::CurrentSun(SceneContext const& ctx) const
        {
            SYSTEMTIME t{};
            GetLocalTime(&t);
            float hour = ctx.timeOverride >= 0 ? ctx.timeOverride : t.wHour + t.wMinute / 60.0f + t.wSecond / 3600.0f;
            return SunAt(hour, DayOfYear(t));
        }

        void MeadowEffect::ApplyLook(Look const& look, float seconds)
        {
            for (auto const& b : m_colors)
            {
                Color col = b.get(look);
                if (seconds <= 0)
                {
                    b.target.StopAnimation(L"Color");
                    if (auto stop = b.target.try_as<wuc::CompositionColorGradientStop>()) stop.Color(col);
                    else if (auto brush = b.target.try_as<wuc::CompositionColorBrush>()) brush.Color(col);
                    continue;
                }
                auto anim = m_c.CreateColorKeyFrameAnimation();
                anim.InsertKeyFrame(1.0f, col);
                anim.Duration(Seconds(seconds));
                b.target.StartAnimation(L"Color", anim);
            }
            for (auto const& b : m_opacities)
            {
                float value = std::clamp(b.get(look), 0.0f, 1.0f);
                if (seconds <= 0)
                {
                    b.target.StopAnimation(L"Opacity");
                    b.target.Opacity(value);
                    continue;
                }
                auto anim = m_c.CreateScalarKeyFrameAnimation();
                anim.InsertKeyFrame(1.0f, value);
                anim.Duration(Seconds(seconds));
                b.target.StartAnimation(L"Opacity", anim);
            }
            m_look = look;
        }

        void MeadowEffect::DrawTextures(Look const& look)
        {
            if (!m_graphics) return;
            for (auto const& t : m_textures)
            {
                uint32_t base = look.color[Grass0Base + t.row * 2], tip = look.color[Grass0Tip + t.row * 2];
                RowSpec const& spec = kRows[t.row];
                m_graphics->Draw(t.surface, t.size, [&](ID2D1DeviceContext* dc) {
                    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                    if (t.flower >= 0)
                        DrawFlower(dc, t.size, t.seed, t.flower, base, tip, look.level[FlowerLight], look.color[SkyUpper]);
                    else
                        DrawTuft(dc, t.size, t.seed, spec.blades, spec.bladeW * m_scaleH, base, tip, t.seedHead);
                });
            }
            m_drawn = look;
        }

        float3 MeadowEffect::LuminaryCenter(RECT const& rc, Sun const& sun) const
        {
            float W = static_cast<float>(rc.right - rc.left), H = static_cast<float>(rc.bottom - rc.top);
            return float3(W * (0.1f + 0.8f * sun.progress), H * 0.66f - std::sin(kPi * sun.progress) * H * 0.48f, 0.0f);
        }

        void MeadowEffect::PlaceLuminaries(Sun const& sun, float seconds)
        {
            auto move = [&](wuc::Visual const& v, float3 to) {
                if (seconds <= 0)
                {
                    v.StopAnimation(L"Offset");
                    v.Offset(to);
                    return;
                }
                auto anim = m_c.CreateVector3KeyFrameAnimation();
                anim.InsertKeyFrame(1.0f, to);
                anim.Duration(Seconds(seconds));
                v.StartAnimation(L"Offset", anim);
            };
            for (auto const& sky : m_skies)
            {
                float3 c = LuminaryCenter(sky.rc, sun);
                float H = static_cast<float>(sky.rc.bottom - sky.rc.top);
                move(sky.glow, c - float3(sky.glowD * 0.5f, sky.glowD * 0.5f, 0));
                move(sky.core, c - float3(sky.coreD * 0.5f, sky.coreD * 0.5f, 0));
                move(sky.horizon, float3(c.x - sky.horizonSize.x * 0.5f, H * 0.6f - sky.horizonSize.y * 0.5f, 0));
                sky.core.Scale(sun.day ? float3(1, 1, 1) : float3(0.72f, 0.72f, 1));
            }
        }

        // ------------------------------------------------------------ build: sky

        void MeadowEffect::BuildSky(SceneContext const&, wuc::ContainerVisual const& root, RECT const& rc)
        {
            float W = static_cast<float>(rc.right - rc.left), H = static_cast<float>(rc.bottom - rc.top);

            auto sky = m_c.CreateSpriteVisual();
            sky.Size(float2(W, H));
            auto gradient = m_c.CreateLinearGradientBrush();
            gradient.StartPoint(float2(0, 0));
            gradient.EndPoint(float2(0, 1));
            gradient.ColorStops().Append(Stop(0.0f, Col(SkyTop)));
            gradient.ColorStops().Append(Stop(0.40f, Col(SkyUpper)));
            gradient.ColorStops().Append(Stop(0.60f, Col(SkyLower)));
            gradient.ColorStops().Append(Stop(0.70f, Col(SkyHorizon)));
            sky.Brush(gradient);
            root.Children().InsertAtTop(sky);

            // Stars and a faint milky band; the whole group fades with the light.
            auto stars = m_c.CreateContainerVisual();
            stars.Size(float2(W, H));
            BindOpacity(stars, [](Look const& l) { return l.level[Stars]; });
            root.Children().InsertAtTop(stars);
            auto band = m_c.CreateSpriteVisual();
            band.Size(float2(W * 1.1f, H * 0.22f));
            band.Offset(float3(-W * 0.05f, H * 0.12f, 0));
            band.CenterPoint(float3(W * 0.55f, H * 0.11f, 0));
            band.RotationAngleInDegrees(-11.0f);
            band.Brush(GlowBrush(m_c, Argb(26, 0xB8C4FF), 0.45f, 0.4f));
            stars.Children().InsertAtTop(band);
            int count = static_cast<int>(180 * W / 3440.0f + 60);
            for (int i = 0; i < count; ++i)
            {
                auto star = m_c.CreateSpriteVisual();
                float y = H * std::pow(m_rng.Next(0.0f, 1.0f), 1.4f) * 0.56f;
                float big = m_rng.Next(0.0f, 1.0f);
                float d = big > 0.96f ? 3.2f : big > 0.8f ? 2.2f : 1.4f;
                star.Size(float2(d, d));
                star.Offset(float3(m_rng.Next(0.0f, W), y, 0));
                uint32_t tone = m_rng.Next(0.0f, 1.0f) < 0.2f ? 0xFFE6C8 : (m_rng.Next(0.0f, 1.0f) < 0.3f ? 0xC8D8FF : 0xFFFFFF);
                float alpha = m_rng.Next(0.35f, 1.0f) * (1.0f - y / (H * 0.7f));
                star.Brush(d > 2.0f ? static_cast<wuc::CompositionBrush>(GlowBrush(m_c, WithAlpha(tone, alpha), 0.35f, 0.5f)) : m_c.CreateColorBrush(WithAlpha(tone, alpha)));
                if (d > 2.0f) star.Size(float2(d * 2.2f, d * 2.2f));
                if (i % 5 == 0) star.StartAnimation(L"Opacity", Oscillate(m_c, 0.25f, 1.0f, m_rng.Next(2.5f, 6.0f), m_rng.Next(0.0f, 4.0f)));
                stars.Children().InsertAtTop(star);
            }

            Sky s;
            s.rc = rc;
            s.horizonSize = float2(W * 1.1f, H * 0.42f);
            s.horizon = m_c.CreateSpriteVisual();
            s.horizon.Size(s.horizonSize);
            s.horizon.Brush(Radial({ { 0.0f, Col(HorizonGlow, 0.75f, HorizonAlpha) }, { 0.45f, Col(HorizonGlow, 0.3f, HorizonAlpha) }, { 1.0f, Col(HorizonGlow, 0.0f) } }));
            root.Children().InsertAtTop(s.horizon);

            s.glowD = H * 0.62f;
            s.glow = m_c.CreateSpriteVisual();
            s.glow.Size(float2(s.glowD, s.glowD));
            s.glow.Brush(Radial({ { 0.0f, Col(SunGlow, 0.6f, SunAlpha) }, { 0.12f, Col(SunGlow, 0.35f, SunAlpha) }, { 0.4f, Col(SunGlow, 0.1f, SunAlpha) }, { 1.0f, Col(SunGlow, 0.0f) } }));
            root.Children().InsertAtTop(s.glow);

            s.coreD = H * 0.062f;
            s.core = m_c.CreateSpriteVisual();
            s.core.Size(float2(s.coreD, s.coreD));
            s.core.CenterPoint(float3(s.coreD * 0.5f, s.coreD * 0.5f, 0));
            s.core.Brush(Radial({ { 0.0f, Col(SunCore) }, { 0.5f, Col(SunCore) }, { 0.62f, Col(SunGlow, 0.45f) }, { 1.0f, Col(SunGlow, 0.0f) } }));
            root.Children().InsertAtTop(s.core);
            m_skies.push_back(s);

            // Shooting stars live above the stars, below the clouds.
            auto skyFx = m_c.CreateContainerVisual();
            skyFx.Size(float2(W, H));
            root.Children().InsertAtTop(skyFx);
            m_skies.back().fx = skyFx;

            // Clouds: soft puffs sharing one brush, sliding slowly and wrapping around the monitor.
            auto puff = Radial({ { 0.0f, Col(Cloud, 0.75f, CloudAlpha) }, { 0.45f, Col(Cloud, 0.42f, CloudAlpha) }, { 1.0f, Col(Cloud, 0.0f) } });
            for (int i = 0; i < 6; ++i)
            {
                auto cloud = m_c.CreateContainerVisual();
                float cw = W * m_rng.Next(0.14f, 0.26f), ch = H * m_rng.Next(0.045f, 0.075f);
                float y = H * m_rng.Next(0.06f, 0.36f);
                int puffs = 4 + static_cast<int>(m_rng.Index(3));
                for (int p = 0; p < puffs; ++p)
                {
                    auto part = m_c.CreateSpriteVisual();
                    float pw = cw * m_rng.Next(0.4f, 0.7f), ph = ch * m_rng.Next(0.8f, 1.6f);
                    part.Size(float2(pw, ph));
                    part.Offset(float3(m_rng.Next(0.0f, cw - pw), (ch - ph) * 0.5f + m_rng.Next(-0.2f, 0.25f) * ch, 0));
                    part.Brush(puff);
                    cloud.Children().InsertAtTop(part);
                }
                float left = -cw, right = W, span = right - left;
                float x0 = m_rng.Next(left, right);
                float secs = span / (W * m_rng.Next(0.0016f, 0.0032f)); // slow: ~5-10 min per crossing
                cloud.Offset(float3(x0, y, 0));
                auto drift = m_c.CreateScalarKeyFrameAnimation();
                auto linear = m_c.CreateLinearEasingFunction();
                float f = (right - x0) / span;
                drift.InsertKeyFrame(0.0f, x0, linear);
                drift.InsertKeyFrame(std::max(f, 0.0001f), right, linear);
                drift.InsertKeyFrame(std::min(f + 0.0001f, 1.0f), left, linear);
                drift.InsertKeyFrame(1.0f, x0, linear);
                drift.Duration(Seconds(secs));
                drift.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
                cloud.StartAnimation(L"Offset.X", drift);
                root.Children().InsertAtTop(cloud);
            }
        }

        // ------------------------------------------------------------ build: hills and mist

        void MeadowEffect::BuildHills(SceneContext const& ctx, wuc::ContainerVisual const& root, RECT const& rc, Ridge& ground)
        {
            float W = static_cast<float>(rc.right - rc.left), H = static_cast<float>(rc.bottom - rc.top);
            float cx = (rc.left + rc.right) * 0.5f;

            auto parallax = [&](wuc::Visual const& v, float baseX, float baseY, float k) {
                auto e = m_c.CreateExpressionAnimation(L"Vector3(baseX + (cx - pointer.Position.X) * k * pointer.Strength, baseY, 0.0)");
                e.SetReferenceParameter(L"pointer", ctx.pointer);
                e.SetScalarParameter(L"baseX", baseX);
                e.SetScalarParameter(L"baseY", baseY);
                e.SetScalarParameter(L"cx", cx);
                e.SetScalarParameter(L"k", k);
                v.StartAnimation(L"Offset", e);
            };

            auto hill = [&](Ridge const& ridge, int topSlot, int bottomSlot, float k, float scale) {
                float margin = std::ceil(k * W * 0.6f + 4.0f);
                float top = std::floor(ridge.Top() - 2.0f);
                float2 size(W + margin * 2.0f, H - top);
                float2 px(std::ceil(size.x * scale), std::ceil(size.y * scale));
                auto sprite = m_c.CreateSpriteVisual();
                sprite.Size(size);
                auto source = m_c.CreateLinearGradientBrush();
                source.StartPoint(float2(0, 0));
                source.EndPoint(float2(0, 1));
                source.ColorStops().Append(Stop(0.0f, Col(topSlot)));
                source.ColorStops().Append(Stop(1.0f, Col(bottomSlot)));
                auto mask = m_c.CreateMaskBrush();
                mask.Source(source);
                if (m_graphics)
                {
                    auto surface = m_graphics->CreateSurface(px);
                    m_graphics->Draw(surface, px, [&](ID2D1DeviceContext* dc) {
                        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                        auto path = NewPath(dc);
                        winrt::com_ptr<ID2D1GeometrySink> sink;
                        if (!path || FAILED(path->Open(sink.put()))) return;
                        sink->BeginFigure(D2D1::Point2F(0, px.y + 1), D2D1_FIGURE_BEGIN_FILLED);
                        for (float x = 0; x <= size.x + 8.0f; x += 6.0f)
                            sink->AddLine(D2D1::Point2F(x * scale, (ridge.At(x - margin) - top) * scale));
                        sink->AddLine(D2D1::Point2F(px.x + 1, px.y + 1));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        winrt::com_ptr<ID2D1SolidColorBrush> white;
                        dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), white.put());
                        dc->FillGeometry(path.get(), white.get());
                    });
                    auto maskSurface = m_c.CreateSurfaceBrush(surface);
                    maskSurface.Stretch(wuc::CompositionStretch::Fill);
                    maskSurface.BitmapInterpolationMode(wuc::CompositionBitmapInterpolationMode::Linear);
                    mask.Mask(maskSurface);
                }
                sprite.Brush(mask);
                parallax(sprite, -margin, top, k);
                root.Children().InsertAtTop(sprite);
            };

            auto mist = [&](float centerY, float height, float alpha, float k, float breathe) {
                auto band = m_c.CreateSpriteVisual();
                float margin = std::ceil(k * W * 0.6f + 4.0f);
                band.Size(float2(W + margin * 2.0f, height));
                auto brush = m_c.CreateLinearGradientBrush();
                brush.StartPoint(float2(0, 0));
                brush.EndPoint(float2(0, 1));
                brush.ColorStops().Append(Stop(0.0f, Col(Mist, 0.0f)));
                brush.ColorStops().Append(Stop(0.55f, Col(Mist, alpha, MistAlpha)));
                brush.ColorStops().Append(Stop(1.0f, Col(Mist, alpha * 0.4f, MistAlpha)));
                band.Brush(brush);
                parallax(band, -margin, centerY - height * 0.55f, k);
                band.StartAnimation(L"Opacity", Oscillate(m_c, 0.65f, 1.0f, breathe, m_rng.Next(0.0f, breathe)));
                root.Children().InsertAtTop(band);
            };

            Ridge distant = MakeRidge(m_rng, H * 0.585f, W, H * 0.050f, H * 0.025f, H * 0.008f, 1.3f, 0.47f, 0.13f);
            Ridge mid = MakeRidge(m_rng, H * 0.665f, W, H * 0.035f, H * 0.018f, H * 0.005f, 0.9f, 0.37f, 0.11f);
            ground = MakeRidge(m_rng, H * 0.790f, W, H * 0.022f, H * 0.010f, H * 0.003f, 1.1f, 0.41f, 0.09f);

            hill(distant, FarTop, FarBottom, 0.004f, 0.25f);
            mist(distant.base + H * 0.015f, H * 0.10f, 0.55f, 0.006f, 23.0f);
            hill(mid, MidTop, MidBottom, 0.009f, 0.33f);
            mist(mid.base + H * 0.03f, H * 0.08f, 0.38f, 0.014f, 31.0f);
            hill(ground, NearTop, NearBottom, 0.020f, 0.5f);
        }

        // ------------------------------------------------------------ build: grass and flowers

        constexpr int kFlowerRows = 2; // flowers grow in rows 2 and 3
        constexpr float kFlowerW[kFlowerRows] = { 0.022f, 0.029f };
        constexpr float kFlowerH[kFlowerRows] = { 0.105f, 0.150f };

        int TuftTexture(int row, int variant) { return row * kTuftVariants + variant; }
        int FlowerTexture(int row, int kind, int variant) { return 4 * kTuftVariants + (row - 2) * FlowerKinds * 2 + kind * 2 + variant; }

        void MeadowEffect::BuildGrass(SceneContext const& ctx, wuc::ContainerVisual const& root, RECT const& rc, Ridge const& ground)
        {
            float W = static_cast<float>(rc.right - rc.left), H = static_cast<float>(rc.bottom - rc.top);
            float cx = (rc.left + rc.right) * 0.5f;
            float f = H / m_scaleH;
            auto sway = m_c.CreateExpressionAnimation(kSway);
            sway.SetReferenceParameter(L"wind", m_wind);
            sway.SetReferenceParameter(L"pointer", ctx.pointer);

            auto place = [&](wuc::ContainerVisual const& row, int r, Texture const& tex, float x, float baseY, float scale, float amp, bool atBottom) {
                auto sprite = m_c.CreateSpriteVisual();
                float2 size(tex.size.x * f, tex.size.y * f);
                sprite.Size(size);
                sprite.Offset(float3(x - size.x * 0.5f, baseY - size.y, 0));
                sprite.CenterPoint(float3(size.x * 0.5f, size.y, 0));
                float flip = m_rng.Next(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;
                sprite.Scale(float3(flip * scale, scale, 1));
                auto brush = m_c.CreateSurfaceBrush(tex.surface);
                brush.Stretch(wuc::CompositionStretch::Fill);
                sprite.Brush(brush);
                float hostX = rc.left + x;
                sway.SetReferenceParameter(L"row", row);
                sway.SetScalarParameter(L"lean", m_rng.Next(-0.05f, 0.05f));
                sway.SetScalarParameter(L"amp", amp * m_rng.Next(0.8f, 1.2f));
                sway.SetScalarParameter(L"x", hostX);
                sway.SetScalarParameter(L"seed", m_rng.Next(0.0f, kTau));
                sway.SetScalarParameter(L"midY", rc.top + baseY - size.y * scale * 0.5f);
                sway.SetScalarParameter(L"reach", H * (0.05f + 0.025f * r));
                sway.SetScalarParameter(L"reachY", size.y * scale * 0.5f + H * 0.06f);
                sway.SetScalarParameter(L"bend", kRows[r].bend);
                sway.SetScalarParameter(L"gust", 0.18f + 0.06f * r);
                sprite.StartAnimation(L"RotationAngle", sway);
                if (atBottom) row.Children().InsertAtBottom(sprite);
                else row.Children().InsertAtTop(sprite);
            };

            for (int r = 0; r < 4; ++r)
            {
                RowSpec const& spec = kRows[r];
                auto row = m_c.CreateContainerVisual();
                row.Size(float2(W, H));
                auto parallax = m_c.CreateExpressionAnimation(L"Vector3((cx - pointer.Position.X) * k * pointer.Strength, 0.0, 0.0)");
                parallax.SetReferenceParameter(L"pointer", ctx.pointer);
                parallax.SetScalarParameter(L"cx", cx);
                parallax.SetScalarParameter(L"k", spec.parallax);
                row.StartAnimation(L"Offset", parallax);
                root.Children().InsertAtTop(row);

                float margin = spec.parallax * W * 0.6f + spec.w * H;
                auto baseAt = [&](float x) { return ground.At(x) + (H * 1.012f - ground.At(x)) * spec.t + H * 0.004f; };
                for (float x = -margin; x < W + margin; x += spec.spacing * H * m_rng.Next(0.55f, 1.45f))
                {
                    int variant = static_cast<int>(m_rng.Index(kTuftVariants));
                    place(row, r, m_textures[TuftTexture(r, variant)], x, baseAt(x) + m_rng.Next(0.0f, 0.006f) * H, m_rng.Next(0.82f, 1.15f), spec.amp, false);
                }

                if (r < 2) continue;
                // Wildflowers in loose clusters, tucked behind the blades of their row.
                int clusters = static_cast<int>(std::round(W / H * (r == 2 ? 2.6f : 1.7f)));
                for (int c = 0; c < clusters; ++c)
                {
                    float center = m_rng.Next(0.03f, 0.97f) * W;
                    int kind = static_cast<int>(m_rng.Index(FlowerKinds));
                    int count = 3 + static_cast<int>(m_rng.Index(5));
                    for (int i = 0; i < count; ++i)
                    {
                        int k = m_rng.Next(0.0f, 1.0f) < 0.8f ? kind : static_cast<int>(m_rng.Index(FlowerKinds));
                        float x = center + m_rng.Next(-1.0f, 1.0f) * H * 0.045f;
                        auto const& tex = m_textures[FlowerTexture(r, k, static_cast<int>(m_rng.Index(2)))];
                        place(row, r, tex, x, baseAt(x) + m_rng.Next(0.0f, 0.01f) * H, m_rng.Next(0.75f, 1.08f), spec.amp * 0.8f, true);
                    }
                }
            }
        }

        // ------------------------------------------------------------ build: fireflies and pollen

        void MeadowEffect::BuildCreatures(SceneContext const& ctx, wuc::ContainerVisual const& root, RECT const& rc, Ridge const& ground)
        {
            float W = static_cast<float>(rc.right - rc.left), H = static_cast<float>(rc.bottom - rc.top);
            float3 org(static_cast<float>(rc.left), static_cast<float>(rc.top), 0);

            auto group = [&](int level) {
                auto g = m_c.CreateContainerVisual();
                g.Size(float2(W, H));
                BindOpacity(g, [level](Look const& l) { return l.level[level]; });
                root.Children().InsertAtTop(g);
                return g;
            };

            auto fireflies = group(Fireflies);
            auto attract = m_c.CreateExpressionAnimation(kAttract);
            attract.SetReferenceParameter(L"pointer", ctx.pointer);
            attract.SetVector3Parameter(L"org", org);
            int count = static_cast<int>(W / H * 11.0f);
            for (int i = 0; i < count; ++i)
            {
                auto anchor = m_c.CreateContainerVisual();
                float d = m_rng.Next(12.0f, 24.0f) * H / 1440.0f;
                float x = m_rng.Next(0.0f, W);
                float y = m_rng.Next(ground.base - H * 0.07f, H * 0.96f);
                anchor.Offset(float3(x, y, 0));
                anchor.StartAnimation(L"Offset", Drift(m_c, m_rng, float3(x, y, 0), H * 0.05f, 5, m_rng.Next(26.0f, 44.0f)));
                auto fly = m_c.CreateSpriteVisual();
                fly.Size(float2(d, d));
                fly.Brush(GlowBrush(m_c, Argb(255, m_rng.Next(0.0f, 1.0f) < 0.7f ? 0xD8FF7A : 0xFFE27A), 0.22f, 0.4f));
                attract.SetReferenceParameter(L"anchor", anchor);
                attract.SetScalarParameter(L"half", d * 0.5f);
                attract.SetScalarParameter(L"pull", m_rng.Next(0.15f, 0.35f));
                fly.StartAnimation(L"Offset", attract);
                fly.StartAnimation(L"Opacity", Oscillate(m_c, 0.0f, 1.0f, m_rng.Next(1.8f, 4.2f), m_rng.Next(0.0f, 4.0f)));
                anchor.Children().InsertAtTop(fly);
                fireflies.Children().InsertAtTop(anchor);
            }

            auto pollen = group(Pollen);
            auto repel = m_c.CreateExpressionAnimation(kRepel);
            repel.SetReferenceParameter(L"pointer", ctx.pointer);
            repel.SetVector3Parameter(L"org", org);
            count = static_cast<int>(W / H * 16.0f);
            for (int i = 0; i < count; ++i)
            {
                auto anchor = m_c.CreateContainerVisual();
                float d = m_rng.Next(4.0f, 9.0f) * H / 1440.0f;
                float x = m_rng.Next(0.0f, W), y = m_rng.Next(H * 0.42f, H * 0.92f);
                anchor.Offset(float3(x, y, 0));
                anchor.StartAnimation(L"Offset", Drift(m_c, m_rng, float3(x, y, 0), H * 0.07f, 6, m_rng.Next(40.0f, 70.0f)));
                auto mote = m_c.CreateSpriteVisual();
                mote.Size(float2(d, d));
                mote.Brush(GlowBrush(m_c, Argb(230, 0xFFF6DC), 0.3f, 0.45f));
                repel.SetReferenceParameter(L"anchor", anchor);
                repel.SetScalarParameter(L"half", d * 0.5f);
                repel.SetScalarParameter(L"push", m_rng.Next(40.0f, 90.0f));
                mote.StartAnimation(L"Offset", repel);
                mote.StartAnimation(L"Opacity", Oscillate(m_c, 0.25f, 0.9f, m_rng.Next(4.0f, 9.0f), m_rng.Next(0.0f, 5.0f)));
                anchor.Children().InsertAtTop(mote);
                pollen.Children().InsertAtTop(anchor);
            }
        }

        // ------------------------------------------------------------ build

        void MeadowEffect::Build(SceneContext const& ctx, wuc::ContainerVisual const& layer)
        {
            m_c = ctx.compositor;
            m_graphics = ctx.graphics;
            m_rng = Rng(0x6D656164); // stable landscape across rebuilds
            m_colors.clear();
            m_opacities.clear();
            m_textures.clear();
            m_skies.clear();
            if (ctx.monitors.empty()) return;

            Sun sun = CurrentSun(ctx);
            m_look = LookFor(sun);
            m_wasDay = sun.day;
            m_sunProgress = sun.progress;
            m_phase = PhaseName(sun);

            m_scaleH = 0;
            for (auto const& rc : ctx.monitors) m_scaleH = std::max(m_scaleH, static_cast<float>(rc.bottom - rc.top));

            m_wind = m_c.CreatePropertySet();
            m_wind.InsertScalar(L"Phase", 0.0f);
            m_wind.InsertScalar(L"Strength", 1.0f);
            m_wind.InsertScalar(L"GustX", -100000.0f);
            m_wind.InsertScalar(L"GustT", 1.0f);
            auto phase = m_c.CreateScalarKeyFrameAnimation();
            auto linear = m_c.CreateLinearEasingFunction();
            phase.InsertKeyFrame(0.0f, 0.0f, linear);
            phase.InsertKeyFrame(1.0f, kTau, linear);
            phase.Duration(Seconds(150.0f));
            phase.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
            m_wind.StartAnimation(L"Phase", phase);
            m_wind.StartAnimation(L"Strength", Oscillate(m_c, 0.7f, 1.2f, 41.0f));

            // Grass and flower textures: a few variants per row, drawn in the current light.
            if (m_graphics)
            {
                for (int r = 0; r < 4; ++r)
                    for (int v = 0; v < kTuftVariants; ++v)
                    {
                        Texture t;
                        t.row = r;
                        t.seed = 1000u * r + 17u * v + 3u;
                        t.seedHead = r > 0 && v == kTuftVariants - 1;
                        t.size = float2(std::ceil(kRows[r].w * m_scaleH), std::ceil(kRows[r].h * m_scaleH * (t.seedHead ? 1.15f : 1.0f)));
                        t.surface = m_graphics->CreateSurface(t.size);
                        m_textures.push_back(t);
                    }
                for (int r = 2; r < 4; ++r)
                    for (int k = 0; k < FlowerKinds; ++k)
                        for (int v = 0; v < 2; ++v)
                        {
                            Texture t;
                            t.row = r;
                            t.flower = k;
                            t.seed = 5000u + 100u * r + 10u * k + v;
                            t.size = float2(std::ceil(kFlowerW[r - 2] * m_scaleH), std::ceil(kFlowerH[r - 2] * m_scaleH * (k == Lavender ? 0.9f : 1.0f)));
                            t.surface = m_graphics->CreateSurface(t.size);
                            m_textures.push_back(t);
                        }
            }

            for (auto const& rc : ctx.monitors)
            {
                auto root = m_c.CreateContainerVisual();
                root.Offset(float3(static_cast<float>(rc.left), static_cast<float>(rc.top), 0));
                root.Size(float2(static_cast<float>(rc.right - rc.left), static_cast<float>(rc.bottom - rc.top)));
                root.Clip(m_c.CreateInsetClip());
                layer.Children().InsertAtTop(root);
                BuildSky(ctx, root, rc);
                Ridge ground;
                BuildHills(ctx, root, rc, ground);
                if (!m_textures.empty()) BuildGrass(ctx, root, rc, ground);
                BuildCreatures(ctx, root, rc, ground);
            }

            m_fx = m_c.CreateContainerVisual();
            m_fx.Size(ctx.size);
            layer.Children().InsertAtTop(m_fx);
            float glowD = m_scaleH * 0.3f;
            auto glow = m_c.CreateSpriteVisual();
            glow.Size(float2(glowD, glowD));
            glow.Brush(Radial({ { 0.0f, Col(SunCore, 0.9f, CursorAlpha) }, { 0.3f, Col(SunGlow, 0.35f, CursorAlpha) }, { 1.0f, Col(SunGlow, 0.0f) } }));
            auto follow = m_c.CreateExpressionAnimation(L"pointer.Position - Vector3(half, half, 0.0)");
            follow.SetReferenceParameter(L"pointer", ctx.pointer);
            follow.SetScalarParameter(L"half", glowD * 0.5f);
            glow.StartAnimation(L"Offset", follow);
            auto strength = m_c.CreateExpressionAnimation(L"pointer.Strength");
            strength.SetReferenceParameter(L"pointer", ctx.pointer);
            glow.StartAnimation(L"Opacity", strength);
            m_fx.Children().InsertAtTop(glow);

            DrawTextures(m_look);
            PlaceLuminaries(sun, 0);
            Log(L"Meadow: built %zu textures, %zu bindings, phase %s", m_textures.size(), m_colors.size() + m_opacities.size(), m_phase.c_str());
        }

        // ------------------------------------------------------------ live updates

        void MeadowEffect::Tick(SceneContext const& ctx, wuc::ContainerVisual const&, bool jump)
        {
            if (!m_c || m_skies.empty()) return;
            Sun sun = CurrentSun(ctx);
            Look look = LookFor(sun);
            float seconds = jump ? 2.5f : 10.0f;
            bool flip = sun.day != m_wasDay;

            // Only touch the tree when the light actually changed (keeps DWM idle between ticks).
            if (LookDistance(look, m_look) >= 2.0f || LevelDistance(look, m_look) >= 0.01f)
                ApplyLook(look, seconds);
            if (LookDistance(look, m_drawn, Grass0Base, Grass3Tip) > 3.0f || Diff(look.color[SkyUpper], m_drawn.color[SkyUpper]) > 6.0f ||
                std::abs(look.level[FlowerLight] - m_drawn.level[FlowerLight]) > 0.03f)
                DrawTextures(look);

            float width = static_cast<float>(m_skies.front().rc.right - m_skies.front().rc.left);
            if (flip || std::abs(sun.progress - m_sunProgress) * width > 2.0f)
            {
                PlaceLuminaries(sun, flip ? 0.0f : seconds);
                m_sunProgress = sun.progress;
            }
            m_wasDay = sun.day;

            std::wstring phase = PhaseName(sun);
            if (phase != m_phase)
            {
                m_phase = phase;
                Log(L"Meadow: %s (elevation %.2f)", m_phase.c_str(), sun.elevation);
            }

            if (!sun.day && look.level[Stars] > 0.6f && (jump || m_rng.Next(0.0f, 1.0f) < 0.07f))
                SpawnShootingStar(m_skies[m_rng.Index(m_skies.size())]);
        }

        void MeadowEffect::SpawnShootingStar(Sky const& sky)
        {
            if (!sky.fx) return;
            float W = static_cast<float>(sky.rc.right - sky.rc.left), H = static_cast<float>(sky.rc.bottom - sky.rc.top);
            if (!m_starTrail)
            {
                auto trail = m_c.CreateLinearGradientBrush();
                trail.ColorStops().Append(m_c.CreateColorGradientStop(0.0f, Argb(0, 0xC8D8FF)));
                trail.ColorStops().Append(m_c.CreateColorGradientStop(0.85f, Argb(150, 0xE8EEFF)));
                trail.ColorStops().Append(m_c.CreateColorGradientStop(1.0f, Argb(255, 0xFFFFFF)));
                m_starTrail = trail;
            }
            float len = H * 0.14f;
            float angle = m_rng.Next(0.0f, 1.0f) < 0.5f ? m_rng.Next(18.0f, 34.0f) : m_rng.Next(146.0f, 162.0f);
            float rad = angle * kPi / 180.0f;
            float3 dir(std::cos(rad), std::sin(rad), 0);
            float3 head(m_rng.Next(0.15f, 0.85f) * W, m_rng.Next(0.05f, 0.28f) * H, 0);
            auto star = m_c.CreateSpriteVisual();
            star.Size(float2(len, 1.6f));
            star.CenterPoint(float3(len, 0.8f, 0));
            star.RotationAngleInDegrees(angle);
            star.Brush(m_starTrail);
            float3 start = head - float3(len, 0.8f, 0);
            star.Offset(start);
            star.Opacity(0);
            sky.fx.Children().InsertAtTop(star);

            auto batch = m_c.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
            auto move = m_c.CreateVector3KeyFrameAnimation();
            move.InsertKeyFrame(1.0f, start + dir * (H * 0.5f), m_c.CreateCubicBezierEasingFunction(float2(0.2f, 0.5f), float2(0.6f, 1.0f)));
            move.Duration(Seconds(1.0f));
            star.StartAnimation(L"Offset", move);
            auto fade = m_c.CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(0.15f, 1.0f);
            fade.InsertKeyFrame(0.6f, 0.8f);
            fade.InsertKeyFrame(1.0f, 0.0f);
            fade.Duration(Seconds(1.0f));
            star.StartAnimation(L"Opacity", fade);
            batch.End();
            auto fx = sky.fx;
            batch.Completed([fx, star](auto&&, auto&&) { fx.Children().Remove(star); });
        }

        void MeadowEffect::OnClick(SceneContext const& ctx, wuc::ContainerVisual const&, float2 pos)
        {
            if (!m_wind || !m_fx) return;
            // A gust ring travels outward through the grass.
            m_wind.InsertScalar(L"GustX", pos.x);
            auto gust = m_c.CreateScalarKeyFrameAnimation();
            gust.InsertKeyFrame(0.0f, 0.0f);
            gust.InsertKeyFrame(1.0f, 1.0f, m_c.CreateCubicBezierEasingFunction(float2(0.25f, 0.6f), float2(0.6f, 1.0f)));
            gust.Duration(Seconds(2.6f));
            m_wind.StartAnimation(L"GustT", gust);

            // Dandelion seeds lift off and drift away on the breeze.
            if (!m_seedBrush) m_seedBrush = GlowBrush(m_c, Argb(235, 0xFFFDF4), 0.28f, 0.5f);
            float H = m_scaleH > 0 ? m_scaleH : 1440.0f;
            auto batch = m_c.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
            std::vector<wuc::Visual> seeds;
            for (int i = 0; i < 9; ++i)
            {
                auto seed = m_c.CreateSpriteVisual();
                float d = m_rng.Next(7.0f, 12.0f) * H / 1440.0f;
                seed.Size(float2(d, d));
                seed.Brush(m_seedBrush);
                float3 p(pos.x + m_rng.Next(-24.0f, 24.0f) - d * 0.5f, pos.y + m_rng.Next(-12.0f, 12.0f) - d * 0.5f, 0);
                seed.Offset(p);
                seed.Opacity(0);
                float secs = m_rng.Next(7.0f, 11.0f);
                auto path = m_c.CreateVector3KeyFrameAnimation();
                float3 at = p;
                for (int k = 1; k <= 4; ++k)
                {
                    at += float3(H * m_rng.Next(0.03f, 0.09f), -H * m_rng.Next(0.03f, 0.075f), 0);
                    at.y += m_rng.Next(-1.0f, 1.0f) * H * 0.015f;
                    path.InsertKeyFrame(k / 4.0f, at);
                }
                path.Duration(Seconds(secs));
                seed.StartAnimation(L"Offset", path);
                auto fade = m_c.CreateScalarKeyFrameAnimation();
                fade.InsertKeyFrame(0.08f, 0.95f);
                fade.InsertKeyFrame(0.7f, 0.7f);
                fade.InsertKeyFrame(1.0f, 0.0f);
                fade.Duration(Seconds(secs));
                seed.StartAnimation(L"Opacity", fade);
                m_fx.Children().InsertAtTop(seed);
                seeds.push_back(seed);
            }
            batch.End();
            auto fx = m_fx;
            batch.Completed([fx, seeds](auto&&, auto&&) { for (auto const& s : seeds) fx.Children().Remove(s); });
            SpawnRipple(ctx, m_fx, pos, Argb(46, 0xFFFFFF), H * 0.2f, 0.0f);
        }
    }

    std::unique_ptr<Effect> CreateMeadowEffect() { return std::make_unique<MeadowEffect>(); }

    float MeadowPhaseHour(std::wstring const& rawName)
    {
        std::wstring name = ToLower(Trim(rawName));
        if (name.empty() || name == L"auto" || name == L"now") return -1.0f;
        SYSTEMTIME t{};
        GetLocalTime(&t);
        float len = DayLength(DayOfYear(t));
        float rise = kSolarNoon - len * 0.5f, set = kSolarNoon + len * 0.5f;
        auto wrap = [](float h) { return std::fmod(h + 24.0f, 24.0f); };
        if (name == L"dawn") return wrap(rise - 0.3f);
        if (name == L"sunrise") return rise + 0.15f;
        if (name == L"morning") return rise + 1.2f;
        if (name == L"day" || name == L"noon") return kSolarNoon;
        if (name == L"afternoon") return kSolarNoon + 2.5f;
        if (name == L"golden") return set - 0.6f;
        if (name == L"sunset") return set - 0.1f;
        if (name == L"dusk") return wrap(set + 0.35f);
        if (name == L"night") return wrap(set + 3.0f);
        if (name == L"midnight") return 0.0f;
        int h = 0, m = 0;
        if (swscanf_s(name.c_str(), L"%d:%d", &h, &m) >= 1 && h >= 0 && h < 24 && m >= 0 && m < 60) return h + m / 60.0f;
        return -2.0f;
    }
}
