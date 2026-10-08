/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

/*
    A PROJECTOR ON WINDOWS (namespace draft §44.4): a child window filling the
    JUCE window - which JUCE 8 paints with Direct2D, and which the device must
    therefore never draw into - and on it a flip-model swap chain, the way
    Windows itself shows a window drawn by Direct3D: the newest frame at the
    display's refresh, never torn, never a change of the display's mode.

    The swap chain has a FRAME LATENCY WAITABLE OBJECT, signalled when it can
    take another frame - at its display's refresh - and at most one frame
    queued: `waitForRefresh` waits on every projector's at once (YJ), so each
    display is drawn for its own refresh and none waits on another's.
*/

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>

#include <wfg/engine/video/render/Projector.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma comment (lib, "user32.lib")
#pragma comment (lib, "gdi32.lib")

namespace wfg::video::render
{
    namespace
    {
        constexpr const wchar_t* className = L"GoDotProjectorView";

        LRESULT CALLBACK viewProc (HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
                /*  BLACK UNTIL THE DEVICE DRAWS: the swap chain's first frame
                    comes a moment after the window - a graphics card waking,
                    the shaders compiled - and a view that painted nothing
                    meanwhile showed whatever the system had, a flash the
                    author saw on 2026-10-08. Once the swap chain shows a frame
                    this painting is never seen. */
                case WM_ERASEBKGND:
                {
                    RECT area {};
                    GetClientRect (window, &area);
                    FillRect (reinterpret_cast<HDC> (wParam), &area, static_cast<HBRUSH> (GetStockObject (BLACK_BRUSH)));
                    return 1;
                }

                case WM_PAINT:
                {
                    PAINTSTRUCT paint {};
                    const auto dc = BeginPaint (window, &paint);
                    FillRect (dc, &paint.rcPaint, static_cast<HBRUSH> (GetStockObject (BLACK_BRUSH)));
                    EndPaint (window, &paint);
                    return 0;
                }

                //  The pointer and the clicks go to the JUCE window under it.
                case WM_NCHITTEST:   return HTTRANSPARENT;

                default:             break;
            }

            return DefWindowProcW (window, message, wParam, lParam);
        }

        bool registerClass()
        {
            static const bool registered = []
            {
                WNDCLASSEXW wc {};
                wc.cbSize = sizeof (wc);
                wc.lpfnWndProc = viewProc;
                wc.hInstance = GetModuleHandleW (nullptr);
                wc.lpszClassName = className;
                wc.hCursor = nullptr;
                wc.hbrBackground = static_cast<HBRUSH> (GetStockObject (BLACK_BRUSH));
                return RegisterClassExW (&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
            }();

            return registered;
        }

        //==============================================================================
        class D3dSurface final : public Surface
        {
        public:
            explicit D3dSurface (HWND windowToDrawIn) : window (windowToDrawIn) {}

            ~D3dSurface() override
            {
                releaseView();

                if (waitable != nullptr)
                    CloseHandle (waitable);

                if (chain != nullptr)
                    chain->Release();
            }

            bool make (std::string& why)
            {
                auto* device = static_cast<ID3D11Device*> (const_cast<void*> (sg_d3d11_device()));

                if (device == nullptr)
                {
                    why = "no Direct3D device";
                    return false;
                }

                sizeNow (width, height);

                IDXGIDevice* dxgi = nullptr;
                IDXGIAdapter* adapter = nullptr;
                IDXGIFactory2* factory = nullptr;

                const auto found = SUCCEEDED (device->QueryInterface (__uuidof (IDXGIDevice), reinterpret_cast<void**> (&dxgi)))
                                && SUCCEEDED (dxgi->GetAdapter (&adapter))
                                && SUCCEEDED (adapter->GetParent (__uuidof (IDXGIFactory2), reinterpret_cast<void**> (&factory)));

                if (found)
                {
                    DXGI_SWAP_CHAIN_DESC1 desc {};
                    desc.Width = static_cast<UINT> (width);
                    desc.Height = static_cast<UINT> (height);
                    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    desc.SampleDesc.Count = 1;
                    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                    desc.BufferCount = 2;
                    desc.Scaling = DXGI_SCALING_NONE;
                    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

                    IDXGISwapChain1* one = nullptr;

                    if (SUCCEEDED (factory->CreateSwapChainForHwnd (device, window, &desc, nullptr, nullptr, &one)))
                    {
                        one->QueryInterface (__uuidof (IDXGISwapChain2), reinterpret_cast<void**> (&chain));
                        one->Release();
                    }

                    //  Alt+Enter never makes a projector take its display over.
                    factory->MakeWindowAssociation (window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
                }

                if (factory != nullptr)  factory->Release();
                if (adapter != nullptr)  adapter->Release();
                if (dxgi != nullptr)     dxgi->Release();

                if (chain == nullptr)
                {
                    why = "Windows would not make a swap chain for the projector's window";
                    return false;
                }

                chain->SetMaximumFrameLatency (1);
                waitable = chain->GetFrameLatencyWaitableObject();
                return makeView();
            }

            bool acquire (sg_swapchain& swapchain) override
            {
                int nowWidth = 0, nowHeight = 0;
                sizeNow (nowWidth, nowHeight);

                if (nowWidth != width || nowHeight != height)
                {
                    releaseView();

                    if (FAILED (chain->ResizeBuffers (0, static_cast<UINT> (nowWidth), static_cast<UINT> (nowHeight),
                                                      DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)))
                        return false;

                    width = nowWidth;
                    height = nowHeight;

                    if (! makeView())
                        return false;
                }

                if (view == nullptr || IsIconic (GetAncestor (window, GA_ROOT)))
                    return false;

                swapchain = {};
                swapchain.width = width;
                swapchain.height = height;
                swapchain.color_format = SG_PIXELFORMAT_BGRA8;
                swapchain.depth_format = SG_PIXELFORMAT_NONE;
                swapchain.sample_count = 1;
                swapchain.d3d11.render_view = view;
                drawn = true;
                return true;
            }

            void present() override
            {
                if (drawn)
                    chain->Present (1, 0);

                drawn = false;
            }

            HANDLE waitable = nullptr;

        private:
            void sizeNow (int& w, int& h) const
            {
                RECT client {};
                GetClientRect (window, &client);
                w = std::max (1, static_cast<int> (client.right - client.left));
                h = std::max (1, static_cast<int> (client.bottom - client.top));
            }

            bool makeView()
            {
                auto* device = static_cast<ID3D11Device*> (const_cast<void*> (sg_d3d11_device()));
                ID3D11Texture2D* buffer = nullptr;

                if (FAILED (chain->GetBuffer (0, __uuidof (ID3D11Texture2D), reinterpret_cast<void**> (&buffer))))
                    return false;

                const auto made = SUCCEEDED (device->CreateRenderTargetView (buffer, nullptr, &view));
                buffer->Release();
                return made;
            }

            void releaseView()
            {
                if (view != nullptr)
                {
                    //  The context lets go of it before the buffers are resized.
                    auto* context = static_cast<ID3D11DeviceContext*> (const_cast<void*> (sg_d3d11_device_context()));

                    if (context != nullptr)
                    {
                        context->OMSetRenderTargets (0, nullptr, nullptr);
                        context->Flush();
                    }

                    view->Release();
                    view = nullptr;
                    sg_reset_state_cache();
                }
            }

            HWND window = nullptr;
            IDXGISwapChain2* chain = nullptr;
            ID3D11RenderTargetView* view = nullptr;
            int width = 1;
            int height = 1;
            bool drawn = false;
        };
    }

    //==============================================================================
    void* nativeDisplay()
    {
        return nullptr;
    }

    NativeView makeNativeView (void* parentWindow, int, int)
    {
        NativeView made;

        if (parentWindow == nullptr || ! registerClass())
            return made;

        /*  THE WINDOW'S OWN PIXELS from the start, never JUCE's idea of its
            size: on a display scaled otherwise than the main one the two
            differ, and a view made at the smaller showed the picture in a
            rectangle at the top left until the next check fitted it (the
            author's report of 2026-10-08). */
        RECT client {};
        GetClientRect (static_cast<HWND> (parentWindow), &client);

        made.handle = CreateWindowExW (WS_EX_NOPARENTNOTIFY, className, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                                       0, 0, std::max (1, static_cast<int> (client.right - client.left)),
                                       std::max (1, static_cast<int> (client.bottom - client.top)),
                                       static_cast<HWND> (parentWindow), nullptr, GetModuleHandleW (nullptr), nullptr);
        return made;
    }

    void fitNativeView (const NativeView& view, void* parentWindow, int, int)
    {
        if (view.handle == nullptr || parentWindow == nullptr)
            return;

        /*  The window's own pixels, whatever JUCE thinks its size is: the
            parent was put on the display's pixels (§39). */
        RECT client {};
        GetClientRect (static_cast<HWND> (parentWindow), &client);
        SetWindowPos (static_cast<HWND> (view.handle), nullptr, 0, 0, client.right - client.left, client.bottom - client.top,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    }

    void destroyNativeView (NativeView& view)
    {
        if (view.handle != nullptr)
            DestroyWindow (static_cast<HWND> (view.handle));

        view = {};
    }

    std::unique_ptr<Surface> makeSurface (const NativeView& view, std::string& why)
    {
        if (view.handle == nullptr)
        {
            why = "the projector's window has no view to draw into";
            return nullptr;
        }

        auto surface = std::make_unique<D3dSurface> (static_cast<HWND> (view.handle));

        if (! surface->make (why))
            return nullptr;

        return surface;
    }

    bool waitForRefresh (std::vector<Surface*>& surfaces, double, int timeoutMs)
    {
        std::vector<HANDLE> handles;
        std::vector<Surface*> owners;

        for (auto* surface : surfaces)
            if (auto* d3d = static_cast<D3dSurface*> (surface); d3d != nullptr && d3d->waitable != nullptr)
            {
                handles.push_back (d3d->waitable);
                owners.push_back (surface);
            }

        if (handles.empty())
        {
            Sleep (static_cast<DWORD> (std::max (1, timeoutMs)));
            return false;
        }

        /*  THE FIRST DISPLAY READY wakes it; every other ready at that moment
            is drawn in the same pass, and one a moment later in the next. */
        const auto count = static_cast<DWORD> (std::min<std::size_t> (handles.size(), MAXIMUM_WAIT_OBJECTS));
        const auto woke = WaitForMultipleObjects (count, handles.data(), FALSE, static_cast<DWORD> (std::max (1, timeoutMs)));

        if (woke >= WAIT_OBJECT_0 + count)
            return false;

        const auto first = static_cast<std::size_t> (woke - WAIT_OBJECT_0);
        owners[first]->due = true;

        for (std::size_t n = 0; n < count; ++n)
            if (n != first && WaitForSingleObject (handles[n], 0) == WAIT_OBJECT_0)
                owners[n]->due = true;

        return true;
    }

    FramePool::FramePool() = default;
    FramePool::~FramePool() = default;
}
