#pragma once
#include "pch.h"
#include "Util.h"

namespace ld
{
    enum class ClipKind { Text, Code, Command, Url, Color, Image, Files, Secret };

    // One clipboard history entry. Payloads are shared so snapshots are cheap to copy.
    struct ClipItem
    {
        uint64_t id = 0;
        ClipKind kind = ClipKind::Text;
        TimePoint at{};
        std::wstring source;                // owning process ("chrome", "Code"), "drop" for drag & drop
        std::wstring text;                  // Text/Code/Command/Url/Color/Secret payload
        std::vector<std::wstring> files;    // Files
        uint64_t bytes = 0;                 // Files: total size of regular files
        uint32_t color = 0;                 // Color: RGB
        int imageW = 0, imageH = 0;         // Image: original size
        std::shared_ptr<std::vector<uint8_t> const> dib;    // Image: packed CF_DIB, for copying back
        std::shared_ptr<std::vector<uint32_t> const> thumb; // Image: premultiplied BGRA thumbnail
        int thumbW = 0, thumbH = 0;
        uint64_t hash = 0;                  // content hash used to de-duplicate
    };

    struct ClipboardSnapshot
    {
        std::vector<ClipItem> items; // newest first
        bool mock = false;
    };

    // Watches the clipboard (AddClipboardFormatListener) on its own thread with a message-only window,
    // so slow or delay-rendering clipboard owners can never stall the UI thread. History is kept in
    // memory only. Content that apps mark as private is never captured:
    //   ExcludeClipboardContentFromMonitorProcessing, CanIncludeInClipboardHistory = 0, Clipboard Viewer Ignore.
    class ClipboardWatcher
    {
    public:
        ~ClipboardWatcher() { Stop(); }
        void Start(winrt::Windows::System::DispatcherQueue const& queue, bool mock, size_t capacity);
        void Stop();
        void SetMock(bool mock);
        // Puts an item back on the clipboard and moves it to the top of the stack.
        void Copy(uint64_t id);
        // Drag & drop onto the shelf: keep the files/text and put them on the clipboard.
        void AddFiles(std::vector<std::wstring> files, std::wstring source);
        void AddText(std::wstring text, std::wstring source);

        std::function<void(ClipboardSnapshot const&)> OnSnapshot; // UI thread

        static ClipKind Classify(std::wstring const& text, uint32_t& color);

    private:
        struct Request
        {
            enum class Type { Copy, Files, Text, Mock } type{};
            uint64_t id = 0;
            bool flag = false;
            std::vector<std::wstring> files;
            std::wstring text, source;
        };
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        void Run();
        void Post(Request request);
        void Handle(Request& request);
        void ReadClipboard();
        bool WriteClipboard(ClipItem const& item);
        void Add(ClipItem item, bool copyToClipboard);
        void SeedMock();
        void Publish();

        winrt::Windows::System::DispatcherQueue m_queue{ nullptr };
        std::thread m_thread;
        HWND m_hwnd = nullptr;
        HANDLE m_ready = nullptr;
        std::atomic<bool> m_running{ false };
        bool m_mock = false;
        size_t m_capacity = 8;
        uint64_t m_nextId = 1;
        std::vector<ClipItem> m_items; // worker thread only
        std::mutex m_lock;
        std::vector<Request> m_requests;
    };
}
