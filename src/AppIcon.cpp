#include "pch.h"

namespace ld
{
    // Draws a small glowing orb icon at runtime so no binary .ico needs to live in the repo.
    HICON CreateAppIcon(int size)
    {
        BITMAPV5HEADER bi{};
        bi.bV5Size = sizeof(bi);
        bi.bV5Width = size;
        bi.bV5Height = -size;
        bi.bV5Planes = 1;
        bi.bV5BitCount = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask = 0x00FF0000;
        bi.bV5GreenMask = 0x0000FF00;
        bi.bV5BlueMask = 0x000000FF;
        bi.bV5AlphaMask = 0xFF000000;

        void* bits = nullptr;
        HDC dc = GetDC(nullptr);
        HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, dc);
        if (!color) return LoadIconW(nullptr, IDI_APPLICATION);

        auto* px = static_cast<uint32_t*>(bits);
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                float u = (x + 0.5f) / size * 2.0f - 1.0f;
                float v = (y + 0.5f) / size * 2.0f - 1.0f;
                float r = std::sqrt(u * u + v * v);
                float a = std::clamp((1.0f - r) * 6.0f, 0.0f, 1.0f);
                float t = std::clamp((u - v) * 0.5f + 0.5f, 0.0f, 1.0f);
                float hl = std::clamp(1.0f - std::sqrt((u + 0.35f) * (u + 0.35f) + (v + 0.35f) * (v + 0.35f)) * 1.6f, 0.0f, 1.0f);
                float R = (0.13f + 0.42f * t) + hl * 0.6f;
                float G = (0.83f - 0.47f * t) + hl * 0.6f;
                float B = (0.93f + 0.03f * t) + hl * 0.6f;
                auto c8 = [&](float c) { return static_cast<uint32_t>(std::clamp(c, 0.0f, 1.0f) * a * 255.0f + 0.5f); };
                px[y * size + x] = (static_cast<uint32_t>(a * 255.0f + 0.5f) << 24) | (c8(R) << 16) | (c8(G) << 8) | c8(B);
            }
        }

        HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
        ICONINFO ii{ TRUE, 0, 0, mask, color };
        HICON icon = CreateIconIndirect(&ii);
        DeleteObject(mask);
        DeleteObject(color);
        return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
    }
}
