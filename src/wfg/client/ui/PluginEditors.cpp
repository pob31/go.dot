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

#include <wfg/client/ui/PluginEditors.h>

#include <wfg/client/model/Fx.h>
#include <wfg/client/model/FxEditor.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/plugin/Catalogue.h>

#include <cmath>
#include <utility>
#include <vector>

namespace wfg::client::ui
{
    namespace
    {
        /** How long an Edit... on an insert the cue had not got waits for the tree to show it. */
        constexpr std::uint32_t waitForCreateMs = 5000;

        /*  A VALUE AS THE ROW SPELLS IT: six places are finer than any hand
            and keep a float's tail (0.30000001) out of the show file. */
        std::string valueText (float value)
        {
            return osc::formatDouble (std::round (static_cast<double> (value) * 1.0e6) / 1.0e6);
        }
    }

    PluginEditors::PluginEditors (Actions actionsToUse,
                                  std::function<std::string (const std::string&)> describeToUse,
                                  std::string workFolderToUse, plugin::EditorLaunch launchToUse, bool headlessToUse)
        : actions (std::move (actionsToUse)), describe (std::move (describeToUse)),
          workFolder (std::move (workFolderToUse)), launch (std::move (launchToUse)), headless (headlessToUse)
    {
    }

    PluginEditors::~PluginEditors()
    {
        stopTimer();
        open.clear();
    }

    plugin::EditorHost* PluginEditors::hostFor (const std::string& pluginId) const
    {
        const auto found = open.find (pluginId);
        return found != open.end() ? found->second.host.get() : nullptr;
    }

    //==============================================================================
    void PluginEditors::say (const std::string& pluginId, const std::string& words)
    {
        if (auto& now = said[pluginId]; now != words)
        {
            now = words;
            ++revision;

            if (actions.changed)
                actions.changed();
        }
    }

    void PluginEditors::hush (const std::string& pluginId)
    {
        if (said.erase (pluginId) > 0)
        {
            ++revision;

            if (actions.changed)
                actions.changed();
        }
    }

    //==============================================================================
    void PluginEditors::edit (const tree::TreeSnapshot& snapshot, const std::string& cueId,
                              const std::string& pluginId, bool locked)
    {
        if (locked)
        {
            say (pluginId, "the show is locked: unlock it to edit its plugins");
            return;
        }

        //  ALREADY UP: forward, and nothing else - the window is following the pick already.
        if (auto* host = hostFor (pluginId);
            host != nullptr && (host->status() == plugin::EditorHost::Status::starting
                                  || host->status() == plugin::EditorHost::Status::open)
                            && ! open[pluginId].leaving)
        {
            host->raise();
            return;
        }

        /*  NOT ON THIS CUE YET: the chain's first switch-in, and the window
            once the tree shows the insert. */
        const auto subject = model::readEditorSubject (snapshot, cueId, pluginId);

        if (subject.greyed && model::text (snapshot, "/godot/cue/" + cueId + "/kind") == "media")
        {
            if (actions.createFx)
                actions.createFx (cueId, pluginId);

            waiting = Waiting { cueId, pluginId, juce::Time::getMillisecondCounter() };
            say (pluginId, "switching it in, then opening its window...");
            startTimer (20);
            return;
        }

        start (snapshot, cueId, pluginId);
    }

    void PluginEditors::start (const tree::TreeSnapshot& snapshot, const std::string& cueId, const std::string& pluginId)
    {
        const auto begin = model::readEditorStart (snapshot, pluginId);

        if (begin.identifier.empty())
        {
            say (pluginId, "the show's set has no such plugin");
            return;
        }

        plugin::EditorSpec spec;
        spec.pluginId = pluginId;
        spec.identifier = begin.identifier;
        spec.name = begin.name;
        spec.presetPath = begin.presetPath;
        spec.workFolder = workFolder;
        spec.sampleRate = begin.sampleRate;
        spec.blockSize = begin.blockSize;
        spec.channels = begin.channels;
        spec.headless = headless;
        spec.launch = launch;

        /*  THE PLUGIN IS MADE FROM THIS MACHINE'S SCAN, which the engine has;
            the built-in test gain needs none. */
        if (begin.identifier != plugin::Catalogue::testGainIdentifier())
        {
            spec.descriptionXml = describe ? describe (begin.identifier) : std::string();

            if (spec.descriptionXml.empty())
            {
                say (pluginId, "this machine's scan does not know it: run wfg plugins --scan");
                return;
            }
        }

        auto host = std::make_unique<plugin::EditorHost> (std::move (spec));

        if (std::string why; ! host->start (why))
        {
            say (pluginId, "its window could not open: " + why);
            return;
        }

        auto& entry = open[pluginId];
        entry = Open {};
        entry.host = std::move (host);

        //  The first subject at once, so the window opens on the cue it was asked for.
        const auto subject = model::readEditorSubject (snapshot, cueId, pluginId);
        entry.host->setSubject ({ subject.cueId, subject.fxId, subject.title, subject.reason,
                                  subject.greyed, subject.values });
        entry.subjectSent = true;
        entry.cueSent = subject.cueId;
        entry.fxSent = subject.fxId;
        entry.titleSent = subject.title;
        entry.reasonSent = subject.reason;
        entry.greyedSent = subject.greyed;
        entry.valuesSent = subject.values;

        say (pluginId, "opening its window...");
        startTimer (20);
    }

    //==============================================================================
    void PluginEditors::follow (const tree::TreeSnapshot& snapshot, const std::string& pickedCueId, bool locked)
    {
        if (locked)
        {
            if (! open.empty() || waiting.has_value())
            {
                for (const auto& [pluginId, entry] : open)
                    say (pluginId, "closed: the show is locked");

                closeAll();
            }

            return;
        }

        /*  AN EDIT... THAT SWITCHED ITS INSERT IN opens when the tree shows
            it - unless the pick moved on, or the tree never did (a create
            refused), in which case it is forgotten and says so no more. */
        if (waiting.has_value())
        {
            const auto late = juce::Time::getMillisecondCounter() - waiting->since > waitForCreateMs;

            if (pickedCueId != waiting->cueId || late)
            {
                hush (waiting->pluginId);
                waiting.reset();
            }
            else if (const auto inserts = model::insertsOf (snapshot, waiting->cueId);
                     inserts.count (waiting->pluginId) > 0)
            {
                const auto w = *waiting;
                waiting.reset();
                start (snapshot, w.cueId, w.pluginId);
            }
        }

        for (auto& [pluginId, entry] : open)
        {
            if (entry.host == nullptr || entry.leaving)
                continue;

            const auto subject = model::readEditorSubject (snapshot, pickedCueId, pluginId);

            const auto sameSubject = entry.subjectSent && subject.cueId == entry.cueSent && subject.fxId == entry.fxSent
                                       && subject.title == entry.titleSent && subject.reason == entry.reasonSent
                                       && subject.greyed == entry.greyedSent;

            if (! sameSubject)
            {
                entry.host->setSubject ({ subject.cueId, subject.fxId, subject.title, subject.reason,
                                          subject.greyed, subject.values });
                entry.subjectSent = true;
                entry.cueSent = subject.cueId;
                entry.fxSent = subject.fxId;
                entry.titleSent = subject.title;
                entry.reasonSent = subject.reason;
                entry.greyedSent = subject.greyed;
                entry.valuesSent = subject.values;
            }
            else if (subject.values != entry.valuesSent)
            {
                entry.host->setLive (subject.values);
                entry.valuesSent = subject.values;
            }
        }
    }

    void PluginEditors::closeAll()
    {
        waiting.reset();

        for (auto& [pluginId, entry] : open)
            if (entry.host != nullptr)
                entry.host->leave();

        open.clear();
        stopTimer();
    }

    //==============================================================================
    void PluginEditors::timerCallback()
    {
        service();
    }

    void PluginEditors::service()
    {
        std::vector<std::string> gone;

        for (auto& [pluginId, entry] : open)
        {
            auto& host = *entry.host;
            host.poll();

            /*  WHAT THE WINDOW DID: the newest value for each parameter,
                written to the insert of the cue it was moved on; its close
                button; the show's keys. */
            std::map<std::pair<std::string, int>, float> moved;

            for (const auto& event : host.drain())
            {
                if (event.kind == plugin::editor::EventKind::value)
                {
                    if (const auto fxId = host.fxIdFor (event.subjectSeq); ! fxId.empty())
                        moved[{ fxId, event.index }] = event.value;
                }
                else if (event.kind == plugin::editor::EventKind::windowClosed)
                {
                    entry.leaving = true;
                    host.leave();
                }
                else if (event.kind == plugin::editor::EventKind::key)
                {
                    if (actions.key)
                        actions.key (event.index == static_cast<int> (plugin::editor::Key::escape));
                }
            }

            for (const auto& [where, value] : moved)
                if (actions.set)
                    actions.set (model::fxParameterAddress (where.first, where.second), valueText (value));

            switch (host.status())
            {
                case plugin::EditorHost::Status::starting:
                    say (pluginId, "opening its window...");
                    break;

                case plugin::EditorHost::Status::open:
                    say (pluginId, entry.leaving ? "closing its window..." : "its window is open");
                    break;

                case plugin::EditorHost::Status::failed:
                    say (pluginId, "its window could not open: " + host.problem());
                    gone.push_back (pluginId);
                    break;

                case plugin::EditorHost::Status::ended:
                    if (entry.leaving)
                        hush (pluginId);
                    else
                        say (pluginId, "its window closed unexpectedly - the voices play on; Edit... opens it again");

                    gone.push_back (pluginId);
                    break;
            }
        }

        for (const auto& pluginId : gone)
            open.erase (pluginId);

        if (open.empty() && ! waiting.has_value())
            stopTimer();
    }
}
