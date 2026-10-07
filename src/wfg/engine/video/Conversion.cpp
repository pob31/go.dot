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

    bool convertMovie (ConversionRequest& request, std::string& why,
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

        const auto pictures = ! request.target.empty();
        const juce::File target (juce::String::fromUTF8 ((pictures ? request.target : request.sound).c_str()));
        const auto part = target.getSiblingFile (target.getFileName() + ".part");
        const auto errors = target.getSiblingFile (target.getFileName() + ".log");
        target.getParentDirectory().createDirectory();

        std::string ignored;
        movie::MovieWriter writer;

        const auto fail = [&] (std::string reason)
        {
            if (pictures)
            {
                writer.finish (ignored);
                part.deleteFile();
            }

            errors.deleteFile();
            why = std::move (reason);
            return false;
        };

        if (pictures)
        {
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
        }

        /*  ITS SOUND, over the same span, when asked and when it has one. */
        if (! pictures && ! probe.sound)
            return fail ("it has no sound");

        if (! request.sound.empty() && probe.sound)
        {
            request.soundChannels = probe.soundChannels;

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
        else
        {
            request.soundName.clear();
        }

        if (pictures)
        {
            target.deleteFile();

            if (! part.moveFileTo (target))
            {
                part.deleteFile();
                why = "the HAP file could not be put in place";
                return false;
            }
        }

        errors.deleteFile();

        if (progress)
            progress (1.0);

        return true;
    }

    namespace
    {
        double secondsOf (const juce::ValueTree& node, const char* name, double otherwise)
        {
            return node.hasProperty (name) ? osc::parseDouble (node[name].toString().toStdString()).value_or (otherwise)
                                           : otherwise;
        }
    }

    UsedSpan usedSpanOf (const doc::ShowDocument& document, const std::string& sourceName)
    {
        auto earliest = -1.0, furthest = -1.0;
        auto toTheEnd = false;

        std::function<void (const juce::ValueTree&)> visit;
        visit = [&] (const juce::ValueTree& node)
        {
            if (node.hasType ("Video") && node["file"].toString().toStdString() == sourceName)
            {
                auto from = -1.0;

                for (const auto& child : node)
                    if (child.hasType ("Range"))
                    {
                        const auto in = std::max (0.0, secondsOf (child, "in", 0.0));
                        from = from < 0.0 ? in : std::min (from, in);
                        furthest = std::max (furthest, secondsOf (child, "out", 0.0));
                    }

                /*  NO RANGE: from the start offset, to the end of the file. */
                if (from < 0.0)
                {
                    from = std::max (0.0, secondsOf (node, "startOffset", 0.0));
                    toTheEnd = true;
                }

                earliest = earliest < 0.0 ? from : std::min (earliest, from);
            }

            for (const auto& child : node)
                visit (child);
        };

        visit (document.root());

        UsedSpan span;
        span.start = earliest < 0.0 ? 0.0 : std::max (0.0, earliest - 10.0);
        span.end = toTheEnd || furthest < 0.0 ? -1.0 : furthest + 10.0;
        return span;
    }

    //==============================================================================
    Converter::Converter (Finished whenFinished) : finished (std::move (whenFinished))
    {
        tools = ffmpeg::find().ffmpeg;
        thread = std::thread ([this] { run(); });
    }

    std::string Converter::ffmpegPath() const
    {
        /*  LOOKED FOR AGAIN while there is none, at most once a second: one
            downloaded on first use (37.5, WN), or installed by hand, is found
            without Go.dot being started again. */
        {
            const std::lock_guard<std::mutex> held (lock);

            if (! tools.empty())
                return tools;

            const auto now = juce::Time::getMillisecondCounter();

            if (now - lookedAt < 1000)
                return tools;

            lookedAt = now;
        }

        const auto found = ffmpeg::find().ffmpeg;
        const std::lock_guard<std::mutex> held (lock);
        tools = found;
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

                            if (file.empty() || (scope != "whole" && scope != "used")
                                  || (format != "hap" && format != "hapq" && format != "none")
                                  || (format == "none" && ! sound))
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
                            /*  THE SOUND ALONE takes the whole file: the movie
                                is not cut, so neither is what goes with it. */
                            if (format != "none")
                            {
                                request.targetName = freeName (stem + (format == "hapq" ? " (Hap Q)" : " (Hap)"), ".mov").toStdString();
                                request.target = root.getChildFile (juce::String::fromUTF8 (request.targetName.c_str()))
                                                     .getFullPathName().toStdString();
                            }

                            if (scope == "used" && format != "none")
                            {
                                const auto span = usedSpanOf (document, file);
                                request.start = span.start;
                                request.length = span.end >= 0.0 ? span.end - span.start : -1.0;
                            }

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
                        " its HAP file instead, its start offset and its Ranges' in and out points moved back by"
                        " what was cut from the front. Submitted by the converter, one undoable edit; the original"
                        " stays in media/.",
                        { { "source", 's', false }, { "target", 's', false }, { "cut", 'd', false },
                          { "sound", 's', true }, { "channels", 'i', true }, { "made", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto source = args[0].getString();
                            const auto target = args[1].getString();
                            const auto cut = args[2].asDouble();
                            const auto soundFile = args.size() > 3 && args[3].isString() ? args[3].getString() : std::string {};
                            const auto channels = args.size() > 4 && args[4].isNumber()
                                                    ? std::clamp (static_cast<int> (args[4].asDouble()), 1, 64) : 2;
                            const auto supplied = args.size() > 5 && args[5].isString()
                                                    ? juce::StringArray::fromTokens (juce::String (args[5].getString()), " ", "")
                                                    : juce::StringArray();

                            if (source.empty() || ! std::isfinite (cut) || cut < 0.0 || (target.empty() && soundFile.empty()))
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

                                /*  THE SOUND ALONE: the movie stays as it is. */
                                if (target.empty())
                                    continue;

                                if (const auto edit = document.setAttribute (base + "file", target); ! edit.ok)
                                    return Outcome::rejected (edit.reason);

                                if (! (cut > 0.0))
                                    continue;

                                if (const auto edit = document.setAttribute (base + "startOffset",
                                                                             osc::formatDouble (std::max (0.0, offset - cut)));
                                    ! edit.ok)
                                    return Outcome::rejected (edit.reason);

                                /*  ITS RANGES, by the same: the in point first,
                                    since moving back it only draws away from
                                    the out, so a range is never for a moment
                                    one that ends before it begins. */
                                for (const auto& child : node)
                                {
                                    if (! child.hasType ("Range"))
                                        continue;

                                    const auto rangeBase = "/godot/range/" + child["id"].toString().toStdString() + "/";
                                    const auto in = secondsOf (child, "in", 0.0);
                                    const auto out = secondsOf (child, "out", 0.0);

                                    for (const auto& [row, value] : { std::pair<const char*, double> { "in", in },
                                                                      std::pair<const char*, double> { "out", out } })
                                        if (const auto edit = document.setAttribute (rangeBase + row,
                                                                                     osc::formatDouble (std::max (0.0, value - cut)));
                                            ! edit.ok)
                                            return Outcome::rejected (edit.reason);
                                }
                            }

                            /*  ITS SOUND AS A CUE LOCKED TO IT (WJ): one after
                                each movie, or the one it has pointed at the new
                                file. Made after the movie's edit, so it takes
                                the movie's start offset and Ranges as they now
                                are. */
                            std::vector<std::string> made;
                            int taken = 0;

                            if (! soundFile.empty())
                                for (const auto& movieId : cues)
                                {
                                    const auto movie = document.findById (movieId);
                                    juce::ValueTree existing;

                                    std::function<void (const juce::ValueTree&)> find = [&] (const juce::ValueTree& node)
                                    {
                                        if (node.hasType ("Media") && node["lockedTo"].toString().toStdString() == movieId)
                                            existing = node;

                                        for (const auto& child : node)
                                            find (child);
                                    };

                                    find (document.root());

                                    if (existing.isValid())
                                    {
                                        const auto existingId = existing["id"].toString().toStdString();

                                        if (const auto edit = document.setAttribute ("/godot/cue/" + existingId + "/file", soundFile); ! edit.ok)
                                            return Outcome::rejected (edit.reason);

                                        continue;
                                    }

                                    /*  RIGHT AFTER THE MOVIE, among its parent's members. */
                                    const auto parent = movie.getParent();
                                    int position = 0;

                                    for (const auto& sibling : parent)
                                    {
                                        if (sibling == movie)
                                            break;

                                        if (doc::ShowDocument::ownerForElement (sibling.getType().toString().toStdString()) == "cue")
                                            ++position;
                                    }

                                    const auto wanted = taken < supplied.size() ? supplied[taken].toStdString() : std::string {};
                                    ++taken;

                                    const auto name = movie["name"].toString().toStdString() + " (sound)";
                                    const auto created = document.createCue (parent["id"].toString().toStdString(), position + 1,
                                                                             "media", name, wanted);

                                    if (! created.ok)
                                        return Outcome::rejected (created.reason);

                                    made.push_back (created.id);

                                    const auto cueBase = "/godot/cue/" + created.id + "/";

                                    for (const auto& [row, value] : { std::pair<std::string, std::string> { "file", soundFile },
                                                                      std::pair<std::string, std::string> { "channels", std::to_string (channels) },
                                                                      std::pair<std::string, std::string> { "lockedTo", movieId } })
                                        if (const auto edit = document.setAttribute (cueBase + row, value); ! edit.ok)
                                            return Outcome::rejected (edit.reason);

                                    /*  ROUTED AS AN IMPORTED SOUND IS, to the first
                                        bus; a show with none leaves it to be. */
                                    document.defaultMediaRoute (created.id, channels);
                                }

                            auto applied = args;

                            while (applied.size() < 5)
                                applied.push_back (applied.size() == 3 ? osc::Value::string (soundFile)
                                                                       : osc::Value::int32 (channels));

                            std::string madeText;

                            for (const auto& id : made)
                                madeText += (madeText.empty() ? "" : " ") + id;

                            if (applied.size() > 5)
                                applied[5] = osc::Value::string (madeText);
                            else
                                applied.push_back (osc::Value::string (madeText));

                            return Outcome::ok (applied);
                        } });
    }
}
