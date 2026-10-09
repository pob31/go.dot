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

#include <wfg/engine/process/PatchEditor.h>

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/video/PipedChild.h>

#include <juce_core/juce_core.h>


namespace wfg::process::editor
{
    namespace
    {
        constexpr const char* pdRelease = "0.56-5";

        juce::File fileOf (const std::string& path)
        {
            return juce::File (juce::String::fromUTF8 (path.c_str()));
        }

        /*  A program by its name, on the path. */
        [[maybe_unused]] std::optional<juce::File> onPath (const char* name)
        {
            const auto path = juce::SystemStats::getEnvironmentVariable ("PATH", {});
            if (path.isEmpty())
                return std::nullopt;

           #if JUCE_WINDOWS
            const juce::String separator (";");
            const juce::String suffix (".exe");
           #else
            const juce::String separator (":");
            const juce::String suffix;
           #endif

            for (const auto& folder : juce::StringArray::fromTokens (path, separator, {}))
            {
                if (folder.isEmpty() || ! juce::File::isAbsolutePath (folder))
                    continue;
                const auto program = juce::File (folder).getChildFile (juce::String (name) + suffix);
                if (program.existsAsFile())
                    return program;
            }
            return std::nullopt;
        }

        /*  The program inside a Mac application: Contents/MacOS/<its first file>. */
        [[maybe_unused]] std::optional<juce::File> insideApp (const juce::File& app)
        {
            const auto programs = app.getChildFile ("Contents").getChildFile ("MacOS")
                                    .findChildFiles (juce::File::findFiles, false);
            if (programs.isEmpty())
                return std::nullopt;
            return programs.getFirst();
        }

        /*  A Pd in a folder Go.dot unpacked: bin/pd.exe on Windows, an .app on the Mac. */
        [[maybe_unused]] std::optional<juce::File> pdIn (const juce::File& folder)
        {
            if (! folder.isDirectory())
                return std::nullopt;

           #if JUCE_WINDOWS || JUCE_MAC
            for (const auto& child : folder.findChildFiles (juce::File::findFilesAndDirectories, false))
            {
               #if JUCE_WINDOWS
                if (const auto pd = child.getChildFile ("bin").getChildFile ("pd.exe"); pd.existsAsFile())
                    return pd;
               #else
                if (child.getFileExtension() == ".app")
                    if (auto inside = insideApp (child))
                        return inside;
               #endif
            }
           #endif

            //  Linux has no download: its Pd is the system's package.
            return std::nullopt;
        }

        [[maybe_unused]] juce::File programFiles()
        {
            return juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory);
        }

        struct Archive
        {
            const char* url;
        };

        std::optional<Archive> archive()
        {
           #if JUCE_WINDOWS
            return Archive { "https://msp.ucsd.edu/Software/pd-0.56-5.msw.zip" };
           #elif JUCE_MAC
            return Archive { "https://msp.ucsd.edu/Software/pd-0.56-5.macos.zip" };
           #else
            return std::nullopt;
           #endif
        }
    }

    std::string installFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                 .getChildFile ("Go.dot").getChildFile ("pd").getFullPathName().toStdString();
    }

    std::optional<Found> find()
    {
        const auto found = [] (const juce::File& program, const char* name) -> std::optional<Found>
        {
            if (! program.existsAsFile())
                return std::nullopt;
            return Found { program.getFullPathName().toStdString(), name };
        };

        //  WFG_PD: a program, or on the Mac an application.
        if (const auto named = juce::SystemStats::getEnvironmentVariable ("WFG_PD", {}); named.isNotEmpty())
        {
            const auto file = juce::File (named);
            if (file.getFileExtension() == ".app")
            {
                if (const auto inside = insideApp (file))
                    return Found { inside->getFullPathName().toStdString(), "Pd" };
            }
            else if (auto pd = found (file, "Pd"))
            {
                return pd;
            }
        }

        //  plugdata, where it is installed.
       #if JUCE_WINDOWS
        if (auto plugdata = found (programFiles().getChildFile ("plugdata").getChildFile ("plugdata.exe"), "plugdata"))
            return plugdata;
       #elif JUCE_MAC
        if (const auto inside = insideApp (juce::File ("/Applications/plugdata.app")))
            return Found { inside->getFullPathName().toStdString(), "plugdata" };
       #else
        if (const auto plugdata = onPath ("plugdata"))
            return Found { plugdata->getFullPathName().toStdString(), "plugdata" };
       #endif

        //  Go.dot's own Pd.
        if (const auto own = pdIn (fileOf (installFolder())))
            return Found { own->getFullPathName().toStdString(), std::string ("Pd ") + pdRelease };

        //  A Pd installed on the machine.
       #if JUCE_WINDOWS
        if (auto pd = found (programFiles().getChildFile ("Pd").getChildFile ("bin").getChildFile ("pd.exe"), "Pd"))
            return pd;
       #elif JUCE_MAC
        auto apps = juce::File ("/Applications").findChildFiles (juce::File::findDirectories, false, "Pd*.app");
        apps.sort();
        for (int i = apps.size(); --i >= 0;)
            if (const auto inside = insideApp (apps.getReference (i)))
                return Found { inside->getFullPathName().toStdString(), "Pd" };
       #else
        for (const auto* name : { "pd", "puredata" })
            if (const auto pd = onPath (name))
                return Found { pd->getFullPathName().toStdString(), "Pd" };
       #endif

        return std::nullopt;
    }

    bool canDownload()
    {
        return archive().has_value();
    }

    std::string downloadSource()
    {
        const auto from = archive();
        return from.has_value() ? juce::URL (from->url).getDomain().toStdString() : std::string {};
    }

    //==========================================================================
    Installer::~Installer()
    {
        stopping = true;
        if (worker.joinable())
            worker.join();
    }

    bool Installer::start()
    {
        if (! canDownload())
            return false;

        {
            const std::lock_guard<std::mutex> l (lock);
            if (current.state == "downloading" || current.state == "unpacking" || current.state == "checking")
                return true;
            current = { "downloading", 0, {} };
        }

        if (worker.joinable())
            worker.join();
        worker = std::thread ([this] { run(); });
        return true;
    }

    InstallStatus Installer::status() const
    {
        const std::lock_guard<std::mutex> l (lock);
        return current;
    }

    void Installer::set (const char* state, int percent, const std::string& problem)
    {
        const std::lock_guard<std::mutex> l (lock);
        current = { state, percent, problem };
    }

    void Installer::run()
    {
        const auto from = archive();
        const auto destination = fileOf (into.empty() ? installFolder() : into);
        const auto staging = destination.getSiblingFile ("pd-downloading");
        staging.deleteRecursively();

        if (! from.has_value() || ! staging.createDirectory())
        {
            set ("failed", 0, "its folder could not be made");
            return;
        }

        const auto zip = staging.getChildFile ("pd.zip");
        int status = 0;
        auto in = juce::URL (from->url).createInputStream (
            juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs (20000)
                .withNumRedirectsToFollow (10)
                .withStatusCode (&status));

        if (in == nullptr || status >= 400)
        {
            set ("failed", 0, std::string ("the download could not be reached (") + from->url + ")");
            staging.deleteRecursively();
            return;
        }

        {
            const auto total = in->getTotalLength();
            juce::FileOutputStream out (zip);
            if (! out.openedOk())
            {
                set ("failed", 0, "the download could not be saved");
                staging.deleteRecursively();
                return;
            }

            juce::HeapBlock<char> buffer (1 << 16);
            std::int64_t got = 0;
            for (;;)
            {
                if (stopping)
                {
                    staging.deleteRecursively();
                    return;
                }
                const auto read = in->read (buffer, 1 << 16);
                if (read <= 0)
                    break;
                out.write (buffer, static_cast<std::size_t> (read));
                got += read;
                const auto share = total > 0 ? static_cast<double> (got) / static_cast<double> (total) : 0.0;
                set ("downloading", static_cast<int> (share * 100.0));
            }
            out.flush();

            if (got == 0 || (total > 0 && got < total))
            {
                set ("failed", 0, "the download stopped part way");
                staging.deleteRecursively();
                return;
            }
        }

        /*  UNPACKED: by JUCE on Windows; on the Mac by `ditto`, which keeps an
            application's links and its programs runnable. */
        set ("unpacking", 100);
        const auto unpacked = staging.getChildFile ("unpacked");
        unpacked.createDirectory();

       #if JUCE_MAC
        video::PipedChild ditto;
        auto ok = ditto.start ({ "/usr/bin/ditto", "-x", "-k", zip.getFullPathName().toStdString(),
                                 unpacked.getFullPathName().toStdString() });
        if (ok)
        {
            ditto.readAll();
            ok = ditto.wait (600000) == 0;
        }
       #else
        juce::ZipFile archiveFile (zip);
        const auto ok = archiveFile.getNumEntries() > 0 && ! archiveFile.uncompressTo (unpacked, true).failed();
       #endif

        if (! ok)
        {
            set ("failed", 0, "the download would not unpack");
            staging.deleteRecursively();
            return;
        }
        zip.deleteFile();

        set ("checking", 100);
        if (! pdIn (unpacked).has_value())
        {
            set ("failed", 0, "the download held no Pd");
            staging.deleteRecursively();
            return;
        }

        destination.deleteRecursively();
        if (! unpacked.moveFileTo (destination))
        {
            set ("failed", 0, "it could not be put in place");
            staging.deleteRecursively();
            return;
        }
        staging.deleteRecursively();
        set ("done", 100);
    }

    //==========================================================================
    std::string Watch::fileFor (const std::string& cueId) const
    {
        return fileOf (files).getChildFile (juce::String (cueId) + ".pd").getFullPathName().toStdString();
    }

    std::string Watch::open (const std::string& program, const std::string& cueId, const std::string& text)
    {
        const auto file = fileOf (fileFor (cueId));
        file.getParentDirectory().createDirectory();
        if (! file.replaceWithText (juce::String::fromUTF8 (text.c_str()), false, false, "\n"))
            return "the patch could not be written to " + file.getFullPathName().toStdString();

        auto child = std::make_unique<plugin::ChildLaunch>();
        if (! program.empty() && ! child->start ({ program, file.getFullPathName().toStdString() }))
            return "the editor could not be started: " + program;
        child->allowForeground();

        const auto modified = file.getLastModificationTime().toMilliseconds();
        for (auto& one : out)
            if (one.cue == cueId)
            {
                one.file = file.getFullPathName().toStdString();
                one.modified = modified;
                one.text = text;
                one.child = std::move (child);
                return {};
            }

        out.push_back ({ cueId, file.getFullPathName().toStdString(), modified, text, std::move (child) });
        return {};
    }

    std::vector<std::pair<std::string, std::string>> Watch::saved()
    {
        std::vector<std::pair<std::string, std::string>> changed;
        for (auto& one : out)
        {
            const auto file = fileOf (one.file);
            const auto modified = file.getLastModificationTime().toMilliseconds();
            if (modified == one.modified || ! file.existsAsFile())
                continue;

            one.modified = modified;
            auto text = file.loadFileAsString().toStdString();
            if (text.empty() || text == one.text)
                continue;
            one.text = text;
            changed.emplace_back (one.cue, std::move (text));
        }
        return changed;
    }

    //==========================================================================
    void registerCommands (CommandRegistry& registry, const doc::ShowDocument& document,
                           Watch* watch, Installer* installer)
    {
        registry.add ({ "process.edit",
                        "Opens a process cue's patch in plugdata or Pure Data, as a program of its own; each save"
                        " there comes back as the cue's patch (namespace draft 51, ACN). Refused while the show is"
                        " locked, and pd-missing on a machine with neither.",
                        { { "cue", 's', false } },
                        false,
                        [&document, watch] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto cueId = args[0].getString();
                            const auto cue = document.findById (cueId);

                            if (! cue.isValid() || cue.getType().toString() != "Process")
                                return Outcome::rejected (reason::unknownId);

                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            if (watch == nullptr)
                                return Outcome::ok (args);

                            const auto program = find();
                            if (! program.has_value())
                                return Outcome::rejected ("pd-missing");

                            const auto text = document.getAttribute ("/godot/cue/" + cueId + "/patch").value_or (std::string {});
                            if (const auto problem = watch->open (program->program, cueId, text); ! problem.empty())
                                return Outcome::rejected ("pd-missing");

                            return Outcome::ok (args);
                        } });

        registry.add ({ "pd.install",
                        "Downloads Pure Data " + std::string (pdRelease) + " into Go.dot's own folder, to edit a"
                        " process cue's patch in (namespace draft 51, ACO). Taken and ignored where nothing downloads.",
                        {},
                        false,
                        [installer] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (installer != nullptr && ! installer->start())
                                return Outcome::rejected (reason::badValue);
                            return Outcome::ok (args);
                        } });
    }
}
