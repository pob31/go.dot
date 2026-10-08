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
    THE DEVICE ON WINDOWS: Direct3D 11 (namespace draft §44.4), the API Spout
    shares its pictures in. On the graphics card driving the first projector's
    display - on a laptop with two, the one it is wired to, so no picture
    crosses from one card to the other - or, with no projector, the fastest;
    or WARP, Microsoft's software rasteriser, which every Windows has and
    every test draws on.
*/

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include <wfg/engine/video/render/GpuNative.h>

#include <string>
#include <vector>

#pragma comment (lib, "d3d11.lib")
#pragma comment (lib, "dxgi.lib")

namespace wfg::video::gpu::native
{
    namespace
    {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;

        std::string utf8 (const wchar_t* wide)
        {
            const auto bytes = WideCharToMultiByte (CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);

            if (bytes <= 1)
                return {};

            std::string out (static_cast<std::size_t> (bytes - 1), '\0');
            WideCharToMultiByte (CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
            return out;
        }

        /*  THE CARD DRIVING THE DISPLAY AT (x, y), or with none named the one
            Windows calls the fastest; null for Windows's own choice. */
        IDXGIAdapter1* adapterFor (const OpenOptions& options)
        {
            IDXGIFactory1* factory = nullptr;

            if (FAILED (CreateDXGIFactory1 (__uuidof (IDXGIFactory1), reinterpret_cast<void**> (&factory))))
                return nullptr;

            IDXGIAdapter1* chosen = nullptr;

            if (options.hasPoint)
            {
                IDXGIAdapter1* adapter = nullptr;

                for (UINT n = 0; chosen == nullptr && factory->EnumAdapters1 (n, &adapter) != DXGI_ERROR_NOT_FOUND; ++n)
                {
                    IDXGIOutput* output = nullptr;

                    for (UINT m = 0; chosen == nullptr && adapter->EnumOutputs (m, &output) != DXGI_ERROR_NOT_FOUND; ++m)
                    {
                        DXGI_OUTPUT_DESC desc {};

                        if (SUCCEEDED (output->GetDesc (&desc)))
                        {
                            const auto& r = desc.DesktopCoordinates;

                            if (options.pointX >= r.left && options.pointX < r.right
                                  && options.pointY >= r.top && options.pointY < r.bottom)
                            {
                                adapter->AddRef();
                                chosen = adapter;
                            }
                        }

                        output->Release();
                    }

                    adapter->Release();
                }
            }

            if (chosen == nullptr)
            {
                IDXGIFactory6* six = nullptr;

                if (SUCCEEDED (factory->QueryInterface (__uuidof (IDXGIFactory6), reinterpret_cast<void**> (&six))))
                {
                    six->EnumAdapterByGpuPreference (0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof (IDXGIAdapter1),
                                                     reinterpret_cast<void**> (&chosen));
                    six->Release();
                }
            }

            factory->Release();
            return chosen;
        }

        std::string adapterOf (ID3D11Device* made)
        {
            IDXGIDevice* dxgi = nullptr;
            std::string name;

            if (SUCCEEDED (made->QueryInterface (__uuidof (IDXGIDevice), reinterpret_cast<void**> (&dxgi))))
            {
                IDXGIAdapter* adapter = nullptr;

                if (SUCCEEDED (dxgi->GetAdapter (&adapter)))
                {
                    DXGI_ADAPTER_DESC desc {};

                    if (SUCCEEDED (adapter->GetDesc (&desc)))
                        name = utf8 (desc.Description);

                    adapter->Release();
                }

                dxgi->Release();
            }

            return name;
        }
    }

    const char* backendName() noexcept
    {
        return "Direct3D 11";
    }

    bool open (const OpenOptions& options, sg_environment& environment, std::string& adapter, std::string& why)
    {
        const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got {};

        /*  BGRA SUPPORT: the swap chains and the pictures JUCE reads are BGRA;
            a device without it is a Direct3D 10 card, which this renderer does
            not run on. */
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

        auto* card = options.software ? nullptr : adapterFor (options);
        const auto type = options.software ? D3D_DRIVER_TYPE_WARP
                                           : (card != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE);

        auto made = D3D11CreateDevice (card, type, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device, &got, &context);

        //  An older runtime that does not know 11.1 is asked for 11.0 alone.
        if (made == E_INVALIDARG)
            made = D3D11CreateDevice (card, type, nullptr, flags, levels + 1, 1, D3D11_SDK_VERSION, &device, &got, &context);

        if (card != nullptr)
            card->Release();

        if (FAILED (made) || device == nullptr || context == nullptr)
        {
            close();
            why = "Direct3D 11 would not start (" + std::to_string (static_cast<long> (made)) + ")";
            return false;
        }

        adapter = adapterOf (device);
        environment.d3d11.device = device;
        environment.d3d11.device_context = context;
        environment.defaults.color_format = SG_PIXELFORMAT_BGRA8;
        return true;
    }

    void close()
    {
        if (context != nullptr)
        {
            context->ClearState();
            context->Flush();
            context->Release();
            context = nullptr;
        }

        if (device != nullptr)
        {
            device->Release();
            device = nullptr;
        }
    }

    bool readBack (sg_image image, int width, int height, sg_pixel_format format, std::vector<float>& rgba)
    {
        auto* texture = static_cast<ID3D11Texture2D*> (const_cast<void*> (sg_d3d11_query_image_info (image).tex2d));

        if (texture == nullptr || device == nullptr || context == nullptr)
            return false;

        /*  A COPY THE CPU CAN READ: Direct3D draws into memory only the GPU
            reaches, so the picture is copied into a staging texture and that
            mapped - which waits for the drawing to be done. */
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc (&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;

        ID3D11Texture2D* staging = nullptr;

        if (FAILED (device->CreateTexture2D (&desc, nullptr, &staging)))
            return false;

        context->CopyResource (staging, texture);

        D3D11_MAPPED_SUBRESOURCE mapped {};
        const auto ok = SUCCEEDED (context->Map (staging, 0, D3D11_MAP_READ, 0, &mapped));

        if (ok)
        {
            rgba.assign (static_cast<std::size_t> (width) * static_cast<std::size_t> (height) * 4, 0.0f);

            for (int y = 0; y < height; ++y)
                rowToFloats (static_cast<const std::uint8_t*> (mapped.pData) + static_cast<std::size_t> (y) * mapped.RowPitch,
                             width, format, rgba.data() + static_cast<std::size_t> (y) * static_cast<std::size_t> (width) * 4);

            context->Unmap (staging, 0);
        }

        staging->Release();
        return ok;
    }
}
