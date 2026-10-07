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

#if defined (_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

#include <wfg/engine/video/Displays.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace wfg::video
{
   #if JUCE_MAC
    /*  The Mac's half, in Displays_mac.mm: each screen's localised name and
        display identifier, with its frame turned into JUCE's top-left
        coordinates so it can be matched to JUCE's list. */
    struct MacScreen
    {
        std::string name;
        std::string id;
        int x = 0, y = 0, width = 0, height = 0;
    };

    std::vector<MacScreen> macScreens();
   #endif

    namespace
    {
       #if defined (_WIN32)
        std::string narrow (const wchar_t* text)
        {
            return juce::String (text).toStdString();
        }

        /*  THE MONITOR'S OWN NAME AND ITS DEVICE PATH, by the GDI name of the
            output it is on (\\.\DISPLAY1), from the display-configuration
            API - which, unlike EnumDisplayDevices, says what the monitor
            calls itself rather than "Generic PnP Monitor". */
        struct WindowsName
        {
            std::string friendly;
            std::string path;
        };

        std::map<std::string, WindowsName> windowsNamesByGdi()
        {
            std::map<std::string, WindowsName> out;

            UINT32 pathCount = 0, modeCount = 0;

            if (::GetDisplayConfigBufferSizes (QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
                return out;

            std::vector<DISPLAYCONFIG_PATH_INFO> paths (pathCount);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes (modeCount);

            if (::QueryDisplayConfig (QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                                      modes.data(), nullptr) != ERROR_SUCCESS)
                return out;

            for (UINT32 n = 0; n < pathCount; ++n)
            {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
                source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                source.header.size = sizeof (source);
                source.header.adapterId = paths[n].sourceInfo.adapterId;
                source.header.id = paths[n].sourceInfo.id;

                DISPLAYCONFIG_TARGET_DEVICE_NAME target {};
                target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                target.header.size = sizeof (target);
                target.header.adapterId = paths[n].targetInfo.adapterId;
                target.header.id = paths[n].targetInfo.id;

                if (::DisplayConfigGetDeviceInfo (&source.header) != ERROR_SUCCESS
                      || ::DisplayConfigGetDeviceInfo (&target.header) != ERROR_SUCCESS)
                    continue;

                out[narrow (source.viewGdiDeviceName)] = { narrow (target.monitorFriendlyDeviceName),
                                                           narrow (target.monitorDevicePath) };
            }

            return out;
        }

        /*  Where each GDI output's top-left corner is, in physical pixels -
            the top-left of JUCE's `physicalBounds`, and how the two lists meet. */
        std::map<std::pair<int, int>, std::string> gdiByCorner()
        {
            std::map<std::pair<int, int>, std::string> out;

            ::EnumDisplayMonitors (nullptr, nullptr,
                                   [] (HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL
                                   {
                                       MONITORINFOEXW info {};
                                       info.cbSize = sizeof (info);

                                       if (::GetMonitorInfoW (monitor, &info))
                                           (*reinterpret_cast<std::map<std::pair<int, int>, std::string>*> (data))
                                               [{ static_cast<int> (info.rcMonitor.left), static_cast<int> (info.rcMonitor.top) }]
                                               = narrow (info.szDevice);

                                       return TRUE;
                                   },
                                   reinterpret_cast<LPARAM> (&out));

            return out;
        }
       #endif
    }

    bool coverDisplay (void* nativeWindow, const DisplayInfo& display)
    {
       #if defined (_WIN32)
        auto* window = static_cast<HWND> (nativeWindow);

        if (window == nullptr || display.physicalWidth <= 0 || display.physicalHeight <= 0)
            return false;

        RECT now {};

        if (GetWindowRect (window, &now) && now.left == display.physicalX && now.top == display.physicalY
              && now.right - now.left == display.physicalWidth && now.bottom - now.top == display.physicalHeight)
            return false;

        SetWindowPos (window, HWND_TOPMOST, display.physicalX, display.physicalY,
                      display.physicalWidth, display.physicalHeight,
                      SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        return true;
       #else
        juce::ignoreUnused (nativeWindow, display);
        return false;
       #endif
    }

    std::vector<DisplayInfo> listDisplays()
    {
        std::vector<DisplayInfo> out;

        const auto& displays = juce::Desktop::getInstance().getDisplays().displays;

       #if defined (_WIN32)
        const auto names = windowsNamesByGdi();
        const auto corners = gdiByCorner();
       #elif JUCE_MAC
        const auto screens = macScreens();
       #endif

        int number = 0;

        for (const auto& display : displays)
        {
            ++number;

            DisplayInfo info;
            const auto bounds = display.logicalBounds.toNearestInt();
            info.x = bounds.getX();
            info.y = bounds.getY();
            info.width = bounds.getWidth();
            info.height = bounds.getHeight();
            info.isMain = display.isMain;

            const auto physical = display.physicalBounds.toNearestInt();
            info.physicalX = physical.getX();
            info.physicalY = physical.getY();
            info.physicalWidth = physical.getWidth();
            info.physicalHeight = physical.getHeight();

            if (display.verticalFrequencyHz.has_value() && *display.verticalFrequencyHz > 1.0)
                info.refreshHz = static_cast<float> (*display.verticalFrequencyHz);

           #if defined (_WIN32)
            const auto corner = corners.find ({ display.physicalBounds.getX(), display.physicalBounds.getY() });

            if (corner != corners.end())
            {
                const auto named = names.find (corner->second);

                if (named != names.end())
                {
                    info.name = named->second.friendly;
                    info.id = named->second.path;
                }

                if (info.id.empty())
                    info.id = corner->second;
            }
           #elif JUCE_MAC
            for (const auto& screen : screens)
                if (screen.x == info.x && screen.y == info.y && screen.width == info.width && screen.height == info.height)
                {
                    info.name = screen.name;
                    info.id = screen.id;
                }
           #endif

            /*  A NAME MADE UP ONLY WHERE THE SYSTEM GAVE NONE - a laptop's own
                panel often has none on Windows - and an identifier made of
                where it is. */
            if (info.name.empty())
                info.name = (info.isMain ? "Main display " : "Display ") + std::to_string (number);

            if (info.id.empty())
                info.id = std::to_string (info.x) + "," + std::to_string (info.y) + " "
                            + std::to_string (info.width) + "x" + std::to_string (info.height);

            out.push_back (std::move (info));
        }

        return out;
    }

    int findDisplay (const std::vector<DisplayInfo>& displays, const std::string& name,
                     const std::string& id, std::string& why)
    {
        if (name.empty() && id.empty())
        {
            why = "no display chosen for it";
            return -1;
        }

        if (! id.empty())
            for (std::size_t n = 0; n < displays.size(); ++n)
                if (displays[n].id == id)
                    return static_cast<int> (n);

        int found = -1;
        int count = 0;

        for (std::size_t n = 0; n < displays.size(); ++n)
            if (displays[n].name == name)
            {
                found = static_cast<int> (n);
                ++count;
            }

        if (count == 1)
            return found;

        why = count == 0 ? "no display called " + name + " on this machine"
                         : "two displays are called " + name + ", and neither is the one it was last on";
        return -1;
    }
}
