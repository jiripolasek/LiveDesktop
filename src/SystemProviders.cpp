#include "pch.h"
#include "Providers.h"
#include "Log.h"
#include "Util.h"
#include <iphlpapi.h>
#include <pdh.h>
#include <fstream>

using namespace std::chrono;

namespace ld
{
    // ---------------------------------------------------------------- System pulse
    namespace
    {
        uint64_t ToU64(FILETIME const& ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }
    }

    SystemProvider::SystemProvider(Config const& config) : SnapshotProvider(L"system", config.system) {}

    SystemProvider::~SystemProvider()
    {
        Stop();
        if (m_query) PdhCloseQuery(m_query);
    }

    bool SystemProvider::PollReal()
    {
        SystemSnapshot snap;
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user))
        {
            uint64_t i = ToU64(idle), k = ToU64(kernel), u = ToU64(user);
            uint64_t total = (k - m_kernel) + (u - m_user); // kernel time includes idle time
            if (m_kernel && total > 0) m_cpu = std::clamp(1.0f - static_cast<float>(i - m_idle) / static_cast<float>(total), 0.0f, 1.0f);
            m_idle = i; m_kernel = k; m_user = u;
        }
        snap.cpu = m_cpu;

        MEMORYSTATUSEX mem{ sizeof(mem) };
        if (GlobalMemoryStatusEx(&mem)) snap.memory = mem.dwMemoryLoad / 100.0f;

        // GPU 3D engine utilisation summed over processes. Expanding the wildcard instance list is the
        // most expensive part, so only sample every third poll.
        if (!m_gpuTried)
        {
            m_gpuTried = true;
            PDH_HQUERY query = nullptr;
            PDH_HCOUNTER counter = nullptr;
            if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS &&
                PdhAddEnglishCounterW(query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &counter) == ERROR_SUCCESS)
            {
                PdhCollectQueryData(query);
                m_query = query;
                m_counter = counter;
            }
            else
            {
                if (query) PdhCloseQuery(query);
                Log(L"[system] GPU performance counters unavailable - GPU ember disabled");
            }
        }
        if (m_query && (m_gpuTick++ % 3) == 0 && PdhCollectQueryData(m_query) == ERROR_SUCCESS)
        {
            DWORD bytes = 0, count = 0;
            PdhGetFormattedCounterArrayW(m_counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr);
            if (bytes)
            {
                std::vector<uint8_t> buffer(bytes);
                auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
                if (PdhGetFormattedCounterArrayW(m_counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) == ERROR_SUCCESS)
                {
                    double sum = 0;
                    for (DWORD n = 0; n < count; ++n)
                    {
                        if (items[n].FmtValue.CStatus == ERROR_SUCCESS) sum += items[n].FmtValue.doubleValue;
                    }
                    m_gpu = static_cast<float>(std::clamp(sum / 100.0, 0.0, 1.0));
                }
            }
        }
        snap.gpu = m_gpu;
        snap.gpuAvailable = m_query != nullptr;
        Publish(snap);
        return true;
    }

    void SystemProvider::PollMock()
    {
        float t = duration<float>(steady_clock::now() - m_mockStart).count();
        SystemSnapshot snap;
        snap.mock = true;
        snap.cpu = std::clamp(0.18f + 0.14f * std::sin(t * 0.21f) + 0.08f * std::sin(t * 0.83f), 0.02f, 1.0f);
        snap.memory = 0.58f + 0.06f * std::sin(t * 0.05f);
        snap.gpu = std::clamp(0.35f + 0.3f * std::sin(t * 0.13f + 1.0f), 0.0f, 1.0f);
        snap.gpuAvailable = true;
        Publish(snap);
    }

    // ---------------------------------------------------------------- Dev ports
    namespace
    {
        bool IsDevProcess(std::wstring const& name)
        {
            static const std::set<std::wstring> dev = {
                L"node", L"dotnet", L"python", L"pythonw", L"py", L"java", L"javaw", L"ruby", L"go", L"deno", L"bun", L"php", L"php-cgi",
                L"nginx", L"httpd", L"caddy", L"postgres", L"mysqld", L"mariadbd", L"redis-server", L"mongod", L"docker", L"com.docker.backend",
                L"wslrelay", L"vite", L"esbuild", L"uvicorn", L"gunicorn", L"iisexpress", L"func", L"hugo", L"cargo", L"rustc", L"azurite",
                L"cosmosdb.emulator", L"sqlservr", L"kestrel", L"live-server", L"http-server", L"jekyll", L"flask", L"rails", L"next-server",
            };
            return dev.contains(name);
        }

        bool IsCommonDevPort(uint16_t port)
        {
            static const std::set<uint16_t> ports = { 3000, 3001, 3002, 4000, 4200, 4321, 5000, 5001, 5173, 5174, 5500, 6006, 8000, 8001,
                8080, 8081, 8443, 8888, 9000, 9229, 1433, 3306, 5432, 6379, 27017, 7071, 10000, 10001, 10002 };
            return ports.contains(port);
        }

        bool IsSystemProcess(std::wstring const& name)
        {
            static const std::set<std::wstring> sys = { L"system", L"svchost", L"lsass", L"wininit", L"services", L"spoolsv", L"smss", L"csrss",
                L"winlogon", L"explorer", L"searchindexer", L"msmpeng", L"" };
            return sys.contains(name);
        }
    }

    PortsProvider::PortsProvider(Config const& config) : SnapshotProvider(L"ports", config.ports) {}

    bool PortsProvider::PollReal()
    {
        struct Entry { uint16_t port; DWORD pid; std::wstring address; };
        std::vector<Entry> entries;

        auto collect = [&](ULONG family) -> bool
        {
            DWORD size = 0;
            GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
            std::vector<uint8_t> buffer(size + 4096);
            size = static_cast<DWORD>(buffer.size());
            if (GetExtendedTcpTable(buffer.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return false;
            if (family == AF_INET)
            {
                auto table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
                for (DWORD i = 0; i < table->dwNumEntries; ++i)
                {
                    auto const& row = table->table[i];
                    auto addr = row.dwLocalAddr;
                    std::wstring a = addr == 0 ? L"*" : (addr == htonl(INADDR_LOOPBACK) ? L"localhost" : L"lan");
                    entries.push_back({ ntohs(static_cast<u_short>(row.dwLocalPort)), row.dwOwningPid, a });
                }
            }
            else
            {
                auto table = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
                for (DWORD i = 0; i < table->dwNumEntries; ++i)
                {
                    auto const& row = table->table[i];
                    static const uint8_t loopback[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 };
                    static const uint8_t any[16] = {};
                    std::wstring a = memcmp(row.ucLocalAddr, any, 16) == 0 ? L"*" : memcmp(row.ucLocalAddr, loopback, 16) == 0 ? L"localhost" : L"lan";
                    entries.push_back({ ntohs(static_cast<u_short>(row.dwLocalPort)), row.dwOwningPid, a });
                }
            }
            return true;
        };
        bool ok4 = collect(AF_INET);
        bool ok6 = collect(AF_INET6);
        if (!ok4 && !ok6) return false;

        std::unordered_map<DWORD, std::wstring> names;
        auto nameOf = [&](DWORD pid) -> std::wstring
        {
            if (auto it = names.find(pid); it != names.end()) return it->second;
            std::wstring name;
            if (auto it = m_names.find(pid); it != m_names.end()) name = it->second;
            else if (pid > 4)
            {
                if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
                {
                    wchar_t path[MAX_PATH];
                    DWORD len = MAX_PATH;
                    if (QueryFullProcessImageNameW(h, 0, path, &len)) name = ToLower(std::filesystem::path(path).stem().wstring());
                    CloseHandle(h);
                }
            }
            names[pid] = name;
            return name;
        };

        PortsSnapshot snap;
        std::set<uint16_t> seen;
        for (auto const& e : entries)
        {
            auto name = nameOf(e.pid);
            if (IsSystemProcess(name)) continue;
            if (!IsDevProcess(name) && !IsCommonDevPort(e.port)) continue;
            if (!seen.insert(e.port).second) continue;
            snap.ports.push_back({ e.port, name, e.pid, e.address });
        }
        m_names = std::move(names); // keeps the cache bounded to live pids
        std::sort(snap.ports.begin(), snap.ports.end(), [](auto const& a, auto const& b) { return a.port < b.port; });
        Publish(std::move(snap));
        return true;
    }

    void PortsProvider::PollMock()
    {
        PortsSnapshot snap;
        snap.mock = true;
        snap.ports = {
            { 3000, L"node", 21408, L"localhost" },
            { 5000, L"dotnet", 18892, L"localhost" },
            { 5173, L"node", 22016, L"localhost" },
            { 5432, L"postgres", 4120, L"*" },
            { 8080, L"python", 25540, L"*" },
        };
        Publish(std::move(snap));
    }

    // ---------------------------------------------------------------- Log tail
    namespace
    {
        bool LooksLikeError(std::wstring const& line)
        {
            auto lower = ToLower(line);
            return lower.find(L"error") != std::wstring::npos || lower.find(L"fail") != std::wstring::npos ||
                lower.find(L"exception") != std::wstring::npos || lower.find(L"fatal") != std::wstring::npos;
        }
    }

    LogTailProvider::LogTailProvider(Config const& config)
        : SnapshotProvider(L"logTail", config.logTail), m_path(config.logPath), m_lines(config.logLines) {}

    void LogTailProvider::OnModeChanged()
    {
        m_size = 0;
        m_mockLines.clear();
    }

    bool LogTailProvider::PollReal()
    {
        if (m_path.empty()) return true;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(m_path.c_str(), GetFileExInfoStandard, &data)) return false;
        uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        if (size == m_size) return true;
        uint64_t previous = m_size;
        m_size = size;

        HANDLE h = CreateFileW(m_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        uint64_t start = size > 64 * 1024 ? size - 64 * 1024 : 0;
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(start);
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
        std::string text(static_cast<size_t>(size - start), '\0');
        DWORD read = 0;
        ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
        CloseHandle(h);
        text.resize(read);

        std::vector<std::pair<uint64_t, std::wstring>> lines; // (end offset, text)
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            auto line = Trim(Utf8ToWide(std::string_view(text).substr(pos, end - pos)));
            if (!line.empty()) lines.emplace_back(start + end, Truncate(line, 140));
            pos = end + 1;
        }
        LogTailSnapshot snap;
        snap.path = m_path;
        size_t first = lines.size() > static_cast<size_t>(m_lines) ? lines.size() - m_lines : 0;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            bool error = LooksLikeError(lines[i].second);
            if (error && lines[i].first > previous && previous != 0) ++m_errorSerial;
            if (i >= first) snap.lines.push_back({ lines[i].second, error });
        }
        snap.errorSerial = m_errorSerial;
        Publish(std::move(snap));
        return true;
    }

    void LogTailProvider::PollMock()
    {
        static const wchar_t* normal[] = {
            L"[vite] page reload src/settings/Theme.tsx",
            L"GET /api/cart 200 in 38ms",
            L"info: Microsoft.Hosting.Lifetime[0] Application started",
            L"[worker] processed batch 4182 (512 msgs) in 210ms",
            L"GET /api/products?page=2 200 in 61ms",
            L"[hmr] updated /src/components/CardList.tsx",
            L"POST /api/checkout 201 in 144ms",
        };
        static const wchar_t* errors[] = {
            L"ERROR CheckoutService: payment provider timeout after 5000ms",
            L"Test failed: checkout > applies discount code (expected 90, got 100)",
        };
        int tick = m_mockTick++;
        bool error = Mocks().logError.exchange(false) || (tick % 9 == 8);
        if (m_mockLines.empty())
        {
            for (int i = 0; i < m_lines; ++i) m_mockLines.push_back({ normal[i % 7], false });
        }
        LogLine line{ error ? errors[(tick / 9) % 2] : normal[(tick * 3) % 7], error };
        if (error) ++m_errorSerial;
        m_mockLines.push_back(line);
        while (m_mockLines.size() > static_cast<size_t>(m_lines)) m_mockLines.erase(m_mockLines.begin());
        LogTailSnapshot snap;
        snap.mock = true;
        snap.path = L"mock: npm run dev";
        snap.lines = m_mockLines;
        snap.errorSerial = m_errorSerial;
        Publish(std::move(snap));
    }

    // ---------------------------------------------------------------- Virtual desktops
    namespace
    {
        constexpr wchar_t kVdKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";

        std::wstring GuidString(GUID const& g)
        {
            wchar_t buf[64];
            StringFromGUID2(g, buf, 64);
            return ToLower(buf);
        }

        bool ReadBinary(HKEY key, wchar_t const* value, std::vector<uint8_t>& out)
        {
            DWORD type = 0, size = 0;
            if (RegQueryValueExW(key, value, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_BINARY) return false;
            out.resize(size);
            return RegQueryValueExW(key, value, nullptr, nullptr, out.data(), &size) == ERROR_SUCCESS;
        }
    }

    VirtualDesktopInfo VirtualDesktopWatcher::ReadCurrent()
    {
        VirtualDesktopInfo info;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kVdKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return info;
        std::vector<uint8_t> current, ids;
        if (!ReadBinary(key, L"CurrentVirtualDesktop", current) || current.size() < sizeof(GUID))
        {
            // Older builds keep the current desktop under SessionInfo\<session id>.
            DWORD sessionId = 0;
            ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
            auto path = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SessionInfo\\") + std::to_wstring(sessionId) + L"\\VirtualDesktops";
            HKEY sk = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &sk) == ERROR_SUCCESS)
            {
                ReadBinary(sk, L"CurrentVirtualDesktop", current);
                RegCloseKey(sk);
            }
        }
        ReadBinary(key, L"VirtualDesktopIDs", ids);
        if (current.size() >= sizeof(GUID))
        {
            GUID g{};
            memcpy(&g, current.data(), sizeof(GUID));
            info.id = GuidString(g);
            for (size_t i = 0; i + sizeof(GUID) <= ids.size(); i += sizeof(GUID))
            {
                if (memcmp(ids.data() + i, &g, sizeof(GUID)) == 0) info.index = static_cast<int>(i / sizeof(GUID)) + 1;
            }
            wchar_t name[256]{};
            DWORD size = sizeof(name);
            auto sub = std::wstring(L"Desktops\\") + info.id;
            if (RegGetValueW(key, sub.c_str(), L"Name", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS && name[0]) info.name = name;
        }
        RegCloseKey(key);
        if (info.index == 0) info.index = 1;
        if (info.name.empty()) info.name = L"Desktop " + std::to_wstring(info.index);
        return info;
    }

    VirtualDesktopWatcher::~VirtualDesktopWatcher() { Stop(); }

    void VirtualDesktopWatcher::Start(winrt::Windows::System::DispatcherQueue const& queue)
    {
        if (m_thread.joinable()) return;
        m_queue = queue;
        m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_thread = std::thread([this] { Run(); });
    }

    void VirtualDesktopWatcher::Stop()
    {
        if (m_stopEvent) SetEvent(m_stopEvent);
        if (m_thread.joinable()) m_thread.join();
        if (m_stopEvent) CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }

    void VirtualDesktopWatcher::Run()
    {
        SetThreadDescription(GetCurrentThread(), L"virtual-desktop-watcher");
        HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HKEY key = nullptr;
        RegOpenKeyExW(HKEY_CURRENT_USER, kVdKey, 0, KEY_NOTIFY | KEY_READ, &key);
        std::wstring lastId = L"?";
        std::wstring lastName;
        for (;;)
        {
            auto info = ReadCurrent();
            if (info.id != lastId || info.name != lastName)
            {
                lastId = info.id;
                lastName = info.name;
                Log(L"Virtual desktop: %s (%s, #%d)", info.name.c_str(), info.id.empty() ? L"unknown id" : info.id.c_str(), info.index);
                m_queue.TryEnqueue([this, info]() { if (OnChanged) OnChanged(info); });
            }
            DWORD timeout = 5000;
            if (key && RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, changed, TRUE) == ERROR_SUCCESS)
                timeout = 60'000; // notifications work; the timeout is just a safety net
            HANDLE handles[] = { m_stopEvent, changed };
            if (WaitForMultipleObjects(2, handles, FALSE, timeout) == WAIT_OBJECT_0) break;
        }
        if (key) RegCloseKey(key);
        CloseHandle(changed);
    }
}
