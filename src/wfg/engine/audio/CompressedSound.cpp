/* This file is part of Go.dot — https://github.com/pob31/go.dot
 *
 * Copyright (C) 2026 Pierre-Olivier Boulant
 *
 * Go.dot is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. Go.dot is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * (LICENSE, at the repository root) for more details.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <wfg/engine/audio/CompressedSound.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <memory>

namespace wfg::audio
{
    namespace
    {
        /*  Every compressed sound one of JUCE's basic formats reads on some
            machine. A movie is not here - its sound is FFmpeg's to take out
            (video/Conversion.cpp) - and neither is anything a cue plays as it
            is: WAV, BWF, AIFF. */
        constexpr const char* compressed[] = { ".mp3", ".mp2", ".m4a", ".aac", ".flac",
                                               ".ogg", ".oga", ".wma", ".caf" };

        //  A block between two looks at `stopping`: a second and a half at 44.1 kHz.
        constexpr int block = 1 << 16;

        std::string tail (const juce::String& said)
        {
            return said.trim().trimCharactersAtEnd (".").trim().toStdString();
        }

        WavWritten failed (std::string why, bool unreadable = false)
        {
            WavWritten written;
            written.unreadable = unreadable;
            written.why = std::move (why);
            return written;
        }
    }

    bool isCompressedSound (const std::string& fileName)
    {
        const auto extension = juce::File::createFileWithoutCheckingPath (juce::String::fromUTF8 (fileName.c_str()))
                                 .getFileExtension().toLowerCase();

        return std::any_of (std::begin (compressed), std::end (compressed),
                            [&extension] (const char* one) { return extension == one; });
    }

    std::string playableNameFor (const std::string& fileName)
    {
        const auto slash = fileName.find_last_of ("/\\");
        const auto name = slash == std::string::npos ? fileName : fileName.substr (slash + 1);

        if (! isCompressedSound (name))
            return name;

        return name.substr (0, name.find_last_of ('.')) + ".wav";
    }

    WavWritten writeAsWav (const juce::File& source, const juce::File& target,
                           const std::function<bool()>& stopping)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        const std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (source) };

        if (reader == nullptr || reader->numChannels == 0 || reader->sampleRate <= 0.0)
            return failed ("nothing on this machine reads it as sound", true);

        const juce::TemporaryFile part (target, juce::TemporaryFile::useHiddenFile);

        {
            auto out = std::make_unique<juce::FileOutputStream> (part.getFile());

            if (out->failedToOpen())
                return failed (tail (out->getStatus().getErrorMessage()));

            std::unique_ptr<juce::OutputStream> stream { std::move (out) };
            juce::WavAudioFormat wav;
            auto writer = wav.createWriterFor (stream,
                                               juce::AudioFormatWriterOptions {}
                                                 .withSampleRate (reader->sampleRate)
                                                 .withNumChannels (static_cast<int> (reader->numChannels))
                                                 .withBitsPerSample (32)
                                                 .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

            if (writer == nullptr)
                return failed ("a WAV of it could not be started");

            for (juce::int64 at = 0; at < reader->lengthInSamples; at += block)
            {
                if (stopping && stopping())
                    return failed ("the window was closed before it finished");

                const auto count = static_cast<int> (std::min<juce::int64> (block, reader->lengthInSamples - at));

                if (! writer->writeFromAudioReader (*reader, at, count))
                    return failed ("writing it stopped part of the way");
            }

            if (! writer->flush())
                return failed ("writing it stopped part of the way");
        }

        /*  ON WINDOWS A FILE A CUE HAS OPEN CANNOT BE WRITTEN OVER, which is
            the likeliest reason a replace does not land. */
        const auto replacing = target.existsAsFile();

        if (! part.overwriteTargetFileWithTemporary())
            return failed (replacing ? "the one in the show could not be replaced - it may be in use"
                                     : "it could not be put in place");

        WavWritten written;
        written.ok = true;
        return written;
    }
}
