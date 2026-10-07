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

#include <wfg/engine/video/Ffmpeg.h>
#include <wfg/engine/video/PipedChild.h>

#include <wfg/engine/osc/OscValue.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <thread>

namespace wfg::video::ffmpeg
{
    namespace
    {
       #if JUCE_WINDOWS
        constexpr const char* ffmpegName = "ffmpeg.exe";
        constexpr const char* ffprobeName = "ffprobe.exe";
       #else
        constexpr const char* ffmpegName = "ffmpeg";
        constexpr const char* ffprobeName = "ffprobe";
       #endif

        /*  BOTH IN ONE FOLDER, or neither: an ffprobe from one build and an
            ffmpeg from another is a question answered by one program and
            acted on by the other. */
        Tools inFolder (const juce::File& folder)
        {
            const auto ffmpeg = folder.getChildFile (ffmpegName);
            const auto ffprobe = folder.getChildFile (ffprobeName);

            if (ffmpeg.existsAsFile() && ffprobe.existsAsFile())
                return { ffmpeg.getFullPathName().toStdString(), ffprobe.getFullPathName().toStdString() };

            return {};
        }

        /*  A JSON NUMBER OR A NUMBER IN A STRING, as ffprobe writes both. The
            locale rule: never the C library's reading of a decimal point. */
        double numberOf (const juce::var& value)
        {
            if (value.isDouble() || value.isInt() || value.isInt64())
                return static_cast<double> (value);

            if (value.isString())
                if (const auto parsed = osc::parseDouble (value.toString().toStdString()); parsed.has_value())
                    return *parsed;

            return 0.0;
        }

        /*  "30000/1001", "25/1", "0/0". */
        double rateOf (const juce::var& value)
        {
            const auto text = value.toString();

            if (! text.contains ("/"))
                return numberOf (value);

            const auto over = numberOf (text.upToFirstOccurrenceOf ("/", false, false));
            const auto under = numberOf (text.fromFirstOccurrenceOf ("/", false, false));
            return under > 0.0 ? over / under : 0.0;
        }
    }

    Tools find()
    {
        /*  WHERE IT IS SAID TO BE. */
        if (const auto said = juce::SystemStats::getEnvironmentVariable ("WFG_FFMPEG", {}); said.isNotEmpty())
        {
            const juce::File place (said);

            if (const auto tools = inFolder (place.isDirectory() ? place : place.getParentDirectory()); tools.found())
                return tools;
        }

        /*  BESIDE GO.DOT, where a package puts it. */
        const auto beside = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

        for (const auto& folder : { beside, beside.getChildFile ("ffmpeg"),
                                    beside.getParentDirectory().getChildFile ("Resources").getChildFile ("ffmpeg") })
            if (const auto tools = inFolder (folder); tools.found())
                return tools;

        /*  ON THE PATH. */
        const auto separator = juce::File::getSeparatorChar() == '\\' ? ";" : ":";

        for (const auto& entry : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}),
                                                                separator, "\""))
            if (entry.isNotEmpty() && juce::File::isAbsolutePath (entry))
                if (const auto tools = inFolder (juce::File (entry)); tools.found())
                    return tools;

        return {};
    }

    bool pixelFormatHasAlpha (const std::string& format)
    {
        const juce::String name (format);

        return name.startsWith ("yuva") || name.startsWith ("gbrap") || name.startsWith ("ya")
            || name.startsWith ("ayuv") || name.contains ("rgba") || name.contains ("bgra")
            || name.contains ("argb") || name.contains ("abgr");
    }

    Probe parseProbe (const std::string& json)
    {
        Probe out;
        const auto parsed = juce::JSON::parse (juce::String::fromUTF8 (json.c_str()));

        if (! parsed.isObject())
        {
            out.why = "ffprobe did not answer";
            return out;
        }

        bool video = false;

        if (const auto* streams = parsed["streams"].getArray())
            for (const auto& stream : *streams)
            {
                const auto type = stream["codec_type"].toString();

                /*  A COVER PICTURE is a video stream of one frame, and not the
                    movie - an MP3's or an M4A's. */
                const auto disposition = stream["disposition"];
                const auto attached = disposition.isObject() && static_cast<int> (numberOf (disposition["attached_pic"])) != 0;

                if (type == "video" && ! video && ! attached)
                {
                    video = true;
                    out.codec = stream["codec_name"].toString().toStdString();
                    out.width = static_cast<int> (numberOf (stream["width"]));
                    out.height = static_cast<int> (numberOf (stream["height"]));
                    auto written = stream["avg_frame_rate"];
                    out.frameRate = rateOf (written);

                    if (! (out.frameRate > 0.0))
                    {
                        written = stream["r_frame_rate"];
                        out.frameRate = rateOf (written);
                    }

                    /*  THE FRACTION ITSELF, for a movie written at exactly that
                        rate: 30000/1001 kept as two whole numbers. */
                    const auto text = written.toString();

                    if (out.frameRate > 0.0 && text.contains ("/"))
                    {
                        out.rateOver = static_cast<int> (numberOf (text.upToFirstOccurrenceOf ("/", false, false)));
                        out.rateUnder = std::max (1, static_cast<int> (numberOf (text.fromFirstOccurrenceOf ("/", false, false))));
                    }
                    else if (out.frameRate > 0.0)
                    {
                        out.rateOver = static_cast<int> (std::lround (out.frameRate * 1000.0));
                        out.rateUnder = 1000;
                    }

                    out.duration = numberOf (stream["duration"]);
                    out.alpha = pixelFormatHasAlpha (stream["pix_fmt"].toString().toStdString());

                    /*  HAP SAYS WHICH IN ITS TAG: Hap5 and HapA carry alpha. */
                    const auto tag = stream["codec_tag_string"].toString();

                    if (tag == "Hap5" || tag == "HapA" || tag == "HapM")
                        out.alpha = true;
                }
                else if (type == "audio" && ! out.sound)
                {
                    out.sound = true;
                    out.soundChannels = static_cast<int> (numberOf (stream["channels"]));
                    out.soundRate = static_cast<int> (numberOf (stream["sample_rate"]));
                }
            }

        if (! (out.duration > 0.0))
            out.duration = numberOf (parsed["format"]["duration"]);

        if (! video)
        {
            out.why = "no picture in it";
            return out;
        }

        if (out.width <= 0 || out.height <= 0)
        {
            out.why = "a picture of no size";
            return out;
        }

        out.ok = true;
        return out;
    }

    Probe probe (const Tools& tools, const std::string& path, int milliseconds)
    {
        if (! tools.found())
        {
            Probe out;
            out.why = "FFmpeg not found";
            return out;
        }

        PipedChild child;

        if (! child.start ({ tools.ffprobe, "-v", "error", "-print_format", "json",
                             "-show_format", "-show_streams", path }))
        {
            Probe out;
            out.why = "ffprobe would not start";
            return out;
        }

        /*  READ ON A THREAD OF ITS OWN, so a file ffprobe hangs on cannot
            hold the caller past its time: the child is ended, and the read
            returns. */
        std::string answer;
        juce::WaitableEvent done;

        std::thread reader ([&child, &answer, &done]
                            {
                                answer = child.readAll (4u << 20);
                                done.signal();
                            });

        if (! done.wait (milliseconds))
            child.kill();

        reader.join();
        child.wait (2000);

        auto out = parseProbe (answer);

        if (! out.ok && answer.empty())
            out.why = "ffprobe could not read it";

        return out;
    }
}
