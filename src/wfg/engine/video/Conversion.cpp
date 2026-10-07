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

#include <wfg/engine/video/Conversion.h>

#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/video/Ffmpeg.h>
#include <wfg/engine/video/HapEncoder.h>
#include <wfg/engine/video/MovieWriter.h>
#include <wfg/engine/video/PipedChild.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace wfg::video
{
    namespace
    {
        /*  WHAT FFMPEG SAID WENT WRONG, its last line, from the file its
            errors went to. */
        std::string lastLineOf (const juce::File& file)
        {
            auto lines = juce::StringArray::fromLines (file.loadFileAsString().trim());
            lines.removeEmptyStrings();
            return lines.isEmpty() ? std::string {} : lines[lines.size() - 1].toStdString();
        }

        /*  THE RATE AS QUICKTIME KEEPS IT: a time scale and each frame's
            share of it, at least 600 units a second so a rate of 25 is not
            written in whole seconds. */
        void timeBaseOf (const ffmpeg::Probe& probe, std::uint32_t& scale, std::uint32_t& duration)
        {
            auto over = probe.rateOver > 0 ? probe.rateOver : 25;
            auto under = probe.rateOver > 0 ? probe.rateUnder : 1;

            while (over < 600 && over < 0x7fffffff / 10)
            {
                over *= 10;
                under *= 10;
            }

            scale = static_cast<std::uint32_t> (over);
            duration = static_cast<std::uint32_t> (std::max (1, under));
        }
    }

    bool convertMovie (const ConversionRequest& request, std::string& why,
                       const std::function<void (double)>& progress, const std::atomic<bool>* cancelled)
    {
        const auto tools = ffmpeg::find();

        if (! tools.found())
        {
            why = "FFmpeg not found";
            return false;
        }

        const auto probe = ffmpeg::probe (tools, request.source);

        if (! probe.ok)
        {
            why = probe.why;
            return false;
        }

        const auto texture = request.quality ? hap::Texture::ycocgDxt5
                           : probe.alpha     ? hap::Texture::dxt5
                                             : hap::Texture::dxt1;
        const auto* codec = texture == hap::Texture::ycocgDxt5 ? "HapY" : texture == hap::Texture::dxt5 ? "Hap5" : "Hap1";

        std::uint32_t timeScale = 0, frameDuration = 0;
        timeBaseOf (probe, timeScale, frameDuration);

        const auto start = std::max (0.0, request.start);
        const auto rest = std::max (0.0, probe.duration - start);
        const auto length = request.length >= 0.0 ? std::min (request.length, rest) : rest;
        const auto expected = std::max (1.0, length * static_cast<double> (timeScale) / static_cast<double> (frameDuration));

        const juce::File target (juce::String::fromUTF8 (request.target.c_str()));
        const auto part = target.getSiblingFile (target.getFileName() + ".part");
        const auto errors = target.getSiblingFile (target.getFileName() + ".log");
        target.getParentDirectory().createDirectory();

        /*  THE FRAMES, raw, at the source's own rate made steady. */
        std::vector<std::string> command { tools.ffmpeg, "-nostdin", "-v", "error",
                                           "-ss", osc::formatDouble (start), "-i", request.source };

        if (request.length >= 0.0)
        {
            command.push_back ("-t");
            command.push_back (osc::formatDouble (length));
        }

        for (const auto* word : { "-map", "0:v:0", "-an", "-sn", "-fps_mode", "cfr" })
            command.push_back (word);

        command.push_back ("-r");
        command.push_back (std::to_string (timeScale) + "/" + std::to_string (frameDuration));

        for (const auto* word : { "-f", "rawvideo", "-pix_fmt", "rgba", "-" })
            command.push_back (word);

        PipedChild decoder;

        if (! decoder.start (command, errors.getFullPathName().toStdString()))
        {
            why = "FFmpeg would not start";
            return false;
        }

        movie::MovieWriter writer;

        if (! writer.open (part.getFullPathName().toStdString(), codec, probe.width, probe.height,
                           timeScale, frameDuration, why))
        {
            decoder.kill();
            return false;
        }

        const auto width = probe.width, height = probe.height;
        const auto frameBytes = static_cast<std::size_t> (width) * static_cast<std::size_t> (height) * 4;
        const auto rowsOfBlocks = (height + 3) / 4;
        const auto threads = std::clamp (static_cast<int> (std::thread::hardware_concurrency()) - 2, 1, 16);

        std::vector<std::uint8_t> pixels (frameBytes), blocks, frame, scratch;
        auto stopped = false;
        auto count = 0;

        while (decoder.readExactly (pixels.data(), pixels.size()))
        {
            if (cancelled != nullptr && cancelled->load())
            {
                stopped = true;
                break;
            }

            /*  ITS BLOCKS ON EVERY THREAD SPARED, a band of rows each. */
            blocks.resize (static_cast<std::size_t> (((width + 3) / 4) * rowsOfBlocks) * hap::bytesPerBlock (texture));
            const auto band = (rowsOfBlocks + threads - 1) / threads;
            std::vector<std::thread> workers;

            for (int t = 1; t < threads; ++t)
                workers.emplace_back ([&, t]
                                      {
                                          hap::encodeTexture (texture, pixels.data(), width, height,
                                                              static_cast<std::size_t> (width) * 4, blocks, t * band, band);
                                      });

            hap::encodeTexture (texture, pixels.data(), width, height, static_cast<std::size_t> (width) * 4, blocks, 0, band);

            for (auto& worker : workers)
                worker.join();

            hap::packFrame (texture, blocks, frame, scratch);

            if (! writer.write (frame.data(), frame.size()))
            {
                why = "the HAP file could not be written";
                stopped = true;
                break;
            }

            ++count;

            if (progress)
                progress (std::min (0.99, static_cast<double> (count) / expected));
        }

        if (stopped)
            decoder.kill();

        const auto exit = decoder.wait (30000);

        const auto fail = [&] (std::string reason)
        {
            std::string ignored;
            writer.finish (ignored);
            part.deleteFile();
            why = std::move (reason);
            return false;
        };

        if (stopped)
            return fail (why.empty() ? std::string ("cancelled") : why);

        if (exit != 0 || count == 0)
        {
            const auto said = lastLineOf (errors);
            return fail (said.empty() ? std::string ("FFmpeg could not decode it") : said);
        }

        if (! writer.finish (why))
        {
            part.deleteFile();
            return false;
        }

        /*  ITS SOUND, over the same span, when asked and when it has one. */
        if (! request.sound.empty() && probe.sound)
        {
            const juce::File sound (juce::String::fromUTF8 (request.sound.c_str()));
            const auto soundPart = sound.getSiblingFile (sound.getFileName() + ".part");

            std::vector<std::string> take { tools.ffmpeg, "-nostdin", "-v", "error", "-y",
                                            "-ss", osc::formatDouble (start), "-i", request.source };

            if (request.length >= 0.0)
            {
                take.push_back ("-t");
                take.push_back (osc::formatDouble (length));
            }

            for (const auto* word : { "-map", "0:a:0", "-vn", "-c:a", "pcm_s24le", "-f", "wav" })
                take.push_back (word);

            take.push_back (soundPart.getFullPathName().toStdString());

            PipedChild taker;

            if (! taker.start (take, errors.getFullPathName().toStdString()))
                return fail ("FFmpeg would not start for the sound");

            taker.readAll();

            if (taker.wait (600000) != 0)
            {
                soundPart.deleteFile();
                const auto said = lastLineOf (errors);
                return fail (said.empty() ? std::string ("its sound could not be taken out") : said);
            }

            sound.deleteFile();

            if (! soundPart.moveFileTo (sound))
                return fail ("its sound could not be put in place");
        }

        target.deleteFile();

        if (! part.moveFileTo (target))
        {
            part.deleteFile();
            why = "the HAP file could not be put in place";
            return false;
        }

        errors.deleteFile();

        if (progress)
            progress (1.0);

        return true;
    }

    double usedStartOf (const doc::ShowDocument& document, const std::string& sourceName)
    {
        auto earliest = -1.0;

        std::function<void (const juce::ValueTree&)> visit;
        visit = [&] (const juce::ValueTree& node)
        {
            if (node.hasType ("Video") && node["file"].toString().toStdString() == sourceName)
            {
                const auto offset = osc::parseDouble (node["startOffset"].toString().toStdString()).value_or (0.0);
                const auto loops = node.hasProperty ("loops") ? osc::parseDouble (node["loops"].toString().toStdString()).value_or (1.0)
                                                              : 1.0;
                const auto from = std::lround (loops) == 1 ? std::max (0.0, offset) : 0.0;
                earliest = earliest < 0.0 ? from : std::min (earliest, from);
            }

            for (const auto& child : node)
                visit (child);
        };

        visit (document.root());
        return earliest < 0.0 ? 0.0 : std::max (0.0, earliest - 10.0);
    }

    //==============================================================================
    Converter::Converter (Finished whenFinished) : finished (std::move (whenFinished))
    {
        tools = ffmpeg::find().ffmpeg;
        thread = std::thread ([this] { run(); });
    }

    std::string Converter::ffmpegPath() const
    {
        const std::lock_guard<std::mutex> held (lock);
        return tools;
    }

    Converter::~Converter()
    {
        stop();
    }

    void Converter::setMediaFolder (std::string mediaFolder)
    {
        const std::lock_guard<std::mutex> held (lock);
        folder = std::move (mediaFolder);
    }

    std::string Converter::mediaFolder() const
    {
        const std::lock_guard<std::mutex> held (lock);
        return folder;
    }

    void Converter::enqueue (ConversionRequest request)
    {
        const auto found = ffmpeg::find().ffmpeg;

        {
            const std::lock_guard<std::mutex> held (lock);
            tools = found;

            std::erase_if (known, [&request] (const ConversionStatus& status)
                           {
                               return status.sourceName == request.sourceName;
                           });

            known.push_back ({ request.id, request.sourceName, "waiting", 0.0, {} });
            queue.push_back (std::move (request));
        }

        wake.notify_one();
    }

    void Converter::cancel (const std::string& sourceName)
    {
        const std::lock_guard<std::mutex> held (lock);

        std::erase_if (queue, [&sourceName] (const ConversionRequest& request) { return request.sourceName == sourceName; });

        for (auto& status : known)
            if (status.sourceName == sourceName && status.state == "waiting")
                status.state = "cancelled";

        if (current == sourceName)
            cancelCurrent = true;
    }

    std::vector<ConversionStatus> Converter::statuses() const
    {
        const std::lock_guard<std::mutex> held (lock);
        return known;
    }

    void Converter::stop()
    {
        {
            const std::lock_guard<std::mutex> held (lock);

            if (stopping && ! thread.joinable())
                return;

            stopping = true;
            cancelCurrent = true;
        }

        wake.notify_all();

        if (thread.joinable())
            thread.join();
    }

    void Converter::run()
    {
        for (;;)
        {
            ConversionRequest request;

            {
                std::unique_lock<std::mutex> held (lock);
                wake.wait (held, [this] { return stopping || ! queue.empty(); });

                if (stopping)
                    return;

                request = std::move (queue.front());
                queue.pop_front();
                current = request.sourceName;
                cancelCurrent = false;
            }

            const auto update = [this, &request] (const char* state, double progress, const std::string& problem)
            {
                ConversionStatus copy;

                {
                    const std::lock_guard<std::mutex> held (lock);

                    for (auto& status : known)
                        if (status.id == request.id)
                        {
                            status.state = state;
                            status.progress = progress;
                            status.problem = problem;
                            copy = status;
                        }
                }

                return copy;
            };

            update ("converting", 0.0, {});

            std::string why;
            const auto ok = convertMovie (request, why, [&update] (double done) { update ("converting", done, {}); },
                                          &cancelCurrent);

            const auto cancelledNow = cancelCurrent.load();
            const auto status = update (ok ? "done" : cancelledNow ? "cancelled" : "failed", ok ? 1.0 : 0.0,
                                        ok || cancelledNow ? std::string {} : why);

            {
                const std::lock_guard<std::mutex> held (lock);
                current.clear();
            }

            if (finished)
                finished (request, status);
        }
    }

    //==============================================================================
    void registerConversionCommands (CommandRegistry& registry, doc::ShowDocument& document, Converter* converter)
    {
        registry.add ({ "media.convert",
                        "Converts a movie to HAP in the background (namespace draft 37.5, WF-WJ): the whole"
                        " file, or the part the cues use with ten seconds either side; Hap, or Hap Q; and its"
                        " sound taken out beside it. When it is done, the cues naming the movie name the HAP"
                        " file (media.converted). Taken and ignored where nothing converts.",
                        { { "file", 's', false }, { "scope", 's', true }, { "format", 's', true },
                          { "sound", 'T', true } },
                        false,
                        [&document, converter] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto file = args[0].getString();
                            const auto scope = args.size() > 1 ? args[1].getString() : std::string ("whole");
                            const auto format = args.size() > 2 ? args[2].getString() : std::string ("hap");
                            const auto sound = args.size() > 3 && args[3].isBool() && args[3].getBool();

                            if (file.empty() || (scope != "whole" && scope != "used") || (format != "hap" && format != "hapq"))
                                return Outcome::rejected (reason::badValue);

                            if (converter == nullptr)
                                return Outcome::ok (args);

                            const auto folder = converter->mediaFolder();
                            const auto source = audio::resolveMediaPath (folder, file);

                            if (folder.empty() || ! juce::File (juce::String::fromUTF8 (source.c_str())).existsAsFile())
                                return Outcome::rejected (reason::unknownId);

                            /*  BESIDE THE SOURCE, under a name of its own: a
                                movie that is a .mov already is not overwritten. */
                            const auto stem = juce::String::fromUTF8 (file.c_str()).upToLastOccurrenceOf (".", false, false);
                            const auto root = juce::File (juce::String::fromUTF8 (audio::mediaRootOf (folder, file).c_str()));

                            const auto freeName = [&root] (const juce::String& wanted, const juce::String& extension)
                            {
                                auto name = wanted + extension;

                                for (int n = 2; root.getChildFile (name).exists() && n < 1000; ++n)
                                    name = wanted + " " + juce::String (n) + extension;

                                return name;
                            };

                            ConversionRequest request;
                            request.id = juce::Uuid().toDashedString().toStdString();
                            request.sourceName = file;
                            request.source = source;
                            request.targetName = freeName (stem + (format == "hapq" ? " (Hap Q)" : " (Hap)"), ".mov").toStdString();
                            request.target = root.getChildFile (juce::String::fromUTF8 (request.targetName.c_str()))
                                                 .getFullPathName().toStdString();
                            request.start = scope == "used" ? usedStartOf (document, file) : 0.0;
                            request.quality = format == "hapq";

                            if (sound)
                            {
                                request.soundName = freeName (stem + " (sound)", ".wav").toStdString();
                                request.sound = root.getChildFile (juce::String::fromUTF8 (request.soundName.c_str()))
                                                    .getFullPathName().toStdString();
                            }

                            converter->enqueue (request);
                            return Outcome::ok (args);
                        } });

        registry.add ({ "media.convert.cancel",
                        "Stops the conversion of a movie, or takes it off the queue. The source stays as it was.",
                        { { "file", 's', false } },
                        false,
                        [converter] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (converter != nullptr)
                                converter->cancel (args[0].getString());

                            return Outcome::ok (args);
                        } });

        registry.add ({ "media.converted",
                        "A conversion is done (namespace draft 37.5, WH): every video cue naming the movie names"
                        " its HAP file instead, its start offset moved back by what was cut from the front."
                        " Submitted by the converter, one undoable edit; the original stays in media/.",
                        { { "source", 's', false }, { "target", 's', false }, { "cut", 'd', false },
                          { "sound", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto source = args[0].getString();
                            const auto target = args[1].getString();
                            const auto cut = args[2].asDouble();

                            if (source.empty() || target.empty() || ! std::isfinite (cut) || cut < 0.0)
                                return Outcome::rejected (reason::badValue);

                            std::vector<std::string> cues;
                            std::function<void (const juce::ValueTree&)> visit;
                            visit = [&] (const juce::ValueTree& node)
                            {
                                if (node.hasType ("Video") && node["file"].toString().toStdString() == source)
                                    cues.push_back (node["id"].toString().toStdString());

                                for (const auto& child : node)
                                    visit (child);
                            };

                            visit (document.root());

                            for (const auto& id : cues)
                            {
                                const auto base = "/godot/cue/" + id + "/";
                                const auto node = document.findById (id);
                                const auto offset = osc::parseDouble (node["startOffset"].toString().toStdString()).value_or (0.0);

                                if (const auto edit = document.setAttribute (base + "file", target); ! edit.ok)
                                    return Outcome::rejected (edit.reason);

                                if (cut > 0.0)
                                    if (const auto edit = document.setAttribute (base + "startOffset",
                                                                                 osc::formatDouble (std::max (0.0, offset - cut)));
                                        ! edit.ok)
                                        return Outcome::rejected (edit.reason);
                            }

                            return Outcome::ok (args);
                        } });
    }
}
