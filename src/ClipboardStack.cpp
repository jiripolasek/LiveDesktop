#include "pch.h"
#include "ClipboardStack.h"
#include "Log.h"

namespace ld
{
    namespace
    {
        constexpr size_t kMaxCards = 8;
        constexpr int kPeekCards = 3;

        wuc::CompositionEasingFunction Decelerate(wuc::Compositor const& c)
        {
            return c.CreateCubicBezierEasingFunction(float2(0.1f, 0.9f), float2(0.2f, 1.0f));
        }

        wuc::CompositionEasingFunction Accelerate(wuc::Compositor const& c)
        {
            return c.CreateCubicBezierEasingFunction(float2(0.7f, 0.0f), float2(1.0f, 0.5f));
        }

        void AnimateScalar(wuc::Compositor const& c, wuc::CompositionObject const& target, wchar_t const* property,
            std::optional<float> from, float to, float seconds, float delay, wuc::CompositionEasingFunction const& ease)
        {
            auto a = c.CreateScalarKeyFrameAnimation();
            if (from) a.InsertKeyFrame(0.0f, *from);
            else a.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
            a.InsertKeyFrame(1.0f, to, ease);
            a.Duration(Seconds(seconds));
            a.DelayTime(Seconds(delay));
            a.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            target.StartAnimation(property, a);
        }

        void AnimateVector3(wuc::Compositor const& c, wuc::CompositionObject const& target, wchar_t const* property,
            std::optional<float3> from, float3 to, float seconds, float delay, wuc::CompositionEasingFunction const& ease)
        {
            auto a = c.CreateVector3KeyFrameAnimation();
            if (from) a.InsertKeyFrame(0.0f, *from);
            else a.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
            a.InsertKeyFrame(1.0f, to, ease);
            a.Duration(Seconds(seconds));
            a.DelayTime(Seconds(delay));
            a.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            target.StartAnimation(property, a);
        }

        bool Intersects(RECT const& a, RECT const& b)
        {
            return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
        }

        std::wstring FileName(std::wstring const& path)
        {
            auto pos = path.find_last_of(L"\\/");
            return pos == std::wstring::npos ? path : path.substr(pos + 1);
        }

        std::wstring FormatBytes(uint64_t bytes)
        {
            wchar_t buf[32];
            if (bytes >= 1024ull * 1024 * 1024) swprintf_s(buf, L"%.1f GB", bytes / (1024.0 * 1024 * 1024));
            else if (bytes >= 1024ull * 1024) swprintf_s(buf, L"%.1f MB", bytes / (1024.0 * 1024));
            else if (bytes >= 1024) swprintf_s(buf, L"%.0f KB", bytes / 1024.0);
            else swprintf_s(buf, L"%llu B", bytes);
            return buf;
        }

        std::wstring FirstLine(std::wstring const& text)
        {
            size_t start = 0;
            while (start < text.size())
            {
                size_t end = text.find(L'\n', start);
                std::wstring line = Trim(text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
                if (!line.empty()) return Truncate(line, 160);
                if (end == std::wstring::npos) break;
                start = end + 1;
            }
            return {};
        }

        struct Look
        {
            std::wstring glyph;     // Segoe Fluent Icons codepoint
            std::wstring glyphText; // drawn with a text font instead of an icon ("Aa")
            uint32_t tint = 0xC8D1E0;
            std::wstring primary, secondary;
            bool mono = false;
        };

        Look LookOf(ClipItem const& it)
        {
            Look l;
            auto age = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - it.at);
            std::wstring when = age < std::chrono::seconds(60) ? L"just now" : FormatAge(age) + L" ago";
            std::wstring tail = L" \u00B7 " + when + (it.source.empty() ? L"" : L" \u00B7 " + it.source);
            switch (it.kind)
            {
            case ClipKind::Code:
            {
                l.glyph = L"\uE943";
                l.tint = 0x60CDFF;
                l.primary = FirstLine(it.text);
                l.mono = true;
                size_t lines = 1 + std::count(it.text.begin(), it.text.end(), L'\n');
                l.secondary = (lines > 1 ? std::to_wstring(lines) + L" lines of code" : L"Code") + tail;
                break;
            }
            case ClipKind::Command:
                l.glyph = L"\uE756";
                l.tint = 0x6CE58A;
                l.primary = Truncate(Trim(it.text), 160);
                l.mono = true;
                l.secondary = L"Command" + tail;
                break;
            case ClipKind::Url:
            {
                l.glyph = L"\uE71B";
                l.tint = 0xB48CFF;
                std::wstring u = Trim(it.text);
                auto scheme = u.find(L"://");
                if (scheme != std::wstring::npos) u = u.substr(scheme + 3);
                if (u.rfind(L"www.", 0) == 0) u = u.substr(4);
                std::wstring host = u.substr(0, u.find(L'/'));
                l.primary = Truncate(u, 160);
                l.secondary = L"Link \u00B7 " + host + L" \u00B7 " + when;
                break;
            }
            case ClipKind::Color:
            {
                wchar_t hex[16], rgb[48];
                swprintf_s(hex, L"#%06X", it.color & 0xFFFFFF);
                swprintf_s(rgb, L"rgb(%u, %u, %u)", (it.color >> 16) & 0xFF, (it.color >> 8) & 0xFF, it.color & 0xFF);
                l.primary = hex;
                l.mono = true;
                l.tint = it.color;
                l.secondary = std::wstring(rgb) + tail;
                break;
            }
            case ClipKind::Image:
            {
                l.glyph = L"\uEB9F";
                l.tint = 0x5FD4C4;
                l.primary = it.source == L"Snipping Tool" ? L"Screenshot" : L"Image";
                wchar_t size[48];
                swprintf_s(size, L"%d \u00D7 %d", it.imageW, it.imageH);
                l.secondary = std::wstring(size) + tail;
                break;
            }
            case ClipKind::Files:
            {
                l.glyph = it.files.size() > 1 ? L"\uE8B7" : L"\uE7C3";
                l.tint = 0xFFB86B;
                l.primary = it.files.empty() ? L"Files" : FileName(it.files.front());
                if (it.files.size() > 1) l.primary += L"  +" + std::to_wstring(it.files.size() - 1);
                std::wstring what = it.files.size() > 1 ? std::to_wstring(it.files.size()) + L" files" : L"File";
                l.secondary = what + (it.bytes ? L" \u00B7 " + FormatBytes(it.bytes) : L"") + tail;
                break;
            }
            case ClipKind::Secret:
            {
                l.glyph = L"\uE72E";
                l.tint = 0xFF8A9A;
                std::wstring t = Trim(it.text);
                auto cut = t.find_first_of(L"_-");
                std::wstring prefix = cut != std::wstring::npos && cut < 12 ? t.substr(0, cut + 1) : t.substr(0, 3);
                l.primary = prefix + L"\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022";
                l.secondary = L"Secret, hidden \u00B7 click to copy" + std::wstring(L" \u00B7 ") + when;
                break;
            }
            default:
            {
                l.glyphText = L"Aa";
                l.tint = 0xC8D1E0;
                l.primary = FirstLine(it.text);
                size_t words = 0;
                bool in = false;
                for (wchar_t ch : it.text)
                {
                    bool space = iswspace(ch) != 0;
                    if (!space && !in) ++words;
                    in = !space;
                }
                l.secondary = (words == 1 ? std::wstring(L"1 word") : std::to_wstring(words) + L" words") + tail;
                break;
            }
            }
            return l;
        }
    }

    void ClipboardStack::Initialize(Scene& scene, Graphics& graphics)
    {
        m_scene = &scene;
        m_graphics = &graphics;
        m_c = scene.Compositor();
        m_root = m_c.CreateContainerVisual();
        scene.Overlay().Children().InsertAtTop(m_root);

        m_caption = m_c.CreateSpriteVisual();
        m_root.Children().InsertAtTop(m_caption);
        m_drop = m_c.CreateSpriteVisual();
        m_drop.Opacity(0.0f);
        m_root.Children().InsertAtTop(m_drop);
        m_cardsRoot = m_c.CreateContainerVisual();
        m_root.Children().InsertAtTop(m_cardsRoot);

        m_hoverGeometry = m_c.CreateRoundedRectangleGeometry();
        auto shape = m_c.CreateSpriteShape(m_hoverGeometry);
        shape.FillBrush(m_c.CreateColorBrush(Argb(16, 0xFFFFFF)));
        shape.StrokeBrush(m_c.CreateColorBrush(Argb(70, 0xFFFFFF)));
        shape.StrokeThickness(1.0f);
        m_hover = m_c.CreateShapeVisual();
        m_hover.Shapes().Append(shape);
        m_hover.Opacity(0.0f);
        m_root.Children().InsertAtTop(m_hover);

        m_toast = m_c.CreateSpriteVisual();
        m_toast.Opacity(0.0f);
        m_root.Children().InsertAtTop(m_toast);
    }

    void ClipboardStack::Shutdown()
    {
        m_cards.clear();
        *this = ClipboardStack{};
    }

    void ClipboardStack::SetLayout(SceneLayout const& layout)
    {
        bool rescale = layout.scale != m_layout.scale;
        m_layout = layout;
        if (!m_root) return;
        m_w = D(304);
        m_h = D(58);
        Place();
        if (rescale) RedrawAll();
        Arrange(false);
        DrawCaption();
        DrawDropZone();
    }

    void ClipboardStack::SetPreset(ScenePreset const& preset)
    {
        m_preset = preset;
        if (!m_root) return;
        // The focus scene stays calm: the shelf is still there, just quieter.
        AnimateScalar(m_c, m_root, L"Opacity", std::nullopt, !m_enabled ? 0.0f : preset.focus ? 0.55f : 1.0f, 0.4f, 0.0f, Decelerate(m_c));
    }

    void ClipboardStack::SetEnabled(bool enabled)
    {
        m_enabled = enabled;
        if (!m_root) return;
        if (!enabled) { SetFanned(false); ShowHover(std::nullopt); }
        SetPreset(m_preset);
        Log(L"Clipboard shelf %s", enabled ? L"shown" : L"hidden");
    }

    // Bottom-right of the primary work area, nudged left past any icon columns.
    void ClipboardStack::Place()
    {
        RECT work = m_layout.primaryWork;
        if (work.right <= work.left || work.bottom <= work.top)
            work = RECT{ 0, 0, static_cast<LONG>(m_layout.size.x), static_cast<LONG>(m_layout.size.y) };
        float margin = D(28);
        float x = work.right - margin - m_w;
        float y = work.bottom - margin - m_h;
        float pad = D(10);
        for (int iter = 0; iter < 16; ++iter)
        {
            RECT fp{ static_cast<LONG>(x - pad), static_cast<LONG>(y - D(48) - pad), static_cast<LONG>(x + m_w + pad), static_cast<LONG>(y + m_h + pad) };
            bool moved = false;
            for (auto const& icon : m_layout.icons)
            {
                if (!Intersects(icon, fp)) continue;
                x = icon.left - pad - m_w;
                moved = true;
                break;
            }
            if (!moved) break;
        }
        x = std::max(x, static_cast<float>(work.left) + margin);
        m_base = float2(std::round(x), std::round(y));
    }

    RECT ClipboardStack::Footprint() const
    {
        return RECT{ static_cast<LONG>(m_base.x), static_cast<LONG>(m_base.y - 2 * D(7) - D(28)),
                     static_cast<LONG>(m_base.x + m_w), static_cast<LONG>(m_base.y + m_h) };
    }

    RECT ClipboardStack::DropZone() const
    {
        RECT r = Footprint();
        InflateRect(&r, static_cast<int>(D(16)), static_cast<int>(D(12)));
        return r;
    }

    size_t ClipboardStack::VisibleCount() const
    {
        size_t n = 0;
        for (auto const& c : m_cards) if (!c.removing) ++n;
        return std::min(n, kMaxCards);
    }

    D2D1_RECT_F ClipboardStack::CardRect(size_t index, bool fanned) const
    {
        if (fanned)
        {
            float top = m_base.y - static_cast<float>(index) * (m_h + D(8));
            return D2D1::RectF(m_base.x, top, m_base.x + m_w, top + m_h);
        }
        float k = static_cast<float>(std::min<size_t>(index, kPeekCards - 1));
        float s = 1.0f - 0.05f * k;
        float top = m_base.y - k * D(7);
        float inset = m_w * (1.0f - s) * 0.5f;
        return D2D1::RectF(m_base.x + inset, top, m_base.x + m_w - inset, top + m_h * s);
    }

    bool ClipboardStack::Contains(float2 p) const
    {
        if (!Shown()) return false;
        RECT r = Footprint();
        if (m_fanned)
        {
            size_t n = std::max<size_t>(VisibleCount(), 1);
            auto top = CardRect(n - 1, true);
            r.top = std::min(r.top, static_cast<LONG>(top.top));
            InflateRect(&r, static_cast<int>(D(14)), static_cast<int>(D(14)));
        }
        return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
    }

    std::optional<uint64_t> ClipboardStack::HitTest(float2 p) const
    {
        if (!Shown() || !m_fanned || m_dropHover) return std::nullopt;
        size_t i = 0;
        for (auto const& card : m_cards)
        {
            if (card.removing) continue;
            if (i >= kMaxCards) break;
            auto r = CardRect(i++, true);
            if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) return card.id;
        }
        return std::nullopt;
    }

    // ------------------------------------------------------------------ snapshot

    void ClipboardStack::Update(ClipboardSnapshot const& snapshot)
    {
        if (!m_root) return;
        m_mock = snapshot.mock;
        std::vector<Card> next;
        for (auto const& item : snapshot.items)
        {
            if (next.size() >= kMaxCards) break;
            auto it = std::find_if(m_cards.begin(), m_cards.end(), [&](Card const& c) { return c.id == item.id && !c.removing; });
            if (it != m_cards.end())
            {
                Card card = std::move(*it);
                m_cards.erase(it);
                card.item = item;
                DrawCard(card, false);
                next.push_back(std::move(card));
                continue;
            }
            Card card;
            card.id = item.id;
            card.item = item;
            card.visual = m_c.CreateSpriteVisual();
            auto shadow = m_c.CreateDropShadow();
            shadow.BlurRadius(D(22));
            shadow.Offset(float3(0, D(8), 0));
            shadow.Color(Argb(255, 0x000000));
            shadow.Opacity(0.38f);
            shadow.SourcePolicy(wuc::CompositionDropShadowSourcePolicy::InheritFromVisualContent);
            card.visual.Shadow(shadow);
            card.visual.Opacity(0.0f);
            card.visual.Offset(float3(m_base.x, m_base.y + D(18), 0));
            card.visual.Scale(float3(0.97f, 0.97f, 1));
            m_cardsRoot.Children().InsertAtTop(card.visual);
            DrawCard(card, true);
            next.push_back(std::move(card));
        }
        // Cards that fell off the end of the history fade out.
        for (auto& stale : m_cards)
        {
            if (stale.removing) continue;
            auto batch = m_c.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
            AnimateScalar(m_c, stale.visual, L"Opacity", std::nullopt, 0.0f, 0.25f, 0.0f, Accelerate(m_c));
            batch.End();
            auto root = m_cardsRoot;
            auto visual = stale.visual;
            batch.Completed([root, visual](auto&&, auto&&) { root.Children().Remove(visual); visual.Close(); });
        }
        m_cards = std::move(next);
        Reorder();
        Arrange(true);
        DrawCaption();
        DrawDropZone();
        if (m_hovered && std::none_of(m_cards.begin(), m_cards.end(), [&](Card const& c) { return c.id == *m_hovered; }))
            ShowHover(std::nullopt);
    }

    void ClipboardStack::Reorder()
    {
        // Newest card on top of the z-order.
        auto children = m_cardsRoot.Children();
        for (auto& card : m_cards) children.Remove(card.visual);
        for (auto& card : m_cards) children.InsertAtBottom(card.visual);
    }

    void ClipboardStack::Arrange(bool animate)
    {
        if (!m_root || m_w <= 0) return;
        auto ease = m_fanned ? Decelerate(m_c) : Decelerate(m_c);
        float peekOpacity[kPeekCards] = { 1.0f, 0.72f, 0.42f };
        size_t i = 0;
        for (auto& card : m_cards)
        {
            float3 offset;
            float scale = 1.0f, opacity = 0.0f;
            if (i >= kMaxCards) { ++i; continue; }
            if (m_fanned)
            {
                offset = float3(m_base.x, m_base.y - static_cast<float>(i) * (m_h + D(8)), 0);
                opacity = 1.0f;
            }
            else
            {
                size_t k = std::min<size_t>(i, kPeekCards - 1);
                offset = float3(m_base.x, m_base.y - static_cast<float>(k) * D(7), 0);
                scale = 1.0f - 0.05f * static_cast<float>(k);
                opacity = i < kPeekCards ? peekOpacity[i] : 0.0f;
            }
            card.visual.CenterPoint(float3(m_w * 0.5f, 0, 0));
            if (animate)
            {
                float delay = m_fanned ? 0.022f * static_cast<float>(i) : 0.0f;
                float seconds = m_fanned ? 0.34f : 0.24f;
                AnimateVector3(m_c, card.visual, L"Offset", std::nullopt, offset, seconds, delay, ease);
                AnimateVector3(m_c, card.visual, L"Scale", std::nullopt, float3(scale, scale, 1), seconds, delay, ease);
                AnimateScalar(m_c, card.visual, L"Opacity", std::nullopt, opacity, seconds * 0.8f, delay, ease);
            }
            else
            {
                card.visual.Offset(offset);
                card.visual.Scale(float3(scale, scale, 1));
                card.visual.Opacity(opacity);
            }
            ++i;
        }
        float captionOpacity = m_fanned || m_dropHover ? 0.0f : 1.0f;
        if (animate) AnimateScalar(m_c, m_caption, L"Opacity", std::nullopt, captionOpacity, 0.2f, 0.0f, ease);
        else m_caption.Opacity(captionOpacity);
        m_toast.Offset(float3(m_base.x - m_toast.Size().x - D(12), m_base.y + std::round((m_h - m_toast.Size().y) * 0.5f), 0));
    }

    void ClipboardStack::SetFanned(bool fanned)
    {
        if (fanned == m_fanned) return;
        m_fanned = fanned;
        if (!fanned) ShowHover(std::nullopt);
        Arrange(true);
    }

    // ------------------------------------------------------------------ drawing

    bool ClipboardStack::DrawCard(Card& card, bool force)
    {
        if (!m_graphics || m_w <= 0) return false;
        Look look = LookOf(card.item);
        std::wstring key = look.primary + L"|" + look.secondary + L"|" + std::to_wstring(m_w) + L"|" + std::to_wstring(m_layout.scale);
        if (!force && card.surface && key == card.key) return false;
        card.key = key;
        auto& g = *m_graphics;
        float W = m_w, H = m_h, P = D(11);
        float2 size(W, H);
        if (!card.surface)
        {
            card.surface = g.CreateSurface(size);
            card.visual.Brush(m_c.CreateSurfaceBrush(card.surface));
        }
        card.visual.Size(size);
        auto const& item = card.item;

        g.Draw(card.surface, size, [&](ID2D1DeviceContext* dc)
        {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(D2DColor(0x1A2030, 0.80f), brush.put());
            auto shape = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), D(10), D(10));
            dc->FillRoundedRectangle(shape, brush.get());
            if (auto noise = g.NoiseBrush(dc)) dc->FillRoundedRectangle(shape, noise);
            D2D1_GRADIENT_STOP stops[2] = { { 0.0f, D2DColor(0xFFFFFF, 0.08f) }, { 1.0f, D2DColor(0xFFFFFF, 0.0f) } };
            winrt::com_ptr<ID2D1GradientStopCollection> collection;
            winrt::com_ptr<ID2D1LinearGradientBrush> sheen;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, collection.put())) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, H)), collection.get(), sheen.put())))
            {
                dc->FillRoundedRectangle(shape, sheen.get());
            }
            brush->SetColor(D2DColor(0xFFFFFF, 0.13f));
            dc->DrawRoundedRectangle(shape, brush.get(), 1.0f);

            // Leading tile: thumbnail, color swatch or kind glyph.
            float T = D(36);
            float tileW = item.kind == ClipKind::Image ? D(56) : T;
            float tx = P, ty = std::round((H - T) * 0.5f);
            auto tile = D2D1::RoundedRect(D2D1::RectF(tx, ty, tx + tileW, ty + T), D(8), D(8));
            if (item.kind == ClipKind::Image && item.thumb && item.thumbW > 0)
            {
                winrt::com_ptr<ID2D1Bitmap> bitmap;
                winrt::com_ptr<ID2D1BitmapBrush> bb;
                auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(item.thumbW, item.thumbH), item.thumb->data(), item.thumbW * 4, props, bitmap.put())) &&
                    SUCCEEDED(dc->CreateBitmapBrush(bitmap.get(), bb.put())))
                {
                    float s = std::max(tileW / item.thumbW, T / item.thumbH);
                    bb->SetTransform(D2D1::Matrix3x2F::Scale(s, s) *
                        D2D1::Matrix3x2F::Translation(tx + (tileW - item.thumbW * s) * 0.5f, ty + (T - item.thumbH * s) * 0.5f));
                    dc->FillRoundedRectangle(tile, bb.get());
                }
                brush->SetColor(D2DColor(0xFFFFFF, 0.18f));
                dc->DrawRoundedRectangle(tile, brush.get(), 1.0f);
            }
            else if (item.kind == ClipKind::Color)
            {
                brush->SetColor(D2DColor(item.color, 1.0f));
                dc->FillRoundedRectangle(tile, brush.get());
                brush->SetColor(D2DColor(0xFFFFFF, 0.28f));
                dc->DrawRoundedRectangle(tile, brush.get(), 1.0f);
            }
            else
            {
                brush->SetColor(D2DColor(look.tint, 0.15f));
                dc->FillRoundedRectangle(tile, brush.get());
                bool text = !look.glyphText.empty();
                auto gl = g.Layout(text ? look.glyphText : look.glyph,
                    g.Font(text ? FontRole::UiSemibold : FontRole::Icons, text ? 13.0f : 15.0f, DWRITE_TEXT_ALIGNMENT_CENTER), tileW, 10000, false);
                float2 gs = g.Measure(gl.get());
                brush->SetColor(D2DColor(look.tint, 0.95f));
                dc->DrawTextLayout(D2D1::Point2F(tx, std::round(ty + (T - gs.y) * 0.5f)), gl.get(), brush.get());
            }

            float x = tx + tileW + D(12);
            float textW = W - P - x;
            auto primary = g.Layout(look.primary, g.Font(look.mono ? FontRole::Mono : FontRole::Ui, look.mono ? 12.0f : 13.0f), textW);
            auto secondary = g.Layout(look.secondary, g.Font(FontRole::Ui, 11.5f), textW);
            float2 ps = g.Measure(primary.get()), ss = g.Measure(secondary.get());
            float top = std::round((H - (ps.y + ss.y + D(2))) * 0.5f);
            brush->SetColor(D2DColor(0xFFFFFF, 0.93f));
            dc->DrawTextLayout(D2D1::Point2F(x, top), primary.get(), brush.get());
            brush->SetColor(D2DColor(0xFFFFFF, 0.52f));
            dc->DrawTextLayout(D2D1::Point2F(x, top + ps.y + D(2)), secondary.get(), brush.get());
        });
        return true;
    }

    void ClipboardStack::DrawCaption()
    {
        if (!m_graphics || m_w <= 0) return;
        size_t n = VisibleCount();
        std::wstring text = L"CLIPBOARD";
        if (n) text += L"  \u00B7  " + std::to_wstring(n);
        std::wstring key = text + std::to_wstring(m_layout.scale);
        auto& g = *m_graphics;
        auto layout = g.Layout(text, g.Font(FontRole::UiSemibold, 10.5f), D(240));
        float2 ts = g.Measure(layout.get());
        float2 size(std::ceil(ts.x + D(4)), std::ceil(ts.y + D(2)));
        if (key != m_captionKey || !m_captionSurface)
        {
            m_captionKey = key;
            if (!m_captionSurface)
            {
                m_captionSurface = g.CreateSurface(size);
                m_caption.Brush(m_c.CreateSurfaceBrush(m_captionSurface));
            }
            g.Draw(m_captionSurface, size, [&](ID2D1DeviceContext* dc)
            {
                winrt::com_ptr<ID2D1SolidColorBrush> brush;
                dc->CreateSolidColorBrush(D2DColor(0xFFFFFF, 0.50f), brush.put());
                dc->DrawTextLayout(D2D1::Point2F(D(2), 0), layout.get(), brush.get());
            });
            m_caption.Size(size);
        }
        m_caption.Offset(float3(m_base.x + D(6), m_base.y - 2 * D(7) - size.y - D(6), 0));
    }

    void ClipboardStack::DrawToast(std::wstring const& text)
    {
        if (!m_graphics) return;
        auto& g = *m_graphics;
        auto glyph = g.Layout(L"\uE73E", g.Font(FontRole::Icons, 12), D(20), 10000, false);
        auto label = g.Layout(text, g.Font(FontRole::Ui, 12.5f), D(320));
        float2 gs = g.Measure(glyph.get()), ls = g.Measure(label.get());
        float2 size(std::ceil(D(12) + gs.x + D(8) + ls.x + D(14)), D(32));
        if (!m_toastSurface)
        {
            m_toastSurface = g.CreateSurface(size);
            m_toast.Brush(m_c.CreateSurfaceBrush(m_toastSurface));
            auto shadow = m_c.CreateDropShadow();
            shadow.BlurRadius(D(18));
            shadow.Offset(float3(0, D(6), 0));
            shadow.Opacity(0.4f);
            shadow.SourcePolicy(wuc::CompositionDropShadowSourcePolicy::InheritFromVisualContent);
            m_toast.Shadow(shadow);
        }
        g.Draw(m_toastSurface, size, [&](ID2D1DeviceContext* dc)
        {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(D2DColor(0x1A2030, 0.90f), brush.put());
            auto pill = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, size.x - 0.5f, size.y - 0.5f), size.y * 0.5f, size.y * 0.5f);
            dc->FillRoundedRectangle(pill, brush.get());
            brush->SetColor(D2DColor(palette::Accent, 0.10f));
            dc->FillRoundedRectangle(pill, brush.get());
            brush->SetColor(D2DColor(palette::Accent, 0.45f));
            dc->DrawRoundedRectangle(pill, brush.get(), 1.0f);
            brush->SetColor(D2DColor(palette::Accent, 1.0f));
            dc->DrawTextLayout(D2D1::Point2F(D(12), std::round((size.y - gs.y) * 0.5f)), glyph.get(), brush.get());
            brush->SetColor(D2DColor(0xFFFFFF, 0.92f));
            dc->DrawTextLayout(D2D1::Point2F(D(12) + gs.x + D(8), std::round((size.y - ls.y) * 0.5f)), label.get(), brush.get());
        });
        m_toast.Size(size);
        float3 to(m_base.x - size.x - D(12), m_base.y + std::round((m_h - size.y) * 0.5f), 0);
        auto ease = Decelerate(m_c);
        AnimateVector3(m_c, m_toast, L"Offset", float3(to.x + D(10), to.y, 0), to, 0.3f, 0.0f, ease);
        auto a = m_c.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(0.0f, 0.0f);
        a.InsertKeyFrame(0.08f, 1.0f, ease);
        a.InsertKeyFrame(0.75f, 1.0f);
        a.InsertKeyFrame(1.0f, 0.0f, Accelerate(m_c));
        a.Duration(Seconds(2.0f));
        m_toast.StartAnimation(L"Opacity", a);
    }

    void ClipboardStack::DrawDropZone()
    {
        if (!m_graphics || m_w <= 0) return;
        auto& g = *m_graphics;
        float W = m_w, H = m_h + 2 * D(7);
        float2 size(W, H);
        bool empty = VisibleCount() == 0;
        if (!m_dropSurface)
        {
            m_dropSurface = g.CreateSurface(size);
            m_drop.Brush(m_c.CreateSurfaceBrush(m_dropSurface));
        }
        g.Draw(m_dropSurface, size, [&](ID2D1DeviceContext* dc)
        {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(D2DColor(0x1A2030, 0.86f), brush.put());
            auto shape = D2D1::RoundedRect(D2D1::RectF(1.0f, 1.0f, W - 1.0f, H - 1.0f), D(12), D(12));
            dc->FillRoundedRectangle(shape, brush.get());
            brush->SetColor(D2DColor(palette::Accent, 0.13f));
            dc->FillRoundedRectangle(shape, brush.get());
            winrt::com_ptr<ID2D1Factory> factory;
            dc->GetFactory(factory.put());
            winrt::com_ptr<ID2D1StrokeStyle> dashed;
            float dashes[] = { 4.0f, 3.0f };
            factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f), dashes, 2, dashed.put());
            brush->SetColor(D2DColor(palette::Accent, 0.85f));
            dc->DrawRoundedRectangle(shape, brush.get(), 1.5f, dashed.get());

            auto glyph = g.Layout(L"\uE896", g.Font(FontRole::Icons, 18), D(28), 10000, false);
            float2 gs = g.Measure(glyph.get());
            auto title = g.Layout(L"Drop to keep on your clipboard", g.Font(FontRole::UiSemibold, 13), W - D(80));
            auto sub = g.Layout(empty ? L"Files or text \u00B7 copies show up here too" : L"Files or text \u00B7 paste them anywhere",
                g.Font(FontRole::Ui, 11.5f), W - D(80));
            float2 ts = g.Measure(title.get()), ss = g.Measure(sub.get());
            float top = std::round((H - (ts.y + ss.y + D(2))) * 0.5f);
            brush->SetColor(D2DColor(palette::Accent, 1.0f));
            dc->DrawTextLayout(D2D1::Point2F(D(18), std::round((H - gs.y) * 0.5f)), glyph.get(), brush.get());
            brush->SetColor(D2DColor(0xFFFFFF, 0.94f));
            dc->DrawTextLayout(D2D1::Point2F(D(56), top), title.get(), brush.get());
            brush->SetColor(D2DColor(0xFFFFFF, 0.55f));
            dc->DrawTextLayout(D2D1::Point2F(D(56), top + ts.y + D(2)), sub.get(), brush.get());
        });
        m_drop.Size(size);
        m_drop.CenterPoint(float3(W * 0.5f, H * 0.5f, 0));
        m_drop.Offset(float3(m_base.x, m_base.y - 2 * D(7), 0));
        float opacity = m_dropHover ? 1.0f : empty ? 0.5f : 0.0f;
        AnimateScalar(m_c, m_drop, L"Opacity", std::nullopt, opacity, 0.2f, 0.0f, Decelerate(m_c));
    }

    void ClipboardStack::RedrawAll()
    {
        for (auto& card : m_cards) if (!card.removing) DrawCard(card, true);
        m_captionKey.clear();
        DrawCaption();
        DrawDropZone();
    }

    void ClipboardStack::Tick()
    {
        // Ages are minute-granular, so this redraws at most once a minute per card.
        for (auto& card : m_cards) if (!card.removing) DrawCard(card, false);
    }

    // ------------------------------------------------------------------ input

    void ClipboardStack::ForceFan(std::optional<bool> fan)
    {
        m_forced = fan;
        if (fan) SetFanned(*fan);
        else SetFanned(false);
    }

    void ClipboardStack::Hover(std::optional<float2> pos)
    {
        if (!m_root || !Shown()) return;
        if (m_dropHover) return;
        bool inside = pos && Contains(*pos);
        if (!m_forced) SetFanned(inside && VisibleCount() > 0);
        ShowHover(m_fanned && pos ? HitTest(*pos) : std::nullopt);
    }

    void ClipboardStack::ShowHover(std::optional<uint64_t> id)
    {
        if (id == m_hovered) return;
        bool wasVisible = m_hovered.has_value();
        m_hovered = id;
        auto ease = Decelerate(m_c);
        if (!id)
        {
            AnimateScalar(m_c, m_hover, L"Opacity", std::nullopt, 0.0f, 0.15f, 0.0f, ease);
            return;
        }
        size_t i = 0;
        for (auto const& card : m_cards)
        {
            if (card.removing) continue;
            if (card.id == *id) break;
            ++i;
        }
        auto r = CardRect(i, true);
        float3 offset(r.left, r.top, 0);
        float2 size(r.right - r.left, r.bottom - r.top);
        m_hoverGeometry.CornerRadius(float2(D(10), D(10)));
        m_hoverGeometry.Size(size);
        m_hover.Size(float2(size.x + 2, size.y + 2));
        if (wasVisible) AnimateVector3(m_c, m_hover, L"Offset", std::nullopt, offset, 0.14f, 0.0f, ease);
        else m_hover.Offset(offset);
        AnimateScalar(m_c, m_hover, L"Opacity", std::nullopt, 1.0f, 0.12f, 0.0f, ease);
    }

    void ClipboardStack::Activate(uint64_t id)
    {
        auto it = std::find_if(m_cards.begin(), m_cards.end(), [&](Card const& c) { return c.id == id && !c.removing; });
        if (it == m_cards.end()) return;
        auto& card = *it;
        if (!card.flash)
        {
            auto geometry = m_c.CreateRoundedRectangleGeometry();
            geometry.CornerRadius(float2(D(10), D(10)));
            geometry.Size(float2(m_w, m_h));
            auto shape = m_c.CreateSpriteShape(geometry);
            shape.FillBrush(m_c.CreateColorBrush(Argb(46, palette::Accent)));
            shape.StrokeBrush(m_c.CreateColorBrush(Argb(230, palette::Accent)));
            shape.StrokeThickness(1.5f);
            card.flash = m_c.CreateShapeVisual();
            card.flash.Shapes().Append(shape);
            card.flash.Size(float2(m_w, m_h));
            card.flash.Opacity(0.0f);
            card.visual.Children().InsertAtTop(card.flash);
        }
        auto a = m_c.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(0.0f, 0.0f);
        a.InsertKeyFrame(0.15f, 1.0f);
        a.InsertKeyFrame(1.0f, 0.0f);
        a.Duration(Seconds(1.0f));
        card.flash.StartAnimation(L"Opacity", a);
        DrawToast(card.item.kind == ClipKind::Secret ? L"Copied \u00B7 hidden" : L"Copied to clipboard");
        Log(L"Clipboard shelf: activate item %llu", id);
    }

    void ClipboardStack::SetDropHover(bool active)
    {
        if (!m_root || active == m_dropHover) return;
        m_dropHover = active;
        if (active)
        {
            if (!m_forced) SetFanned(false);
            ShowHover(std::nullopt);
        }
        Arrange(true);
        DrawDropZone();
        auto ease = Decelerate(m_c);
        AnimateVector3(m_c, m_drop, L"Scale", std::nullopt, active ? float3(1.04f, 1.04f, 1) : float3(1, 1, 1), 0.25f, 0.0f, ease);
    }

    void ClipboardStack::Dropped(std::wstring const& what)
    {
        SetDropHover(false);
        DrawToast(L"Kept " + what + L" \u00B7 on your clipboard");
    }
}
