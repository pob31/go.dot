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

#include <wfg/engine/video/render/Ndi.h>

#include <ndi/Processing.NDI.Lib.h>

#include <juce_core/juce_core.h>

#if JUCE_WINDOWS
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <dlfcn.h>
#endif

#include <mutex>
#include <string>
#include <vector>

namespace wfg::video::ndi
{
    namespace
    {
        /*  WHERE TO LOOK, in order: the folder the installers name, then the
            places each system's installers put it, then the system's search. */
        std::vector<std::string> candidates()
        {
            std::vector<std::string> out;
            const std::string name = NDILIB_LIBRARY_NAME;

            for (const auto* variable : { NDILIB_REDIST_FOLDER, "NDI_RUNTIME_DIR_V5" })
            {
                const auto folder = juce::SystemStats::getEnvironmentVariable (variable, {});

                if (folder.isNotEmpty())
                    out.push_back (juce::File (folder).getChildFile (juce::String (name)).getFullPathName().toStdString());
            }

           #if JUCE_MAC
            out.push_back ("/usr/local/lib/" + name);
            out.push_back ("/Library/NDI SDK for Apple/lib/macOS/" + name);
            out.push_back ("/Applications/NDI Video Monitor.app/Contents/Frameworks/" + name);
           #elif JUCE_LINUX
            out.push_back ("/usr/lib/" + name);
            out.push_back ("/usr/local/lib/" + name);
            out.push_back ("/usr/lib/x86_64-linux-gnu/" + name);
           #endif

            //  The system's own search last: a library beside Go.dot, or on its path.
            out.push_back (name);
            return out;
        }

        /*  THE SYSTEM'S OWN LOADER, and never its unloader: NDI's threads
            outlive anything Go.dot holds, and the library let go at the
            program's end under them was an access violation as the process
            left (a loader object's destructor did exactly that). */
        void* openLibrary (const std::string& path)
        {
           #if JUCE_WINDOWS
            return LoadLibraryW (juce::String (path).toWideCharPointer());
           #else
            return dlopen (path.c_str(), RTLD_NOW | RTLD_LOCAL);
           #endif
        }

        void* symbolOf (void* library, const char* name)
        {
           #if JUCE_WINDOWS
            return reinterpret_cast<void*> (GetProcAddress (static_cast<HMODULE> (library), name));
           #else
            return dlsym (library, name);
           #endif
        }

        void closeLibrary (void* library)
        {
           #if JUCE_WINDOWS
            FreeLibrary (static_cast<HMODULE> (library));
           #else
            dlclose (library);
           #endif
        }

        Runtime load()
        {
            Runtime found;

            for (const auto& path : candidates())
            {
                auto* library = openLibrary (path);

                if (library == nullptr)
                    continue;

                using Load = const NDIlib_v6* (*) ();
                const auto entry = reinterpret_cast<Load> (symbolOf (library, "NDIlib_v6_load"));

                if (entry == nullptr)
                {
                    closeLibrary (library);
                    continue;
                }

                const auto* lib = entry();

                if (lib == nullptr || lib->initialize == nullptr || ! lib->initialize())
                {
                    found.problem = "the NDI runtime at " + path + " would not start on this machine";
                    closeLibrary (library);
                    continue;
                }

                found.lib = lib;
                found.path = path;
                found.version = lib->version != nullptr && lib->version() != nullptr ? lib->version() : "";
                found.problem.clear();
                return found;
            }

            if (found.problem.empty())
                found.problem = std::string ("the NDI runtime is not installed - NDI Tools, or the runtime from ") + downloadUrl();

            return found;
        }
    }

    const Runtime& runtime()
    {
        static const Runtime loaded = load();
        return loaded;
    }

    const char* downloadUrl() noexcept
    {
       #if JUCE_LINUX
        return "https://ndi.video/tools/";
       #else
        return NDILIB_REDIST_URL;
       #endif
    }
}
