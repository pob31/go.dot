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
    The global operator new and delete, replaced by counting versions.

    ONE TRANSLATION UNIT FOR THE WHOLE PROGRAM, which is what the standard
    requires: these are replaceable functions with external linkage, and two
    definitions in one binary is undefined behaviour rather than a link error on
    every toolchain. It is compiled only when WFG_RT_CHECKS is on, and it is the
    only file in the tree that defines them.

    EVERY OVERLOAD OR NONE. A partial set is worse than none at all: the compiler
    is free to pair a replaced `new` with the library's `delete`, and on a
    Windows debug heap that is a heap corruption rather than a warning. So all
    fourteen are here, including the sized and over-aligned forms C++17 added,
    and every one of them routes to the same pair of primitives.

    WHAT A COUNTED ALLOCATION COSTS off the audio thread: one thread-local read
    and a branch that is not taken. That is the price of having the check
    available in a build somebody might actually run a show on, and it is why
    this is a build option rather than a test-only artefact.
*/

#include <wfg/engine/rt/RtCounters.h>

#if WFG_RT_CHECKS

#include <cstdlib>
#include <new>

namespace
{
    /*  Zero bytes still allocates: `operator new (0)` must return a distinct,
        freeable pointer, and malloc(0) is allowed to return null. Asking for one
        byte instead is the ordinary way round it. */
    void* allocate (std::size_t bytes) noexcept
    {
        wfg::rt::detail::noteAllocation();
        return std::malloc (bytes != 0 ? bytes : 1);
    }

    void* allocateAligned (std::size_t bytes, std::size_t alignment) noexcept
    {
        wfg::rt::detail::noteAllocation();

        if (bytes == 0)
            bytes = alignment;

       #if defined (_WIN32)
        return _aligned_malloc (bytes, alignment);
       #else
        /*  std::aligned_alloc requires the size to be a multiple of the
            alignment; posix_memalign does not, and is available everywhere
            Go.dot builds that is not Windows. */
        void* result = nullptr;

        if (::posix_memalign (&result, alignment, bytes) != 0)
            return nullptr;

        return result;
       #endif
    }

    void release (void* pointer) noexcept
    {
        std::free (pointer);
    }

    void releaseAligned (void* pointer) noexcept
    {
       #if defined (_WIN32)
        _aligned_free (pointer);
       #else
        std::free (pointer);
       #endif
    }

    /*  The throwing forms must throw std::bad_alloc, and must first give the
        installed new-handler its chance - a program that installed one is
        entitled to have it called, and skipping it would change behaviour that
        has nothing to do with what is being measured. */
    void* allocateOrThrow (std::size_t bytes)
    {
        for (;;)
        {
            if (auto* result = allocate (bytes))
                return result;

            auto handler = std::get_new_handler();

            if (handler == nullptr)
                throw std::bad_alloc();

            handler();
        }
    }

    void* allocateAlignedOrThrow (std::size_t bytes, std::size_t alignment)
    {
        for (;;)
        {
            if (auto* result = allocateAligned (bytes, alignment))
                return result;

            auto handler = std::get_new_handler();

            if (handler == nullptr)
                throw std::bad_alloc();

            handler();
        }
    }
}

//==============================================================================
void* operator new (std::size_t bytes)                              { return allocateOrThrow (bytes); }
void* operator new[] (std::size_t bytes)                            { return allocateOrThrow (bytes); }

void* operator new (std::size_t bytes, const std::nothrow_t&) noexcept   { return allocate (bytes); }
void* operator new[] (std::size_t bytes, const std::nothrow_t&) noexcept { return allocate (bytes); }

void operator delete (void* pointer) noexcept                       { release (pointer); }
void operator delete[] (void* pointer) noexcept                     { release (pointer); }

void operator delete (void* pointer, std::size_t) noexcept          { release (pointer); }
void operator delete[] (void* pointer, std::size_t) noexcept        { release (pointer); }

void operator delete (void* pointer, const std::nothrow_t&) noexcept   { release (pointer); }
void operator delete[] (void* pointer, const std::nothrow_t&) noexcept { release (pointer); }

//==============================================================================
void* operator new (std::size_t bytes, std::align_val_t alignment)
{
    return allocateAlignedOrThrow (bytes, static_cast<std::size_t> (alignment));
}

void* operator new[] (std::size_t bytes, std::align_val_t alignment)
{
    return allocateAlignedOrThrow (bytes, static_cast<std::size_t> (alignment));
}

void* operator new (std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return allocateAligned (bytes, static_cast<std::size_t> (alignment));
}

void* operator new[] (std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return allocateAligned (bytes, static_cast<std::size_t> (alignment));
}

void operator delete (void* pointer, std::align_val_t) noexcept                  { releaseAligned (pointer); }
void operator delete[] (void* pointer, std::align_val_t) noexcept                { releaseAligned (pointer); }
void operator delete (void* pointer, std::size_t, std::align_val_t) noexcept     { releaseAligned (pointer); }
void operator delete[] (void* pointer, std::size_t, std::align_val_t) noexcept   { releaseAligned (pointer); }

void operator delete (void* pointer, std::align_val_t, const std::nothrow_t&) noexcept   { releaseAligned (pointer); }
void operator delete[] (void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned (pointer); }

#endif
