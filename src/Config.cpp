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
  "_comment": "LiveDesktop config. Restart LiveDesktop after editing.",
  "mock": false,
  "providers": {
    "sessions": { "enabled": true, "intervalSeconds": 4, "maxIdleHours": 8, "maxOrbs": 8 }
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

        auto providers = Object(root, L"providers");
        ReadProvider(providers, L"sessions", cfg.sessions);
        auto sessions = Object(providers, L"sessions");
        cfg.sessionMaxIdleHours = std::clamp(Number(sessions, L"maxIdleHours", cfg.sessionMaxIdleHours), 0.1, 24.0 * 30);
        cfg.maxOrbs = std::clamp(static_cast<int>(Number(sessions, L"maxOrbs", cfg.maxOrbs)), 1, 16);

        Log(L"Config: %s (mock=%d, maxOrbs=%d)", cfg.path.c_str(), cfg.mock ? 1 : 0, cfg.maxOrbs);
        return cfg;
    }
}
