#include "pch.h"
#include "Snapshot.h"
#include "Log.h"
#include <wincodec.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace ld
{
    namespace
    {
        namespace wgc = winrt::Windows::Graphics::Capture;
        namespace wgd = winrt::Windows::Graphics::DirectX::Direct3D11;

        struct Capture
        {
            winrt::com_ptr<ID3D11Device> d3d;
            winrt::com_ptr<ID3D11DeviceContext> context;
            wgc::Direct3D11CaptureFramePool pool{ nullptr };
            wgc::GraphicsCaptureSession session{ nullptr };
            std::wstring path;
            std::function<void(bool)> done;
            std::atomic<int> frames{ 0 };
            std::atomic<bool> finished{ false };
        };

        std::mutex g_lock;
        std::vector<std::shared_ptr<Capture>> g_pending; // keeps captures alive until their frame arrives

        bool WritePng(std::wstring const& path, uint8_t const* pixels, UINT stride, UINT width, UINT height)
        {
            HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            bool ok = false;
            {
                winrt::com_ptr<IWICImagingFactory> factory;
                winrt::com_ptr<IWICStream> stream;
                winrt::com_ptr<IWICBitmapEncoder> encoder;
                winrt::com_ptr<IWICBitmapFrameEncode> frame;
                WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
                ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))) &&
                     SUCCEEDED(factory->CreateStream(stream.put())) &&
                     SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                     SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) &&
                     SUCCEEDED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) &&
                     SUCCEEDED(encoder->CreateNewFrame(frame.put(), nullptr)) &&
                     SUCCEEDED(frame->Initialize(nullptr)) &&
                     SUCCEEDED(frame->SetSize(width, height)) &&
                     SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA &&
                     SUCCEEDED(frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(pixels))) &&
                     SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
            }
            if (SUCCEEDED(init)) CoUninitialize();
            return ok;
        }

        bool Save(Capture& capture, wgc::Direct3D11CaptureFrame const& frame)
        {
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            winrt::com_ptr<ID3D11Texture2D> staging;
            winrt::check_hresult(capture.d3d->CreateTexture2D(&desc, nullptr, staging.put()));
            capture.context->CopyResource(staging.get(), texture.get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(capture.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
            UINT stride = desc.Width * 4;
            std::vector<uint8_t> pixels(static_cast<size_t>(stride) * desc.Height);
            for (UINT y = 0; y < desc.Height; ++y)
            {
                auto src = static_cast<uint8_t const*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
                auto dst = pixels.data() + static_cast<size_t>(y) * stride;
                memcpy(dst, src, stride);
                for (UINT x = 0; x < desc.Width; ++x) dst[x * 4 + 3] = 255; // the wallpaper is opaque
            }
            capture.context->Unmap(staging.get(), 0);
            return WritePng(capture.path, pixels.data(), stride, desc.Width, desc.Height);
        }

        void Finish(std::shared_ptr<Capture> const& capture, bool ok)
        {
            if (capture->finished.exchange(true)) return;
            try { if (capture->session) capture->session.Close(); } catch (...) {}
            try { if (capture->pool) capture->pool.Close(); } catch (...) {}
            {
                std::lock_guard lock(g_lock);
                std::erase(g_pending, capture);
            }
            if (capture->done) capture->done(ok);
        }
    }

    void SnapshotVisual(winrt::Windows::UI::Composition::Visual const& visual, std::wstring const& path,
                        std::function<void(bool)> done)
    {
        auto capture = std::make_shared<Capture>();
        capture->path = path;
        capture->done = std::move(done);
        try
        {
            winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                                   nullptr, 0, D3D11_SDK_VERSION, capture->d3d.put(), nullptr, capture->context.put()));
            winrt::com_ptr<::IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(capture->d3d.as<IDXGIDevice>().get(), inspectable.put()));
            auto device = inspectable.as<wgd::IDirect3DDevice>();

            auto item = wgc::GraphicsCaptureItem::CreateFromVisual(visual);
            capture->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                device, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
            capture->session = capture->pool.CreateCaptureSession(item);
            try { capture->session.IsCursorCaptureEnabled(false); } catch (...) {}
            {
                std::lock_guard lock(g_lock);
                g_pending.push_back(capture);
            }
            std::weak_ptr<Capture> weak = capture;
            capture->pool.FrameArrived([weak](wgc::Direct3D11CaptureFramePool const& pool, auto&&) {
                auto self = weak.lock();
                if (!self || self->finished) return;
                auto frame = pool.TryGetNextFrame();
                if (!frame) return;
                bool ok = false;
                try { ok = Save(*self, frame); }
                catch (winrt::hresult_error const& e) { Log(L"Snapshot: save failed 0x%08X", static_cast<uint32_t>(e.code())); }
                frame.Close();
                Finish(self, ok);
            });
            capture->session.StartCapture();
        }
        catch (winrt::hresult_error const& e)
        {
            Log(L"Snapshot: capture failed 0x%08X %s", static_cast<uint32_t>(e.code()), e.message().c_str());
            Finish(capture, false);
        }
    }
}
