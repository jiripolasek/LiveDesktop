#pragma once
#include "pch.h"
#include "Scene.h"
#include "Model.h"

namespace ld
{
    // Geometry of the desktop in host-window pixels (the host covers the whole virtual screen).
    struct SceneLayout
    {
        float2 size{};
        RECT primary{};      // primary monitor
        RECT primaryWork{};  // primary monitor work area (excludes taskbar)
        float scale = 1.0f;  // primary monitor DPI / 96
        std::vector<RECT> monitors;
        std::vector<RECT> icons; // desktop icon cells
    };

    // What a scene shows. Selected per virtual desktop (config "desktopScenes") or from the tray.
    struct ScenePreset
    {
        std::wstring name = L"default";
        bool dashboard = true;   // Win+D reveal shows cards
        bool devCards = false;   // Dev ports + log tail cards
        bool focus = false;      // calm veil + focus timer
        bool allOrbs = true;     // false: only sessions waiting for input
        bool fireflies = true;
        bool tide = true;
        bool dust = true;
    };

    inline ScenePreset PresetFor(std::wstring const& name)
    {
        ScenePreset p;
        p.name = name;
        if (name == L"work")
        {
            p.devCards = true;
        }
        else if (name == L"focus")
        {
            p.dashboard = false;
            p.focus = true;
            p.allOrbs = false;
            p.fireflies = false;
            p.tide = false;
            p.dust = false;
        }
        else
        {
            p.name = L"default";
        }
        return p;
    }

    inline std::vector<std::wstring> SceneNames() { return { L"default", L"work", L"focus" }; }

    inline float RectW(RECT const& r) { return static_cast<float>(r.right - r.left); }
    inline float RectH(RECT const& r) { return static_cast<float>(r.bottom - r.top); }

    namespace palette
    {
        constexpr uint32_t Working = 0x4CC2FF;
        constexpr uint32_t Waiting = 0xFFB33C;
        constexpr uint32_t Done = 0x6CE58A;
        constexpr uint32_t DoneDim = 0x86B79C;
        constexpr uint32_t Idle = 0x8A94A6;
        constexpr uint32_t Failure = 0xFF5468;
        constexpr uint32_t Pending = 0xFFB33C;
        constexpr uint32_t Success = 0x6CE58A;
        constexpr uint32_t Firefly = 0xE8FF8A;
        constexpr uint32_t Meeting = 0xB48CFF;
        constexpr uint32_t MeetingSoon = 0xFFB86B;
        constexpr uint32_t MeetingNow = 0x5FD4C4;
        constexpr uint32_t Accent = 0x60CDFF;
    }

    inline uint32_t StatusRgb(AgentStatus status)
    {
        switch (status)
        {
        case AgentStatus::Working: return palette::Working;
        case AgentStatus::Waiting: return palette::Waiting;
        case AgentStatus::Done: return palette::Done;
        default: return palette::Idle;
        }
    }
}
