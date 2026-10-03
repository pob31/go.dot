/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <wfg/engine/audio/AudioSettings.h>
#include <wfg/engine/audio/AudioCommands.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/ShowDocument.h>
#include <charconv>
#include <algorithm>
#include <array>
#include <sstream>
#include <locale>
#include <string>
#include <vector>

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
                                        cue::Runner& runner, cue::RunTable& runs, AudioState& state,
                                        SettingsRequest request)
    {
        /*  WHETHER THE AUDIO IS OUT, for Doh!'s handler (D4's review, OJ): a
            press it accepts in an outage says so at once on its readout. The
            status `audio.connection` writes - a record, so a replay, which
            registers the same commands, answers the same. */
        runner.setOutage ([&state] { return state.status == "noClock"; });

        /*  WHAT AN OUTAGE LETS THROUGH (namespace draft §11.1). Since
            2026-09-28 also: the engine's own `audio.clockMoved`, and the two
            records a settings operation ends with - a follow, or an Apply made
            during the outage, would otherwise never be heard to finish - and
            Apply itself (decision DI), which still refuses while anything
            plays: the way out when the interface is gone for good, which until
            then was a relaunch. And Doh! (2026-10-01, PRD §3.32), which is
            recovery as Esc is: the pointer goes back at once, and what its
            fades and arms need waits for the clock - with the two records its
            hook submits (2026-10-03, D3): a scene put back once it has ended,
            and the report, which a drain inside the outage must not refuse. */
        engine.setAdmissionCheck ([&state] (const std::string& command)
        {
            if (state.status == "noClock" && command != "audio.connection" && command != "audio.reconnect"
                && command != "audio.clockMoved" && command != "audio.settingsReady"
                && command != "audio.editBuilt" && command != "audio.apply" && command != "audio.setup"
                && command != "run.killAll" && command != "run.kill" && command != "run.stopAll"
                && command != "run.stop" && command != "go.doh" && command != "go.dohRelaunch"
                && command != "list.dohReport" && command != "audio.testStop"
                && command != "audio.armed" && command != "run.failed" && command != "take.closed"
                && command != "take.kept"
                && command != "document.save" && command != "document.autosave"
                && command != "document.saved" && command != "document.writeFailed")
                return std::string ("audio-reconnecting");
            return state.settingsStatus == "applying" && command != "audio.settingsReady"
                && command != "audio.testStop"
                && command != "document.save" && command != "audio.defaults" && command != "audio.defaultsReady"
                ? std::string ("audio-restarting") : std::string {};
        });
        const auto apply = [&] (CommandContext& context, const std::vector<osc::Value>& args,
                               const AudioSettings* changed, bool rebuildOnly)
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

                /*  AND WHAT THEY PRE-SENT IS PUT BACK, by a hook, once the
                    rebuild is over and a write is let in again (2026-10-02,
                    K5's extension, namespace draft §23.16, LD) - the clock
                    move's road. Only preparations are ever revoked here: the
                    door refuses `audio-busy` while anything else is
                    unfinished, so this is setup, and the desk goes back. */
                runner.putBackWhenWritable (prepared);
                state.settingsStatus = "applying";
                stopOutputTest (state);
                state.settingsError.clear();
                if (rebuildOnly)
                {
                    if (state.requestRebuild) state.requestRebuild();
                }
                else if (state.requestSettings) state.requestSettings (audioSettingsOf (document), false);
                return Outcome::ok (args);
            };
        engine.commands().add ({ "audio.apply", "Apply this show's audio settings while stopped.", {}, false,
            [apply] (CommandContext& context, const std::vector<osc::Value>& args)
            { return apply (context, args, nullptr, false); } });

        /*  LOAD NOW (2026-09-26): the plugin set as it stands put into the
            audio graph, which is fixed when it is built (PRD §3.25). The same
            door as audio.apply - refused locked, refused while anything sounds,
            prepared runs revoked, the clock gapped until audio.settingsReady -
            with the interface, rate and block left exactly as they are. */
        engine.commands().add ({ "plugin.load",
            "Rebuild the audio graph with the show's plugin set as it stands, while stopped.", {}, false,
            [apply] (CommandContext& context, const std::vector<osc::Value>& args)
            { return apply (context, args, nullptr, true); } });
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
                return apply (context, args, &changed, false);
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
              { "inputs", 'i', false }, { "outputs", 'i', false }, { "bufferSizes", 's', true },
              /*  AFTER THE LAST ONE A SESSION ALWAYS SENT (Phase 9b), so a log
                  written before them replays as it always did. */
              { "inputLatency", 'i', true }, { "outputLatency", 'i', true } }, false,
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
                state.inputLatency = args.size() > 6 ? std::max (0, args[6].getInt32()) : 0;
                state.outputLatency = args.size() > 7 ? std::max (0, args[7].getInt32()) : 0;
                state.status = state.hardwareOutputs > 0 ? "running" : "stopped";
                if (state.hardwareOutputs == 0) { state.device.clear(); state.outputs = 0; }
                if (state.sampleRate > 0) runner.setSamplesPerTick (state.sampleRate / 50);
                runner.resetAudioPreparation();
                return Outcome::ok (args);
            } });

        /*  THE INTERFACE'S CLOCK MOVED (PRD §6.2, built 2026-09-28): the same
            interface came back from an outage on another rate, block or
            channel count, was asked for the one the show ran on, and would
            not give it up. The engine's watchdog submits this; nothing about
            it is anybody's decision, so it is logged like `audio.connection`
            and a replay re-applies what it did.

            What it does is §6.2's answer - a stop, then an adaptation. The
            prepared runs are revoked, as for any settings operation, with no
            footer - and since K5 what they pre-sent is put back, by a hook,
            once the clock runs again (below, §23.16). Anything
            still playing is stopped THE ESC WAY (decision DG): members come
            down in order and every footer runs, because nobody declared an
            emergency and the rest of the rig should be left as the show says;
            the sound itself went when the outage began. This has to happen
            here, in the handler: the scheduler runs before the commands on
            each tick, so a `run.stopAll` sent from here would reach a group
            one tick after it had seen its member fall silent on the new graph
            and started the next one. Then the Console is asked to follow
            (`followClock`), which ends with `audio.settingsReady`.

            Refused outside an outage: there is no moved clock to follow on an
            interface that is running. */
        engine.commands().add ({ "audio.clockMoved",
            "The interface came back on another clock and will not give it up: stop what plays, and follow it.",
            { { "sampleRate", 'i', false }, { "bufferSize", 'i', false } }, false,
            [&] (CommandContext& context, const std::vector<osc::Value>& args)
            {
                if (state.status != "noClock")
                    return Outcome::rejected ("audio-not-reconnecting");

                const auto to = args[0].getInt32();
                const auto block = args[1].getInt32();

                if (to <= 0 || block <= 0)
                    return Outcome::rejected (reason::badValue);

                std::vector<std::string> prepared;
                auto sounding = false;

                for (const auto& run : runs.all())
                {
                    if (run.isFinished())
                        continue;

                    if (run.state == cue::runState::preparing
                          || (run.state == cue::runState::armed && ! run.prepare.empty()))
                        prepared.push_back (run.id);
                    else
                        sounding = true;
                }

                for (const auto& id : prepared)
                    runner.revokePrepared (engine, context.tick, id);

                /*  AND WHAT THEY PRE-SENT IS PUT BACK, by a hook (2026-10-02,
                    K5, namespace draft §23.16; the author's ruling 6d). Not
                    from here: a handler that submitted the `node.set`s would
                    put them in a replay twice. Handed over instead, and
                    `armStandby` writes them once the outage that refuses every
                    write is over - then makes the standby ready again after
                    them. Hook state only, so a replay is untouched. */
                runner.putBackWhenWritable (prepared);

                if (sounding)
                    cue::stopEveryRoot (runs, false);

                stopOutputTest (state);

                const auto hz = [] (int rate) { return std::to_string (rate) + " Hz"; };
                const auto from = state.sampleRate;

                state.rateMoved = from > 0 && from != to
                                    ? "The interface's clock moved from " + hz (from) + " to " + hz (to)
                                        + "; the show runs at " + hz (to)
                                    : "The interface came back changed, at " + hz (to) + " and "
                                        + std::to_string (block) + "-sample blocks; the show follows it";
                state.rateMoved += sounding ? ", and the cues that were playing were stopped." : ".";
                state.rateMovedTick = context.tick;
                state.settingsError = "The interface's clock moved to " + hz (to) + ": following it.";

                if (state.followClock)
                    state.followClock();

                return Outcome::ok (args);
            } });
    }
}
