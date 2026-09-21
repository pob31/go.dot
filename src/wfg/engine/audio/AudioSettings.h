/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace wfg { class Engine; }
namespace wfg::doc { class ShowDocument; }
namespace wfg::cue { class Runner; }

namespace wfg::audio
{
    inline constexpr int maximumPatchChannels = 512;

    struct OutputTestSettings
    {
        int type = 0; // Off, pink noise, tone, sweep, repeating pulse (spatcore order).
        int channel = -1, frequency = 1000;
        double level = -40.0;
        bool hold = false;
        bool operator== (const OutputTestSettings&) const = default;
    };

    // One hardware channel per logical channel, zero based; -1 disconnects.
    // An empty patch preserves the historical identity routing.
    bool readPatch (const std::string&, std::vector<int>&);
    std::string writePatch (const std::vector<int>&);
    std::vector<int> identityPatch (int channels);
    void patchInputs (const std::vector<int>&, const float* const* source, int sourceChannels,
                      float* const* destination, int destinationChannels, int frames) noexcept;
    void patchOutputs (const std::vector<int>&, const float* const* source, int sourceChannels,
                       float* const* destination, int destinationChannels, int frames) noexcept;

    struct AudioSettings
    {
        bool enabled = false;
        std::string deviceType, outputDevice, inputDevice;
        int bufferSize = 0; // zero lets the device choose
        std::string inputPatch, outputPatch;
        bool operator== (const AudioSettings&) const = default;
    };

    using SettingsReader = std::function<std::string (const std::string&)>;
    AudioSettings readAudioSettings (const SettingsReader&);
    AudioSettings audioSettingsOf (const doc::ShowDocument&);
    bool validAudioSettings (const AudioSettings&);

    // Defaults are copied into NEW shows only. Existing shows never read them.
    std::string saveAudioDefaults (const std::string& file, const AudioSettings&);
    bool loadAudioDefaults (const std::string& file, AudioSettings&);

    struct AudioState;
    using SettingsRequest = std::function<void (const AudioSettings&, bool defaultsOnly)>;
    void registerAudioSettingsCommands (Engine&, doc::ShowDocument&, cue::Runner&,
                                        AudioState&, SettingsRequest = {});
}
