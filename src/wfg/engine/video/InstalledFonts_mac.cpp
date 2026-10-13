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
    MACOS: CoreText's C interface, so no Objective-C. A family is matched by
    its family name, every face of it; a PostScript name by a descriptor of
    that name - kept only when what CoreText matched has that very name, since
    it would otherwise hand back a font it chose instead. Each face's file is
    its URL.
*/

#include <wfg/engine/video/InstalledFonts.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <climits>

namespace wfg::video::installed
{
    namespace
    {
        /** A CoreFoundation object released when it goes. */
        template <typename Ref>
        struct Owned
        {
            Ref ref = nullptr;
            explicit Owned (Ref r = nullptr) noexcept : ref (r) {}
            ~Owned() { if (ref != nullptr) CFRelease (ref); }
            Owned (const Owned&) = delete;
            Owned& operator= (const Owned&) = delete;
        };

        Owned<CFStringRef> stringOf (const std::string& text)
        {
            return Owned<CFStringRef> (CFStringCreateWithCString (kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8));
        }

        void addPathOf (CTFontDescriptorRef descriptor, std::vector<std::string>& paths)
        {
            const Owned<CFURLRef> url (static_cast<CFURLRef> (CTFontDescriptorCopyAttribute (descriptor, kCTFontURLAttribute)));
            if (url.ref == nullptr)
                return;

            char path[PATH_MAX] = {};
            if (! CFURLGetFileSystemRepresentation (url.ref, true, reinterpret_cast<UInt8*> (path), sizeof (path)))
                return;

            std::string found (path);
            if (! found.empty() && std::find (paths.begin(), paths.end(), found) == paths.end())
                paths.push_back (found);
        }
    }

    std::vector<std::string> filesOfFamily (const std::string& family)
    {
        std::vector<std::string> paths;
        if (family.empty())
            return paths;

        const auto name = stringOf (family);
        if (name.ref == nullptr)
            return paths;

        const void* keys[] = { kCTFontFamilyNameAttribute };
        const void* values[] = { name.ref };
        const Owned<CFDictionaryRef> attributes (CFDictionaryCreate (kCFAllocatorDefault, keys, values, 1,
                                                                     &kCFTypeDictionaryKeyCallBacks,
                                                                     &kCFTypeDictionaryValueCallBacks));
        const Owned<CTFontDescriptorRef> wanted (CTFontDescriptorCreateWithAttributes (attributes.ref));
        const Owned<CFArrayRef> matches (CTFontDescriptorCreateMatchingFontDescriptors (wanted.ref, nullptr));
        if (matches.ref == nullptr)
            return paths;

        const auto count = CFArrayGetCount (matches.ref);
        for (CFIndex i = 0; i < count; ++i)
        {
            const auto descriptor = static_cast<CTFontDescriptorRef> (CFArrayGetValueAtIndex (matches.ref, i));
            const Owned<CFStringRef> itsFamily (static_cast<CFStringRef> (CTFontDescriptorCopyAttribute (descriptor, kCTFontFamilyNameAttribute)));
            if (itsFamily.ref != nullptr && CFStringCompare (itsFamily.ref, name.ref, kCFCompareCaseInsensitive) == kCFCompareEqualTo)
                addPathOf (descriptor, paths);
        }
        return paths;
    }

    std::vector<std::string> filesOfPostScript (const std::string& postScript)
    {
        std::vector<std::string> paths;
        if (postScript.empty())
            return paths;

        const auto name = stringOf (postScript);
        if (name.ref == nullptr)
            return paths;

        const Owned<CTFontDescriptorRef> wanted (CTFontDescriptorCreateWithNameAndSize (name.ref, 0.0));
        const Owned<CTFontDescriptorRef> match (CTFontDescriptorCreateMatchingFontDescriptor (wanted.ref, nullptr));
        if (match.ref == nullptr)
            return paths;

        const Owned<CFStringRef> itsName (static_cast<CFStringRef> (CTFontDescriptorCopyAttribute (match.ref, kCTFontNameAttribute)));
        if (itsName.ref != nullptr && CFStringCompare (itsName.ref, name.ref, kCFCompareCaseInsensitive) == kCFCompareEqualTo)
            addPathOf (match.ref, paths);

        return paths;
    }
}
