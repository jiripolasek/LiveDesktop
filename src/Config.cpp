#include "pch.h"
#include "Config.h"
#include "Log.h"
#include "Util.h"
#include <fstream>
#include <sstream>

namespace wdj = winrt::Windows::Data::Json;

namespace ld
{
    namespace
    {
        constexpr char kDefaultConfig[] = R"({
  "_comment": "LiveDesktop config. Restart LiveDesktop after editing. Empty 'repos' = all of your open PRs.",
  "mock": false,
  "repos": [],
  "providers": {
    "sessions": { "enabled": true, "intervalSeconds": 4, "maxIdleHours": 8, "maxOrbs": 8 },
    "github":   { "enabled": true, "intervalSeconds": 120 },
    "system":   { "enabled": true, "intervalSeconds": 3 },
    "calendar": { "enabled": true, "intervalSeconds": 300, "icsUrl": "" },
    "ports":    { "enabled": true, "intervalSeconds": 10 },
    "logTail":  { "enabled": false, "intervalSeconds": 2, "path": "", "lines": 6 }
  },
  "scenes": {
    "default": "default",
    "focusMinutes": 25,
    "desktops": {
      "_example {GUID} or name or index": "work"
    }
  }
}
)";

        double Number(wdj::JsonObject const& o, wchar_t const* key, double fallback)
        {
            if (o && o.HasKey(key))
            {
                auto v = o.GetNamedValue(key);
                if (v.ValueType() == wdj::JsonValueType::Number) return v.GetNumber();
            }
            return fallback;
        }

        bool Bool(wdj::JsonObject const& o, wchar_t const* key, bool fallback)
        {
            if (o && o.HasKey(key))
            {
                auto v = o.GetNamedValue(key);
                if (v.ValueType() == wdj::JsonValueType::Boolean) return v.GetBoolean();
            }
            return fallback;
        }

        std::wstring String(wdj::JsonObject const& o, wchar_t const* key, std::wstring const& fallback)
        {
            if (o && o.HasKey(key))
            {
                auto v = o.GetNamedValue(key);
                if (v.ValueType() == wdj::JsonValueType::String) return std::wstring(v.GetString());
            }
            return fallback;
        }

        wdj::JsonObject Object(wdj::JsonObject const& o, wchar_t const* key)
        {
            if (o && o.HasKey(key))
            {
                auto v = o.GetNamedValue(key);
                if (v.ValueType() == wdj::JsonValueType::Object) return v.GetObject();
            }
            return nullptr;
        }

        void ReadProvider(wdj::JsonObject const& providers, wchar_t const* key, ProviderSettings& s)
        {
            auto o = Object(providers, key);
            s.enabled = Bool(o, L"enabled", s.enabled);
            s.intervalSeconds = std::clamp(static_cast<int>(Number(o, L"intervalSeconds", s.intervalSeconds)), 1, 3600);
        }
    }

    Config LoadConfig()
    {
        Config cfg;
        auto dir = std::filesystem::path(KnownFolder(FOLDERID_LocalAppData)) / L"LiveDesktop";
        cfg.path = (dir / L"config.json").wstring();

        std::error_code ec;
        if (!std::filesystem::exists(cfg.path, ec))
        {
            std::filesystem::create_directories(dir, ec);
            std::ofstream out(cfg.path, std::ios::binary);
            out << kDefaultConfig;
            Log(L"Config: wrote defaults to %s", cfg.path.c_str());
        }

        std::ifstream in(cfg.path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.erase(0, 3);

        wdj::JsonObject root{ nullptr };
        if (!wdj::JsonObject::TryParse(Utf8ToWide(text), root))
        {
            Log(L"Config: %s is not valid JSON - using defaults", cfg.path.c_str());
            return cfg;
        }

        cfg.mock = Bool(root, L"mock", false);
        if (root.HasKey(L"repos") && root.GetNamedValue(L"repos").ValueType() == wdj::JsonValueType::Array)
        {
            for (auto const& v : root.GetNamedArray(L"repos"))
            {
                if (v.ValueType() == wdj::JsonValueType::String && !v.GetString().empty()) cfg.repos.emplace_back(v.GetString());
            }
        }

        auto providers = Object(root, L"providers");
        ReadProvider(providers, L"sessions", cfg.sessions);
        ReadProvider(providers, L"github", cfg.github);
        ReadProvider(providers, L"system", cfg.system);
        ReadProvider(providers, L"calendar", cfg.calendar);
        ReadProvider(providers, L"ports", cfg.ports);
        ReadProvider(providers, L"logTail", cfg.logTail);
        auto sessions = Object(providers, L"sessions");
        cfg.sessionMaxIdleHours = std::clamp(Number(sessions, L"maxIdleHours", cfg.sessionMaxIdleHours), 0.1, 24.0 * 30);
        cfg.maxOrbs = std::clamp(static_cast<int>(Number(sessions, L"maxOrbs", cfg.maxOrbs)), 1, 16);
        cfg.icsUrl = String(Object(providers, L"calendar"), L"icsUrl", L"");
        auto logTail = Object(providers, L"logTail");
        cfg.logPath = String(logTail, L"path", L"");
        cfg.logLines = std::clamp(static_cast<int>(Number(logTail, L"lines", cfg.logLines)), 1, 20);

        auto scenes = Object(root, L"scenes");
        cfg.defaultScene = ToLower(String(scenes, L"default", cfg.defaultScene));
        cfg.focusMinutes = std::clamp(static_cast<int>(Number(scenes, L"focusMinutes", cfg.focusMinutes)), 1, 240);
        if (auto desktops = Object(scenes, L"desktops"))
        {
            for (auto const& kv : desktops)
            {
                std::wstring key(kv.Key());
                if (key.empty() || key[0] == L'_' || kv.Value().ValueType() != wdj::JsonValueType::String) continue;
                cfg.desktopScenes.emplace_back(ToLower(key), ToLower(std::wstring(kv.Value().GetString())));
            }
        }

        Log(L"Config: %s (repos=%zu, mock=%d, default scene '%s', %zu desktop mappings)", cfg.path.c_str(),
            cfg.repos.size(), cfg.mock ? 1 : 0, cfg.defaultScene.c_str(), cfg.desktopScenes.size());
        return cfg;
    }
}
