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

/*  A REAL LV2 PLUGIN, NOTHING BUT A TEST (2026-09-26). Built with the tests
    only, never shipped, and never on a user's LV2 path: it exists so every CI
    runner - which has no plugin of its own - scans and hosts an LV2 through
    JUCE's real LV2 host, and not only through the built-in `godot:test-gain`
    shortcut that bypasses the format altogether.

    Two plugins in one bundle, which is also what makes the scan's de-duplication
    of a bundle's path something a test can see:

      urn:godot:test-lv2-gain    two in, two out, each side times `gain`
      urn:godot:test-lv2-widen   one in, two out: left is in times `gain`,
                                 right is in times `gain` / 2 - a mono source
                                 made stereo, with the sides told apart

    Port 0 is the gain on both, default a half. Plain C, the LV2 core header
    and nothing else: the SDK's own copy, vendored by JUCE. */

#include <lv2/core/lv2.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    int widen;
    const float* gain;
    const float* in[2];
    float* out[2];
} TestPlugin;

static LV2_Handle instantiate (const LV2_Descriptor* descriptor, double rate,
                               const char* bundle, const LV2_Feature* const* features)
{
    TestPlugin* self = (TestPlugin*) calloc (1, sizeof (TestPlugin));
    (void) rate;
    (void) bundle;
    (void) features;

    if (self != NULL)
        self->widen = strcmp (descriptor->URI, "urn:godot:test-lv2-widen") == 0;

    return (LV2_Handle) self;
}

static void connectGain (LV2_Handle instance, uint32_t port, void* data)
{
    TestPlugin* self = (TestPlugin*) instance;

    switch (port)
    {
        case 0: self->gain = (const float*) data; break;
        case 1: self->in[0] = (const float*) data; break;
        case 2: self->in[1] = (const float*) data; break;
        case 3: self->out[0] = (float*) data; break;
        case 4: self->out[1] = (float*) data; break;
        default: break;
    }
}

static void connectWiden (LV2_Handle instance, uint32_t port, void* data)
{
    TestPlugin* self = (TestPlugin*) instance;

    switch (port)
    {
        case 0: self->gain = (const float*) data; break;
        case 1: self->in[0] = (const float*) data; break;
        case 2: self->out[0] = (float*) data; break;
        case 3: self->out[1] = (float*) data; break;
        default: break;
    }
}

static void run (LV2_Handle instance, uint32_t samples)
{
    TestPlugin* self = (TestPlugin*) instance;
    const float gain = self->gain != NULL ? *self->gain : 0.5f;
    uint32_t i;

    if (self->widen)
    {
        for (i = 0; i < samples; ++i)
        {
            const float x = self->in[0][i];
            self->out[0][i] = x * gain;
            self->out[1][i] = x * gain * 0.5f;
        }

        return;
    }

    for (i = 0; i < samples; ++i)
    {
        self->out[0][i] = self->in[0][i] * gain;
        self->out[1][i] = self->in[1][i] * gain;
    }
}

static void cleanup (LV2_Handle instance)
{
    free (instance);
}

static const LV2_Descriptor gainDescriptor =
{
    "urn:godot:test-lv2-gain", instantiate, connectGain, NULL, run, NULL, cleanup, NULL
};

static const LV2_Descriptor widenDescriptor =
{
    "urn:godot:test-lv2-widen", instantiate, connectWiden, NULL, run, NULL, cleanup, NULL
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor (uint32_t index)
{
    switch (index)
    {
        case 0: return &gainDescriptor;
        case 1: return &widenDescriptor;
        default: return NULL;
    }
}
