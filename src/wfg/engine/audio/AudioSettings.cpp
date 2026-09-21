/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <wfg/engine/audio/AudioSettings.h>
#include <wfg/engine/audio/AudioCommands.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/ShowDocument.h>
#include <charconv>
#include <algorithm>
#include <array>
#include <sstream>
#include <locale>

namespace wfg::audio
{
    bool readPatch (const std::string& text, std::vector<int>& result)
    {
        result.clear();
        std::istringstream stream (text);
        stream.imbue (std::locale::classic());
        std::string word;
        std::array<bool, maximumPatchChannels> used {};
        while (stream >> word)
        {
            int channel = -1;
            const auto parsed = std::from_chars (word.data(), word.data() + word.size(), channel);
            if (parsed.ec != std::errc {} || parsed.ptr != word.data() + word.size()
                || channel < -1 || channel >= maximumPatchChannels
                || result.size() >= maximumPatchChannels)
                return false;
            if (channel >= 0)
            {
                if (used[static_cast<std::size_t> (channel)]) return false;
                used[static_cast<std::size_t> (channel)] = true;
            }
            result.push_back (channel);
        }
        return true;
    }

    std::string writePatch (const std::vector<int>& patch)
    {
        std::string text;
        for (const auto channel : patch)
        {
            if (! text.empty()) text += ' ';
            text += std::to_string (channel);
        }
        return text;
    }

    std::vector<int> identityPatch (int channels)
    {
        std::vector<int> patch;
        for (int i = 0; i < std::clamp (channels, 0, maximumPatchChannels); ++i) patch.push_back (i);
        return patch;
    }

    void patchInputs (const std::vector<int>& patch, const float* const* source, int sourceChannels,
                      float* const* destination, int destinationChannels, int frames) noexcept
    {
        for (int row = 0; row < destinationChannels; ++row)
        {
            if (destination[row] == nullptr) continue;
            const auto channel = patch.empty() ? row : row < static_cast<int> (patch.size())
                                                      ? patch[static_cast<std::size_t> (row)] : -1;
            if (channel >= 0 && channel < sourceChannels && source != nullptr && source[channel] != nullptr)
                std::copy_n (source[channel], frames, destination[row]);
            else
                std::fill_n (destination[row], frames, 0.0f);
        }
    }

    void patchOutputs (const std::vector<int>& patch, const float* const* source, int sourceChannels,
                       float* const* destination, int destinationChannels, int frames) noexcept
    {
        for (int channel = 0; channel < destinationChannels; ++channel)
            if (destination[channel] != nullptr) std::fill_n (destination[channel], frames, 0.0f);
        for (int row = 0; row < sourceChannels; ++row)
        {
            const auto channel = patch.empty() ? row : row < static_cast<int> (patch.size())
                                                      ? patch[static_cast<std::size_t> (row)] : -1;
            if (channel >= 0 && channel < destinationChannels && destination[channel] != nullptr
                && source[row] != nullptr)
                std::copy_n (source[row], frames, destination[channel]);
        }
    }

    AudioSettings readAudioSettings (const SettingsReader& read)
    {
        AudioSettings result;
        result.enabled = read ("enabled") == "true";
        result.deviceType = read ("deviceType");
        result.outputDevice = read ("outputDevice");
        result.inputDevice = read ("inputDevice");
        const auto buffer = read ("bufferSize");
        std::from_chars (buffer.data(), buffer.data() + buffer.size(), result.bufferSize);
        result.inputPatch = read ("inputPatch");
        result.outputPatch = read ("outputPatch");
        return result;
    }

    AudioSettings audioSettingsOf (const doc::ShowDocument& document)
    {
        return readAudioSettings ([&] (const std::string& name)
        { return document.getAttribute ("/godot/audio/" + name).value_or (""); });
    }

    bool validAudioSettings (const AudioSettings& settings)
    {
        std::vector<int> patch;
        return settings.bufferSize >= 0 && settings.bufferSize <= 65536
            && readPatch (settings.inputPatch, patch) && readPatch (settings.outputPatch, patch);
    }

    std::string saveAudioDefaults (const std::string& path, const AudioSettings& settings)
    {
        if (! validAudioSettings (settings)) return "Invalid audio defaults";
        juce::XmlElement xml ("AudioDefaults");
        xml.setAttribute ("enabled", settings.enabled ? "true" : "false");
        xml.setAttribute ("deviceType", juce::String (settings.deviceType));
        xml.setAttribute ("outputDevice", juce::String (settings.outputDevice));
        xml.setAttribute ("inputDevice", juce::String (settings.inputDevice));
        xml.setAttribute ("bufferSize", settings.bufferSize);
        xml.setAttribute ("inputPatch", juce::String (settings.inputPatch));
        xml.setAttribute ("outputPatch", juce::String (settings.outputPatch));
        const juce::File file { juce::String (path) };
        if (! file.getParentDirectory().createDirectory()) return "Could not create the audio defaults folder";
        juce::TemporaryFile temporary (file);
        if (! xml.writeTo (temporary.getFile()) || ! temporary.overwriteTargetFileWithTemporary())
            return "Could not save audio defaults";
        return {};
    }

    bool loadAudioDefaults (const std::string& path, AudioSettings& result)
    {
        const auto xml = juce::XmlDocument::parse (juce::File (juce::String (path)));
        if (xml == nullptr || ! xml->hasTagName ("AudioDefaults")) return false;
        auto read = readAudioSettings ([&] (const std::string& name)
        { return xml->getStringAttribute (juce::String (name)).toStdString(); });
        if (! validAudioSettings (read)) return false;
        result = std::move (read);
        return true;
    }

    void registerAudioSettingsCommands (Engine& engine, doc::ShowDocument& document,
                                        cue::Runner& runner, AudioState& state, SettingsRequest request)
    {
        engine.setAdmissionCheck ([&state] (const std::string& command)
        {
            return state.settingsStatus == "applying" && command != "audio.settingsReady"
                && command != "audio.testStop"
                && command != "document.save" && command != "audio.defaults" && command != "audio.defaultsReady"
                ? std::string ("audio-restarting") : std::string {};
        });
        const auto apply = [&] (CommandContext& context, const std::vector<osc::Value>& args,
                               const AudioSettings* changed)
            {
                if (document.isLocked()) return Outcome::rejected (reason::locked);
                for (const auto& run : runner.runTable().all())
                    if (! run.isFinished() && run.state != cue::runState::preparing
                        && ! (run.state == cue::runState::armed && ! run.prepare.empty()))
                        return Outcome::rejected ("audio-busy");
                if (changed != nullptr)
                    if (const auto result = document.configureAudio (*changed); ! result.ok)
                        return Outcome::rejected (result.reason);
                std::vector<std::string> prepared;
                for (const auto& run : runner.runTable().all())
                    if (! run.isFinished()) prepared.push_back (run.id);
                for (const auto& id : prepared) runner.revokePrepared (engine, context.tick, id);
                state.settingsStatus = "applying";
                stopOutputTest (state);
                state.settingsError.clear();
                if (state.requestSettings) state.requestSettings (audioSettingsOf (document), false);
                return Outcome::ok (args);
            };
        engine.commands().add ({ "audio.apply", "Apply this show's audio settings while stopped.", {}, false,
            [apply] (CommandContext& context, const std::vector<osc::Value>& args)
            { return apply (context, args, nullptr); } });
        engine.commands().add ({ "audio.setup", "Edit and apply audio settings while stopped.",
            { { "enabled", 'T', false }, { "deviceType", 's', false }, { "outputDevice", 's', false },
              { "inputDevice", 's', false }, { "bufferSize", 'i', false },
              { "inputPatch", 's', false }, { "outputPatch", 's', false } }, true,
            [apply] (CommandContext& context, const std::vector<osc::Value>& args)
            {
                AudioSettings changed;
                changed.enabled = args[0].getBool();
                changed.deviceType = args[1].getString(); changed.outputDevice = args[2].getString();
                changed.inputDevice = args[3].getString(); changed.bufferSize = args[4].getInt32();
                changed.inputPatch = args[5].getString(); changed.outputPatch = args[6].getString();
                return apply (context, args, &changed);
            } });
        state.requestSettings = std::move (request);
        engine.commands().add ({ "audio.defaultsReady", "The audio defaults write finished.",
            { { "error", 's', false } }, false,
            [&state] (CommandContext&, const std::vector<osc::Value>& args)
            {
                if (! args[0].getString().empty())
                { state.settingsError = args[0].getString(); state.settingsStatus = "error"; }
                return Outcome::ok (args);
            } });
        engine.commands().add ({ "audio.defaults", "Use this show's audio settings for new shows.", {}, false,
            [&] (CommandContext&, const std::vector<osc::Value>& args)
            {
                if (document.isLocked()) return Outcome::rejected (reason::locked);
                if (state.requestSettings) state.requestSettings (audioSettingsOf (document), true);
                return Outcome::ok (args);
            } });
        engine.commands().add ({ "audio.settingsReady", "The audio settings operation finished.",
            { { "error", 's', false }, { "sampleRate", 'i', false }, { "bufferSize", 'i', false },
              { "inputs", 'i', false }, { "outputs", 'i', false }, { "bufferSizes", 's', true } }, false,
            [&] (CommandContext&, const std::vector<osc::Value>& args)
            {
                state.settingsError = args[0].getString();
                ++state.settingsRevision;
                state.settingsStatus = state.settingsError.empty() ? "ready" : "error";
                state.sampleRate = args[1].getInt32();
                state.bufferSize = args[2].getInt32();
                state.inputs = args[3].getInt32();
                state.hardwareOutputs = args[4].getInt32();
                state.availableBufferSizes = args.size() > 5 ? args[5].getString() : std::string {};
                state.status = state.hardwareOutputs > 0 ? "running" : "stopped";
                if (state.hardwareOutputs == 0) { state.device.clear(); state.outputs = 0; }
                if (state.sampleRate > 0) runner.setSamplesPerTick (state.sampleRate / 50);
                runner.resetAudioPreparation();
                return Outcome::ok (args);
            } });
    }
}
