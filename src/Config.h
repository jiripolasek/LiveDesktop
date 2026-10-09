#pragma once
#include "pch.h"

namespace ld
{
    struct ProviderSettings
    {
        bool enabled = true;
        int intervalSeconds = 60;
    };

    struct Config
    {
        std::wstring path;
        std::vector<std::wstring> repos; // empty = all of my open PRs (GraphQL search)

        ProviderSettings sessions{ true, 4 };
        ProviderSettings github{ true, 120 };
        ProviderSettings system{ true, 3 };
        ProviderSettings calendar{ true, 300 };
        ProviderSettings ports{ true, 10 };
        ProviderSettings logTail{ false, 2 };

        double sessionMaxIdleHours = 8.0;
        int maxOrbs = 8;
        std::wstring icsUrl;
        std::wstring logPath;
        int logLines = 6;

        std::wstring defaultScene = L"default";
        // Lower-cased desktop GUID ("{...}"), name, or 1-based index -> scene name
        std::vector<std::pair<std::wstring, std::wstring>> desktopScenes;
        int focusMinutes = 25;
        bool mock = false;
    };

    // Loads %LOCALAPPDATA%\LiveDesktop\config.json, writing defaults first if it does not exist.
    Config LoadConfig();
}
