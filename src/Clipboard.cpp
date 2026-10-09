#include "pch.h"
#include "Clipboard.h"
#include "Log.h"
#include <regex>

using namespace std::chrono_literals;

namespace ld
{
    namespace
    {
        constexpr wchar_t kWatcherClass[] = L"LiveDesktop.ClipboardWatcher";
        constexpr UINT WM_WATCHER_REQUEST = WM_APP + 1;
        constexpr UINT WM_WATCHER_QUIT = WM_APP + 2;
        constexpr UINT_PTR TIMER_READ = 1;
        constexpr size_t kMaxTextChars = 2'000'000;
        constexpr size_t kMaxDibBytes = 96u * 1024 * 1024;
        constexpr int kThumbMax = 160;

        uint64_t Fnv(void const* data, size_t size, uint64_t h = 1469598103934665603ull)
        {
            auto const* p = static_cast<uint8_t const*>(data);
            for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
            return h;
        }

        std::wstring FriendlyApp(std::wstring stem)
        {
            static const std::pair<wchar_t const*, wchar_t const*> names[] = {
                { L"code", L"VS Code" }, { L"code - insiders", L"VS Code" }, { L"msedge", L"Edge" }, { L"chrome", L"Chrome" },
                { L"firefox", L"Firefox" }, { L"windowsterminal", L"Terminal" }, { L"openconsole", L"Terminal" },
                { L"explorer", L"Explorer" }, { L"ms-teams", L"Teams" }, { L"teams", L"Teams" }, { L"devenv", L"Visual Studio" },
                { L"snippingtool", L"Snipping Tool" }, { L"screenclippinghost", L"Snipping Tool" }, { L"notepad", L"Notepad" },
                { L"olk", L"Outlook" }, { L"outlook", L"Outlook" }, { L"winword", L"Word" }, { L"excel", L"Excel" },
                { L"powerpnt", L"PowerPoint" }, { L"pwsh", L"PowerShell" }, { L"powershell", L"PowerShell" },
                { L"figma", L"Figma" }, { L"slack", L"Slack" }, { L"livedesktop", L"LiveDesktop" },
            };
            std::wstring lower = ToLower(stem);
            for (auto const& [key, value] : names) if (lower == key) return value;
            return stem;
        }

        std::wstring OwnerApp(HWND owner)
        {
            if (!owner) return {};
            DWORD pid = 0;
            GetWindowThreadProcessId(owner, &pid);
            if (!pid) return {};
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h) return {};
            wchar_t path[MAX_PATH * 2]{};
            DWORD len = ARRAYSIZE(path);
            std::wstring name;
            if (QueryFullProcessImageNameW(h, 0, path, &len)) name = FriendlyApp(std::filesystem::path(path).stem().wstring());
            CloseHandle(h);
            return name;
        }

        bool OpenClipboardRetry(HWND hwnd)
        {
            for (int i = 0; i < 6; ++i)
            {
                if (OpenClipboard(hwnd)) return true;
                Sleep(25 + i * 20);
            }
            return false;
        }

        bool PutGlobal(UINT format, void const* data, size_t size)
        {
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, size);
            if (!g) return false;
            if (void* p = GlobalLock(g)) { memcpy(p, data, size); GlobalUnlock(g); }
            if (!SetClipboardData(format, g)) { GlobalFree(g); return false; }
            return true;
        }

        // Scales any packed DIB down to a small 32bpp thumbnail with GDI (handles every bit depth/compression GDI knows).
        bool MakeThumbnail(std::vector<uint8_t> const& dib, int& w, int& h, std::vector<uint32_t>& thumb, int& tw, int& th)
        {
            if (dib.size() < sizeof(BITMAPINFOHEADER)) return false;
            auto const* bih = reinterpret_cast<BITMAPINFOHEADER const*>(dib.data());
            w = bih->biWidth;
            h = std::abs(bih->biHeight);
            if (w <= 0 || h <= 0 || bih->biSize < sizeof(BITMAPINFOHEADER)) return false;
            size_t colors = bih->biClrUsed ? bih->biClrUsed : (bih->biBitCount <= 8 ? (size_t{ 1 } << bih->biBitCount) : 0);
            size_t offset = bih->biSize + colors * sizeof(RGBQUAD);
            if (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER)) offset += 3 * sizeof(DWORD);
            if (offset >= dib.size()) return false;

            float s = std::min(1.0f, static_cast<float>(kThumbMax) / static_cast<float>(std::max(w, h)));
            tw = std::max(1, static_cast<int>(std::lround(w * s)));
            th = std::max(1, static_cast<int>(std::lround(h * s)));

            HDC dc = CreateCompatibleDC(nullptr);
            if (!dc) return false;
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = tw;
            bi.bmiHeader.biHeight = -th;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            bool ok = false;
            if (bmp && bits)
            {
                HGDIOBJ old = SelectObject(dc, bmp);
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                ok = StretchDIBits(dc, 0, 0, tw, th, 0, 0, w, h, dib.data() + offset,
                    reinterpret_cast<BITMAPINFO const*>(dib.data()), DIB_RGB_COLORS, SRCCOPY) != 0;
                GdiFlush();
                if (ok)
                {
                    auto const* px = static_cast<uint32_t const*>(bits);
                    thumb.resize(static_cast<size_t>(tw) * th);
                    for (size_t i = 0; i < thumb.size(); ++i) thumb[i] = 0xFF000000u | (px[i] & 0x00FFFFFFu);
                }
                SelectObject(dc, old);
            }
            if (bmp) DeleteObject(bmp);
            DeleteDC(dc);
            return ok;
        }

        bool StartsWith(std::wstring const& s, std::wstring_view prefix)
        {
            return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin());
        }

        // Procedural "screenshot" for the mock image item.
        void MockImage(ClipItem& item)
        {
            int tw = 160, th = 90;
            auto thumb = std::make_shared<std::vector<uint32_t>>(static_cast<size_t>(tw) * th);
            for (int y = 0; y < th; ++y)
            {
                for (int x = 0; x < tw; ++x)
                {
                    float fx = x / float(tw), fy = y / float(th);
                    float hill = 0.62f + 0.08f * std::sin(fx * 5.0f + 0.6f) + 0.04f * std::sin(fx * 13.0f);
                    float r, g, b;
                    if (fy > hill) { r = 0.10f + 0.1f * fx; g = 0.28f + 0.1f * (1 - fy); b = 0.20f; }
                    else
                    {
                        float t = fy / hill;
                        r = 0.98f - 0.55f * t; g = 0.62f - 0.18f * t + 0.1f * fx; b = 0.42f + 0.38f * t;
                        float d = std::hypot(fx - 0.72f, fy - 0.48f);
                        float sun = std::clamp(1.0f - d * 9.0f, 0.0f, 1.0f);
                        r += sun * 0.4f; g += sun * 0.35f; b += sun * 0.2f;
                    }
                    auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
                    (*thumb)[static_cast<size_t>(y) * tw + x] = 0xFF000000u | (c(r) << 16) | (c(g) << 8) | c(b);
                }
            }
            // A matching small CF_DIB so "copy again" works in mock mode too.
            auto dib = std::make_shared<std::vector<uint8_t>>(sizeof(BITMAPINFOHEADER) + thumb->size() * 4);
            auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(dib->data());
            *bih = BITMAPINFOHEADER{ sizeof(BITMAPINFOHEADER), tw, -th, 1, 32, BI_RGB };
            memcpy(dib->data() + sizeof(BITMAPINFOHEADER), thumb->data(), thumb->size() * 4);
            item.kind = ClipKind::Image;
            item.imageW = 1920;
            item.imageH = 1080;
            item.thumbW = tw;
            item.thumbH = th;
            item.thumb = thumb;
            item.dib = dib;
            item.hash = Fnv(dib->data(), dib->size());
        }
    }

    ClipKind ClipboardWatcher::Classify(std::wstring const& raw, uint32_t& color)
    {
        std::wstring t = Trim(raw);
        bool multi = t.find(L'\n') != std::wstring::npos;
        bool spaces = t.find_first_of(L" \t\r\n") != std::wstring::npos;

        if (!spaces && t.size() >= 20)
        {
            static const wchar_t* prefixes[] = { L"ghp_", L"gho_", L"ghu_", L"ghs_", L"ghr_", L"github_pat_", L"sk-", L"xoxb-", L"xoxp-", L"AKIA", L"AIza" };
            for (auto p : prefixes) if (StartsWith(t, p)) return ClipKind::Secret;
            if (StartsWith(t, L"eyJ") && std::count(t.begin(), t.end(), L'.') == 2) return ClipKind::Secret;
        }
        if (!spaces && (StartsWith(t, L"https://") || StartsWith(t, L"http://"))) return ClipKind::Url;

        if (!multi && t.size() <= 40)
        {
            std::wstring lower = ToLower(t);
            std::wsmatch m;
            static const std::wregex hex(L"^#([0-9a-f]{3}|[0-9a-f]{4}|[0-9a-f]{6}|[0-9a-f]{8})$");
            static const std::wregex rgb(L"^rgba?\\(\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3}).*\\)$");
            if (std::regex_match(lower, m, hex))
            {
                std::wstring h = m[1].str();
                if (h.size() <= 4) h = std::wstring{ h[0], h[0], h[1], h[1], h[2], h[2] };
                color = static_cast<uint32_t>(std::wcstoul(h.substr(0, 6).c_str(), nullptr, 16));
                return ClipKind::Color;
            }
            if (std::regex_match(lower, m, rgb))
            {
                auto ch = [&](int i) { return static_cast<uint32_t>(std::min(255, std::stoi(m[i].str()))); };
                color = (ch(1) << 16) | (ch(2) << 8) | ch(3);
                return ClipKind::Color;
            }
        }

        if (!multi && spaces)
        {
            static const wchar_t* commands[] = { L"git", L"gh", L"npm", L"npx", L"pnpm", L"yarn", L"dotnet", L"cargo", L"python",
                L"py", L"pip", L"winget", L"cd", L"cmake", L"docker", L"kubectl", L"az", L"code", L"node", L"go", L"msbuild",
                L"curl", L"ssh", L"wsl", L"choco", L"scoop", L"bun", L"deno", L"make", L"mvn", L"gradle", L"terraform" };
            std::wstring first = ToLower(t.substr(0, t.find(L' ')));
            for (auto c : commands) if (first == c) return ClipKind::Command;
        }

        static const wchar_t* codeMarkers[] = { L"{", L"};", L"=>", L"#include", L"def ", L"function ", L"return ", L"</",
            L"import ", L"using ", L"const ", L"let ", L"var ", L"fn ", L"public ", L"class " };
        int indented = 0;
        if (multi)
        {
            size_t pos = 0;
            while ((pos = t.find(L'\n', pos)) != std::wstring::npos)
            {
                ++pos;
                if (pos < t.size() && (t[pos] == L'\t' || t.compare(pos, 2, L"  ") == 0)) ++indented;
            }
        }
        int markers = 0;
        for (auto mk : codeMarkers) if (t.find(mk) != std::wstring::npos) ++markers;
        if ((multi && (markers >= 1 || indented >= 2)) || (!multi && markers >= 2 && t.back() == L';')) return ClipKind::Code;
        return ClipKind::Text;
    }

    void ClipboardWatcher::Start(winrt::Windows::System::DispatcherQueue const& queue, bool mock, size_t capacity)
    {
        if (m_running) return;
        m_queue = queue;
        m_mock = mock;
        m_capacity = std::clamp<size_t>(capacity, 3, 24);
        m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_running = true;
        m_thread = std::thread([this] { Run(); });
        WaitForSingleObject(m_ready, 3000);
    }

    void ClipboardWatcher::Stop()
    {
        if (!m_running.exchange(false)) return;
        if (m_hwnd) PostMessageW(m_hwnd, WM_WATCHER_QUIT, 0, 0);
        if (m_thread.joinable()) m_thread.join();
        if (m_ready) { CloseHandle(m_ready); m_ready = nullptr; }
        Log(L"Clipboard watcher stopped");
    }

    void ClipboardWatcher::Post(Request request)
    {
        if (!m_hwnd) return;
        {
            std::lock_guard lock(m_lock);
            m_requests.push_back(std::move(request));
        }
        PostMessageW(m_hwnd, WM_WATCHER_REQUEST, 0, 0);
    }

    void ClipboardWatcher::SetMock(bool mock) { Post(Request{ Request::Type::Mock, 0, mock }); }
    void ClipboardWatcher::Copy(uint64_t id) { Post(Request{ Request::Type::Copy, id }); }

    void ClipboardWatcher::AddFiles(std::vector<std::wstring> files, std::wstring source)
    {
        Request r{ Request::Type::Files };
        r.files = std::move(files);
        r.source = std::move(source);
        Post(std::move(r));
    }

    void ClipboardWatcher::AddText(std::wstring text, std::wstring source)
    {
        Request r{ Request::Type::Text };
        r.text = std::move(text);
        r.source = std::move(source);
        Post(std::move(r));
    }

    LRESULT CALLBACK ClipboardWatcher::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<ClipboardWatcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
        switch (msg)
        {
        case WM_CLIPBOARDUPDATE:
            // Our own writes are already reflected in the history.
            if (GetClipboardOwner() == hwnd) return 0;
            // Apps often set several formats in bursts; read once things settle.
            SetTimer(hwnd, TIMER_READ, 150, nullptr);
            return 0;
        case WM_TIMER:
            if (wParam == TIMER_READ)
            {
                KillTimer(hwnd, TIMER_READ);
                self->ReadClipboard();
            }
            return 0;
        case WM_WATCHER_REQUEST:
        {
            std::vector<Request> requests;
            {
                std::lock_guard lock(self->m_lock);
                requests.swap(self->m_requests);
            }
            for (auto& r : requests) self->Handle(r);
            return 0;
        }
        case WM_WATCHER_QUIT:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void ClipboardWatcher::Run()
    {
        SetThreadDescription(GetCurrentThread(), L"LiveDesktop clipboard");
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.lpszClassName = kWatcherClass;
        RegisterClassExW(&wc);
        m_hwnd = CreateWindowExW(0, kWatcherClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        SetEvent(m_ready);
        if (!m_hwnd)
        {
            Log(L"Clipboard watcher: window creation failed (%lu)", GetLastError());
            return;
        }
        bool listening = AddClipboardFormatListener(m_hwnd) != FALSE;
        Log(L"Clipboard watcher %s (%s, %zu items, memory only)", listening ? L"listening" : L"could not listen",
            m_mock ? L"mock" : L"real", m_capacity);
        if (m_mock) SeedMock();
        else ReadClipboard();
        Publish();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);

        if (listening) RemoveClipboardFormatListener(m_hwnd);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    void ClipboardWatcher::Handle(Request& r)
    {
        switch (r.type)
        {
        case Request::Type::Mock:
            m_mock = r.flag;
            m_items.clear();
            if (m_mock) SeedMock();
            else ReadClipboard();
            Publish();
            break;
        case Request::Type::Copy:
        {
            auto it = std::find_if(m_items.begin(), m_items.end(), [&](ClipItem const& i) { return i.id == r.id; });
            if (it == m_items.end()) break;
            ClipItem item = *it;
            m_items.erase(it);
            item.at = Clock::now();
            m_items.insert(m_items.begin(), item);
            bool ok = WriteClipboard(item);
            Log(L"Clipboard shelf: copied item %llu back to the clipboard (%s)", item.id, ok ? L"ok" : L"failed");
            Publish();
            break;
        }
        case Request::Type::Files:
        {
            if (r.files.empty()) break;
            ClipItem item;
            item.kind = ClipKind::Files;
            item.files = std::move(r.files);
            item.source = r.source;
            Add(std::move(item), true);
            break;
        }
        case Request::Type::Text:
        {
            if (Trim(r.text).empty()) break;
            ClipItem item;
            item.text = std::move(r.text);
            item.kind = Classify(item.text, item.color);
            item.source = r.source;
            Add(std::move(item), true);
            break;
        }
        }
    }

    void ClipboardWatcher::ReadClipboard()
    {
        static UINT const exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
        static UINT const canInclude = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
        static UINT const viewerIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");

        std::wstring source = OwnerApp(GetClipboardOwner());
        if (!OpenClipboardRetry(m_hwnd))
        {
            Log(L"Clipboard watcher: clipboard busy, skipped one update");
            return;
        }

        bool skip = IsClipboardFormatAvailable(exclude) || IsClipboardFormatAvailable(viewerIgnore);
        if (!skip && IsClipboardFormatAvailable(canInclude))
        {
            if (HANDLE h = GetClipboardData(canInclude))
            {
                if (GlobalSize(h) >= sizeof(DWORD))
                {
                    if (auto const* p = static_cast<DWORD const*>(GlobalLock(h)))
                    {
                        skip = *p == 0;
                        GlobalUnlock(h);
                    }
                }
            }
        }
        if (skip)
        {
            CloseClipboard();
            Log(L"Clipboard watcher: content marked private by %s - not captured", source.empty() ? L"its app" : source.c_str());
            return;
        }

        ClipItem item;
        std::vector<uint8_t> dib;
        bool ok = false;
        if (IsClipboardFormatAvailable(CF_HDROP))
        {
            if (auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP)))
            {
                UINT count = std::min<UINT>(DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0), 256);
                for (UINT i = 0; i < count; ++i)
                {
                    UINT len = DragQueryFileW(drop, i, nullptr, 0);
                    std::wstring path(len, L'\0');
                    DragQueryFileW(drop, i, path.data(), len + 1);
                    if (!path.empty()) item.files.push_back(std::move(path));
                }
                item.kind = ClipKind::Files;
                ok = !item.files.empty();
            }
        }
        else if (IsClipboardFormatAvailable(CF_UNICODETEXT))
        {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
            {
                size_t maxChars = GlobalSize(h) / sizeof(wchar_t);
                if (auto const* p = static_cast<wchar_t const*>(GlobalLock(h)))
                {
                    size_t len = wcsnlen(p, maxChars);
                    if (len <= kMaxTextChars) item.text.assign(p, len);
                    GlobalUnlock(h);
                }
                ok = !Trim(item.text).empty();
            }
        }
        else if (IsClipboardFormatAvailable(CF_DIB))
        {
            if (HANDLE h = GetClipboardData(CF_DIB))
            {
                size_t size = GlobalSize(h);
                if (size > sizeof(BITMAPINFOHEADER) && size <= kMaxDibBytes)
                {
                    if (auto const* p = static_cast<uint8_t const*>(GlobalLock(h)))
                    {
                        dib.assign(p, p + size);
                        GlobalUnlock(h);
                        ok = true;
                    }
                }
            }
        }
        CloseClipboard();
        if (!ok) return;

        item.source = source;
        if (item.kind == ClipKind::Files)
        {
            for (auto const& f : item.files)
            {
                WIN32_FILE_ATTRIBUTE_DATA data{};
                if (GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &data) && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    item.bytes += (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            }
        }
        else if (!dib.empty())
        {
            std::vector<uint32_t> thumb;
            item.kind = ClipKind::Image;
            if (MakeThumbnail(dib, item.imageW, item.imageH, thumb, item.thumbW, item.thumbH))
                item.thumb = std::make_shared<std::vector<uint32_t>>(std::move(thumb));
            item.hash = Fnv(dib.data(), dib.size());
            item.dib = std::make_shared<std::vector<uint8_t>>(std::move(dib));
        }
        else
        {
            item.kind = Classify(item.text, item.color);
        }
        Add(std::move(item), false);
    }

    void ClipboardWatcher::Add(ClipItem item, bool copyToClipboard)
    {
        item.at = Clock::now();
        if (item.hash == 0)
        {
            uint64_t h = Fnv(&item.kind, sizeof(item.kind));
            h = Fnv(item.text.data(), item.text.size() * sizeof(wchar_t), h);
            for (auto const& f : item.files) h = Fnv(f.data(), f.size() * sizeof(wchar_t), h);
            item.hash = h;
        }
        auto dup = std::find_if(m_items.begin(), m_items.end(), [&](ClipItem const& i) { return i.hash == item.hash; });
        if (dup != m_items.end())
        {
            // Copying the same thing again just brings it back to the top (keeps its card).
            item.id = dup->id;
            if (item.source.empty()) item.source = dup->source;
            m_items.erase(dup);
        }
        else
        {
            item.id = m_nextId++;
        }
        m_items.insert(m_items.begin(), std::move(item));
        if (m_items.size() > m_capacity) m_items.resize(m_capacity);
        auto const& top = m_items.front();
        Log(L"Clipboard shelf: + item %llu kind=%d from '%s'%s", top.id, static_cast<int>(top.kind), top.source.c_str(),
            copyToClipboard ? L" (dropped, now on clipboard)" : L"");
        if (copyToClipboard) WriteClipboard(top);
        Publish();
    }

    bool ClipboardWatcher::WriteClipboard(ClipItem const& item)
    {
        if (!OpenClipboardRetry(m_hwnd)) return false;
        EmptyClipboard();
        bool ok = false;
        switch (item.kind)
        {
        case ClipKind::Image:
            ok = item.dib && PutGlobal(CF_DIB, item.dib->data(), item.dib->size());
            break;
        case ClipKind::Files:
        {
            std::vector<uint8_t> buffer(sizeof(DROPFILES));
            auto* df = reinterpret_cast<DROPFILES*>(buffer.data());
            df->pFiles = sizeof(DROPFILES);
            df->fWide = TRUE;
            std::wstring list, text;
            for (auto const& f : item.files)
            {
                list += f;
                list.push_back(L'\0');
                if (!text.empty()) text += L"\r\n";
                text += f;
            }
            list.push_back(L'\0');
            auto const* bytes = reinterpret_cast<uint8_t const*>(list.data());
            buffer.insert(buffer.end(), bytes, bytes + list.size() * sizeof(wchar_t));
            ok = PutGlobal(CF_HDROP, buffer.data(), buffer.size());
            DWORD effect = DROPEFFECT_COPY;
            PutGlobal(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), &effect, sizeof(effect));
            PutGlobal(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            break;
        }
        default:
            ok = PutGlobal(CF_UNICODETEXT, item.text.c_str(), (item.text.size() + 1) * sizeof(wchar_t));
            break;
        }
        CloseClipboard();
        return ok;
    }

    void ClipboardWatcher::SeedMock()
    {
        auto now = Clock::now();
        auto make = [&](ClipKind kind, std::chrono::seconds age, std::wstring source, std::wstring text) {
            ClipItem i;
            i.kind = kind;
            i.at = now - age;
            i.source = std::move(source);
            i.text = std::move(text);
            i.id = m_nextId++;
            i.hash = Fnv(i.text.data(), i.text.size() * sizeof(wchar_t), static_cast<uint64_t>(kind) + 7);
            return i;
        };
        m_items.clear();
        m_items.push_back(make(ClipKind::Code, 1min, L"VS Code",
            L"export function useDevServer(port = 5173) {\n  const [ready, setReady] = useState(false);\n"
            L"  useEffect(() => { ping(port).then(setReady); }, [port]);\n  return ready;\n}"));
        m_items.push_back(make(ClipKind::Url, 4min, L"Edge", L"https://github.com/contoso/webapp/pull/1287"));
        auto color = make(ClipKind::Color, 9min, L"Figma", L"#60CDFF");
        color.color = 0x60CDFF;
        m_items.push_back(color);
        m_items.push_back(make(ClipKind::Command, 14min, L"Terminal", L"git rebase -i origin/main --autosquash"));
        auto image = make(ClipKind::Image, 22min, L"Snipping Tool", L"");
        MockImage(image);
        m_items.push_back(image);
        auto files = make(ClipKind::Files, 35min, L"Explorer", L"");
        files.files = { L"C:\\Users\\you\\Downloads\\design-spec-v3.pdf", L"C:\\Users\\you\\Downloads\\hero@2x.png" };
        files.bytes = 4'412'000;
        files.hash = Fnv(files.files[0].data(), files.files[0].size() * 2);
        m_items.push_back(files);
        // Built at runtime so the source never contains a token-shaped literal.
        m_items.push_back(make(ClipKind::Secret, 52min, L"Edge", std::wstring(L"ghp") + L"_" + std::wstring(36, L'x')));
        m_items.push_back(make(ClipKind::Text, 2h, L"Teams", L"Ship the ambient layer behind a flag - demo Thursday 10:00"));
        if (m_items.size() > m_capacity) m_items.resize(m_capacity);
    }

    void ClipboardWatcher::Publish()
    {
        if (!m_queue) return;
        ClipboardSnapshot snapshot{ m_items, m_mock };
        m_queue.TryEnqueue([this, s = std::move(snapshot)]() { if (OnSnapshot) OnSnapshot(s); });
    }
}
