#include "pch.h"
#include "Scene.h"
#include "Log.h"

using namespace std::chrono_literals;

namespace ld
{
    namespace
    {
        // Explicitly release a subtree so DWM resources are freed promptly on effect switch.
        void CloseTree(wuc::Visual const& visual)
        {
            if (auto container = visual.try_as<wuc::ContainerVisual>())
            {
                std::vector<wuc::Visual> children;
                for (auto const& child : container.Children()) children.push_back(child);
                container.Children().RemoveAll();
                for (auto const& child : children) CloseTree(child);
            }
            visual.Close();
        }

        float PointerRadius(std::vector<RECT> const& monitors)
        {
            float minDim = 100000.0f;
            for (auto const& rc : monitors)
            {
                minDim = std::min(minDim, static_cast<float>(std::min(rc.right - rc.left, rc.bottom - rc.top)));
            }
            return std::max(280.0f, minDim * 0.32f);
        }
    }

    void Scene::Initialize()
    {
        // A DispatcherQueue on the UI thread is required for Windows.UI.Composition in Win32 apps.
        DispatcherQueueOptions options{ sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };
        winrt::check_hresult(CreateDispatcherQueueController(options,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(m_dispatcher))));

        m_ctx.compositor = wuc::Compositor();
        m_root = m_ctx.compositor.CreateContainerVisual();

        m_ctx.pointer = m_ctx.compositor.CreatePropertySet();
        m_ctx.pointer.InsertVector3(L"Position", float3(-10000.0f, -10000.0f, 0.0f));
        m_ctx.pointer.InsertScalar(L"Strength", 0.0f);
        m_ctx.pointer.InsertScalar(L"Radius", 400.0f);

        auto easeOut = m_ctx.compositor.CreateCubicBezierEasingFunction(float2(0.1f, 0.6f), float2(0.3f, 1.0f));
        m_followAnimation = m_ctx.compositor.CreateVector3KeyFrameAnimation();
        m_followAnimation.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
        m_followAnimation.InsertExpressionKeyFrame(1.0f, L"target", easeOut);
        m_followAnimation.Duration(260ms);

        m_strengthAnimation = m_ctx.compositor.CreateScalarKeyFrameAnimation();
        m_strengthAnimation.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
        m_strengthAnimation.InsertExpressionKeyFrame(1.0f, L"target", easeOut);
        m_strengthAnimation.Duration(450ms);

        m_effects = CreateEffects();
        Log(L"Compositor ready (%zu effects). GlobalPlaybackRate range [%.3f, %.3f]", m_effects.size(),
            wuc::Compositor::MinGlobalPlaybackRate(), wuc::Compositor::MaxGlobalPlaybackRate());
    }

    void Scene::AttachToWindow(HWND hwnd, float2 size, std::vector<RECT> monitors)
    {
        DetachFromWindow();
        auto interop = m_ctx.compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        winrt::check_hresult(interop->CreateDesktopWindowTarget(hwnd, FALSE,
            reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(m_target))));
        m_target.Root(m_root);
        Resize(size, std::move(monitors));
    }

    void Scene::DetachFromWindow()
    {
        if (m_target)
        {
            m_target.Root(nullptr);
            m_target.Close();
            m_target = nullptr;
        }
    }

    void Scene::Resize(float2 size, std::vector<RECT> monitors)
    {
        m_ctx.size = size;
        m_ctx.monitors = std::move(monitors);
        m_ctx.pointer.InsertScalar(L"Radius", PointerRadius(m_ctx.monitors));
        m_root.Size(size);
        Rebuild(false);
    }

    void Scene::NextEffect()
    {
        SetEffect((m_current + 1) % m_effects.size());
    }

    void Scene::SetEffect(size_t index)
    {
        m_current = index % m_effects.size();
        Log(L"Effect -> %s", CurrentEffectName());
        Rebuild(true);
    }

    const wchar_t* Scene::CurrentEffectName() const
    {
        return m_effects.empty() ? L"" : m_effects[m_current]->Name();
    }

    void Scene::Rebuild(bool crossfade)
    {
        if (!m_root || m_ctx.size.x <= 0 || m_ctx.size.y <= 0) return;
        auto const& c = m_ctx.compositor;

        auto layer = c.CreateContainerVisual();
        layer.Size(m_ctx.size);
        m_effects[m_current]->Build(m_ctx, layer);

        auto old = m_layer;
        m_layer = layer;
        m_root.Children().InsertAtTop(layer);

        if (old && crossfade)
        {
            auto fadeIn = c.CreateScalarKeyFrameAnimation();
            fadeIn.InsertKeyFrame(0.0f, 0.0f);
            fadeIn.InsertKeyFrame(1.0f, 1.0f);
            fadeIn.Duration(900ms);
            auto batch = c.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
            layer.StartAnimation(L"Opacity", fadeIn);
            batch.End();
            auto root = m_root;
            batch.Completed([root, old](auto&&, auto&&)
            {
                root.Children().Remove(old);
                CloseTree(old);
            });
        }
        else if (old)
        {
            m_root.Children().Remove(old);
            CloseTree(old);
        }
    }

    void Scene::PointerMoved(float2 pos, bool overDesktop)
    {
        if (!m_ctx.pointer) return;
        if (overDesktop)
        {
            if (!m_pointerOver)
            {
                // Re-entering: jump to the new position instead of sweeping across the screen.
                m_ctx.pointer.StopAnimation(L"Position");
                m_ctx.pointer.InsertVector3(L"Position", float3(pos.x, pos.y, 0.0f));
            }
            else
            {
                m_followAnimation.SetVector3Parameter(L"target", float3(pos.x, pos.y, 0.0f));
                m_ctx.pointer.StartAnimation(L"Position", m_followAnimation);
            }
        }
        if (overDesktop != m_pointerOver)
        {
            m_pointerOver = overDesktop;
            m_strengthAnimation.SetScalarParameter(L"target", overDesktop ? 1.0f : 0.0f);
            m_ctx.pointer.StartAnimation(L"Strength", m_strengthAnimation);
        }
    }

    void Scene::Click(float2 pos)
    {
        if (m_layer && !m_paused) m_effects[m_current]->OnClick(m_ctx, m_layer, pos);
    }

    void Scene::SetPaused(bool paused)
    {
        if (paused == m_paused || !m_ctx.compositor) return;
        m_paused = paused;
        // Freezes every running animation in this compositor (DWM stops producing new frames for us).
        m_ctx.compositor.GlobalPlaybackRate(paused ? std::max(0.0f, wuc::Compositor::MinGlobalPlaybackRate()) : 1.0f);
    }

    void Scene::Shutdown()
    {
        DetachFromWindow();
        if (m_layer)
        {
            CloseTree(m_layer);
            m_layer = nullptr;
        }
        m_effects.clear();
        m_followAnimation = nullptr;
        m_strengthAnimation = nullptr;
        if (m_root) { m_root.Close(); m_root = nullptr; }
        if (m_ctx.pointer) { m_ctx.pointer.Close(); m_ctx.pointer = nullptr; }
        if (m_ctx.compositor) { m_ctx.compositor.Close(); m_ctx.compositor = nullptr; }

        if (m_dispatcher)
        {
            // Drain the dispatcher queue so it shuts down cleanly before the thread exits.
            bool done = false;
            auto op = m_dispatcher.ShutdownQueueAsync();
            op.Completed([&done](auto&&, auto&&) { done = true; });
            ULONGLONG deadline = GetTickCount64() + 2000;
            MSG msg;
            while (!done && GetTickCount64() < deadline)
            {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (!done) MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            }
            m_dispatcher = nullptr;
        }
    }
}
