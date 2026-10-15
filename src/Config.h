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

        ProviderSettings sessions{ true, 4 };

        double sessionMaxIdleHours = 8.0;
        int maxOrbs = 8;
        bool mock = false;
    };

    // Loads %LOCALAPPDATA%\LiveDesktop\config.json, writing defaults first if it does not exist.
    Config LoadConfig();
}
