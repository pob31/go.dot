/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace wfg { class Engine; }
namespace wfg::doc { class ShowDocument; }
namespace wfg::cue { class Runner; class RunTable; }

namespace wfg::audio
{
    inline constexpr int maximumPatchChannels = 512;

    struct OutputTestSettings
    {
        int type = 0; // Off, pink noise, tone, sweep, repeating pulse (spatcore order).
        int channel = -1, frequency = 1000;
        double level = -40.0;
        bool hold = false;
        /*  Exact, and said without `==` on a double: clang's -Wfloat-equal
            trips on a defaulted comparison, as the strict build's would. */
        bool operator== (const OutputTestSettings& other) const noexcept
        {
            return type == other.type && channel == other.channel && frequency == other.frequency
                && ! (level < other.level) && ! (other.level < level) && hold == other.hold;
        }
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

    /*  WHAT A NEW SHOW TAKES OF THE DEFAULTS: the interface, its devices and
        its buffer - and NOT the two patches (the author's round of 2026-11-23:
        a new show's Main L/R was found pushed down the interface). A patch says
        where THAT show's outputs and inputs land; a new show has one output of
        its own and none of the other's, so a patch carried over put its Main
        wherever the other show's first output was, counted the show as patched
        by hand from birth, and gave every output added after it the channels
        past the highest the other show used. Empty, the new show follows its
        own list until something plays or somebody patches it. */
    AudioSettings forNewShow (AudioSettings defaults);

    struct AudioState;
    using SettingsRequest = std::function<void (const AudioSettings&, bool defaultsOnly)>;
    void registerAudioSettingsCommands (Engine&, doc::ShowDocument&, cue::Runner&, cue::RunTable&,
                                        AudioState&, SettingsRequest = {});
}
