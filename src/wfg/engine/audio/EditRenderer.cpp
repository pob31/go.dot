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

#include "EditRenderer.h"

#include <wfg/engine/video/Movie.h>
#include <wfg/engine/video/MovieEditRender.h>

#include <wfg/engine/cue/PlayedMedia.h>
#include <wfg/engine/osc/OscValue.h>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace wfg::audio
{
    namespace
    {
        constexpr int blockFrames = 4096;
        constexpr const char* editsFolder = ".edits";
        constexpr double halfPi = 1.5707963267948966;

        bool stopRequested (const std::atomic<bool>* stop) noexcept
        {
            return stop != nullptr && stop->load (std::memory_order_relaxed);
        }

        /*  A JOB'S KIND, by its source's name (namespace draft §55.5): a
            movie renders to a movie, a sound to a WAV. */
        bool isMovieJob (const std::string& sourceName) noexcept
        {
            return video::movie::isMovieName (sourceName);
        }

        const char* kindOf (const std::string& sourceName) noexcept
        {
            return isMovieJob (sourceName) ? "movie" : "sound";
        }

        const char* renderExtensionOf (const std::string& sourceName) noexcept
        {
            return isMovieJob (sourceName) ? ".mov" : ".wav";
        }

        float gainOf (double dB) noexcept
        {
            return juce::Decibels::decibelsToGain (static_cast<float> (dB), -120.0f);
        }

        /*  A section's geometry in frames: where it sits on the edited
            timeline, where its material is in the file, and the half-fades
            either side of it. */
        struct Piece
        {
            std::int64_t start = 0;        // edited frame the section begins at
            std::int64_t end = 0;          // edited frame it ends at
            std::int64_t in = 0;           // the file frame its material begins at
            double trimDb = 0.0;
            float trim = 1.0f;
            std::int64_t fadeIn = 0;       // the crossfade INTO this section, in frames; 0 for none
            std::int64_t fadeOut = 0;      // the crossfade into the next section, in frames
            bool rampIn = false;           // a continuous join with a trim to ramp from
            bool rampOut = false;
            std::int64_t rampInFrames = 0;
            std::int64_t rampOutFrames = 0;
            double trimBeforeDb = 0.0;
            double trimAfterDb = 0.0;
        };

        std::vector<Piece> piecesOf (const std::vector<doc::Section>& given, double rate)
        {
            const auto sections = doc::clampCrossfades (given);
            std::vector<Piece> pieces (sections.size());
            std::int64_t at = 0;

            for (std::size_t k = 0; k < sections.size(); ++k)
            {
                auto& piece = pieces[k];
                const auto& section = sections[k];

                piece.in = std::llround (section.in * rate);
                const auto length = std::max<std::int64_t> (1, std::llround (section.out * rate) - piece.in);
                piece.start = at;
                piece.end = at + length;
                piece.trimDb = section.trimDb;
                piece.trim = gainOf (section.trimDb);
                at = piece.end;
            }

            for (std::size_t k = 1; k < sections.size(); ++k)
            {
                const auto& before = sections[k - 1];
                const auto& after = sections[k];
                const auto fade = std::llround (doc::crossfadeInto (sections, k) * rate);

                if (doc::isContinuousJoin (before, after))
                {
                    /*  ONE IN THE FILE: plays plain; a trim that differs ramps
                        across the join, straight in dB, over the crossfade's
                        length and five milliseconds at the least. */
                    if (std::abs (before.trimDb - after.trimDb) >= 1.0e-6)
                    {
                        const auto ramp = std::max (fade, std::llround (doc::leastTrimRamp * rate));
                        pieces[k - 1].rampOut = true;
                        pieces[k - 1].rampOutFrames = ramp;
                        pieces[k - 1].trimAfterDb = after.trimDb;
                        pieces[k].rampIn = true;
                        pieces[k].rampInFrames = ramp;
                        pieces[k].trimBeforeDb = before.trimDb;
                    }
                }
                else if (fade > 1)
                {
                    pieces[k - 1].fadeOut = fade;
                    pieces[k].fadeIn = fade;
                }
            }

            return pieces;
        }
    }

    //==============================================================================
    RenderResult renderEdit (const std::string& sourcePath, const std::vector<doc::Section>& sections,
                             const std::string& targetPath, const std::atomic<bool>* stop,
                             const std::function<void (int)>& progress)
    {
        RenderResult result;

        const auto fail = [&result] (const std::string& why)
        {
            result.problem = why;
            return result;
        };

        if (sections.empty())
            return fail ("there are no sections to render");

        if (const auto why = doc::whyNotSections (sections); ! why.empty())
            return fail (why);

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        const juce::File source { juce::String::fromUTF8 (sourcePath.c_str()) };
        std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (source) };

        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels == 0)
            return fail ("the file could not be read");

        const auto rate = reader->sampleRate;
        const auto channels = static_cast<int> (reader->numChannels);
        const auto fileFrames = reader->lengthInSamples;
        const auto pieces = piecesOf (sections, rate);
        const auto total = pieces.back().end;

        const juce::File target { juce::String::fromUTF8 (targetPath.c_str()) };
        target.getParentDirectory().createDirectory();
        const auto partial = target.getSiblingFile (target.getFileName() + ".part");
        partial.deleteFile();

        {
            std::unique_ptr<juce::OutputStream> stream { partial.createOutputStream() };

            if (stream == nullptr)
                return fail ("the render folder could not be written to");

            juce::WavAudioFormat wav;
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                           .withSampleRate (rate)
                                                           .withNumChannels (channels)
                                                           .withBitsPerSample (32)
                                                           .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

            if (writer == nullptr)
            {
                partial.deleteFile();
                return fail ("the WAV writer would not open");
            }

            juce::AudioBuffer<float> out { channels, blockFrames };
            juce::AudioBuffer<float> scratch { channels, blockFrames };
            std::vector<float> envelope (static_cast<std::size_t> (blockFrames));

            /*  The file read into `scratch` from `from`, frames past either
                end of it left silent. */
            const auto readFile = [&] (std::int64_t from, int frames)
            {
                scratch.clear();

                const auto first = std::max<std::int64_t> (0, from);
                const auto last = std::min<std::int64_t> (fileFrames, from + frames);

                if (last <= first)
                    return;

                std::vector<float*> into (static_cast<std::size_t> (channels));

                for (int c = 0; c < channels; ++c)
                    into[static_cast<std::size_t> (c)] = scratch.getWritePointer (c) + (first - from);

                reader->read (into.data(), channels, first, static_cast<int> (last - first));
            };

            for (std::int64_t t0 = 0; t0 < total; t0 += blockFrames)
            {
                if (stopRequested (stop))
                {
                    writer.reset();
                    partial.deleteFile();
                    return fail ("stopped");
                }

                const auto frames = static_cast<int> (std::min<std::int64_t> (blockFrames, total - t0));
                const auto t1 = t0 + frames;
                out.clear();

                for (const auto& piece : pieces)
                {
                    /*  Where this piece is heard: its own frames, and the half
                        fades either side of them. */
                    const auto from = piece.start - piece.fadeIn / 2;
                    const auto to = piece.end + piece.fadeOut / 2;
                    const auto a = std::max (from, t0);
                    const auto b = std::min (to, t1);

                    if (b <= a)
                        continue;

                    const auto count = static_cast<int> (b - a);
                    readFile (piece.in + (a - piece.start), count);

                    for (int i = 0; i < count; ++i)
                    {
                        const auto t = a + i;
                        auto gain = piece.trim;

                        if (piece.fadeIn > 0 && t < piece.start + piece.fadeIn / 2)
                        {
                            const auto theta = static_cast<double> (t - (piece.start - piece.fadeIn / 2))
                                                 / static_cast<double> (piece.fadeIn) * halfPi;
                            gain *= static_cast<float> (std::sin (theta));
                        }

                        if (piece.fadeOut > 0 && t >= piece.end - piece.fadeOut / 2)
                        {
                            const auto theta = static_cast<double> (t - (piece.end - piece.fadeOut / 2))
                                                 / static_cast<double> (piece.fadeOut) * halfPi;
                            gain *= static_cast<float> (std::cos (theta));
                        }

                        if (piece.rampIn && t < piece.start + piece.rampInFrames / 2)
                        {
                            const auto share = 0.5 + static_cast<double> (t - piece.start) / static_cast<double> (piece.rampInFrames);
                            gain = gainOf (piece.trimBeforeDb + share * (piece.trimDb - piece.trimBeforeDb));
                        }

                        if (piece.rampOut && t >= piece.end - piece.rampOutFrames / 2)
                        {
                            const auto share = static_cast<double> (t - (piece.end - piece.rampOutFrames / 2))
                                                 / static_cast<double> (piece.rampOutFrames);
                            gain = gainOf (piece.trimDb + share * (piece.trimAfterDb - piece.trimDb));
                        }

                        envelope[static_cast<std::size_t> (i)] = gain;
                    }

                    const auto offset = static_cast<int> (a - t0);

                    for (int c = 0; c < channels; ++c)
                    {
                        auto* into = out.getWritePointer (c) + offset;
                        const auto* read = scratch.getReadPointer (c);

                        for (int i = 0; i < count; ++i)
                            into[i] += read[i] * envelope[static_cast<std::size_t> (i)];
                    }
                }

                if (! writer->writeFromAudioSampleBuffer (out, 0, frames))
                {
                    writer.reset();
                    partial.deleteFile();
                    return fail ("the disk would not take the whole render");
                }

                if (progress)
                    progress (static_cast<int> (std::min<std::int64_t> (100, (t1 * 100) / std::max<std::int64_t> (1, total))));
            }

            writer.reset();
        }

        target.deleteFile();

        if (! partial.moveFileTo (target))
        {
            partial.deleteFile();
            return fail ("the render could not be moved into place");
        }

        result.ok = true;
        result.seconds = static_cast<double> (total) / rate;
        return result;
    }

    std::string renderKeyOf (const std::string& sourceName, std::int64_t sizeBytes, std::int64_t modifiedMs,
                             const std::string& editText)
    {
        const auto words = sourceName + "\n" + std::to_string (sizeBytes) + "\n" + std::to_string (modifiedMs) + "\n" + editText;
        const juce::SHA256 digest { words.data(), words.size() };
        return digest.toHexString().substring (0, 16).toStdString();
    }

    std::string freeBounceName (const std::string& folder, const std::string& sourceName)
    {
        const juce::File dir { juce::String::fromUTF8 (folder.c_str()) };
        const auto stem = juce::File (juce::String::fromUTF8 (sourceName.c_str())).getFileNameWithoutExtension();
        const auto base = stem + " (edit)";

        const auto extension = renderExtensionOf (sourceName);

        for (int n = 1; n < 1000; ++n)
        {
            const auto name = (n == 1 ? base : base + " " + juce::String (n)) + extension;

            if (! dir.getChildFile (name).exists())
                return name.toStdString();
        }

        return {};
    }

    //==============================================================================
    EditRenderer::EditRenderer (MediaInfo& mediaToPublishInto, std::string mediaFolder)
        : media (&mediaToPublishInto), folder (std::move (mediaFolder))
    {
    }

    EditRenderer::~EditRenderer()
    {
        stop();
    }

    bool EditRenderer::start()
    {
        if (running.load (std::memory_order_relaxed) || folder.empty())
            return false;

        stopping.store (false, std::memory_order_relaxed);
        running.store (true, std::memory_order_relaxed);
        thread = std::thread ([this] { run(); });
        return true;
    }

    void EditRenderer::stop()
    {
        if (! running.load (std::memory_order_relaxed))
            return;

        /*  Raised under the lock, as the analyser's is, so the thread either
            sees it before it sleeps or is woken by the notify. */
        {
            const std::lock_guard<std::mutex> lock { guard };
            stopping.store (true, std::memory_order_relaxed);
        }

        wake.notify_all();

        if (thread.joinable())
            thread.join();

        running.store (false, std::memory_order_relaxed);

        const std::lock_guard<std::mutex> lock { guard };
        queued.clear();
        freezes.clear();
        sweepWanted.clear();
        sweepAsked = false;
    }

    std::string EditRenderer::keyFor (const RenderJob& job) const
    {
        const juce::File source { juce::String::fromUTF8 (resolveMediaPath (folder, job.sourceName).c_str()) };

        return renderKeyOf (job.sourceName, source.getSize(), source.getLastModificationTime().toMilliseconds(),
                            doc::editText (job.sections));
    }

    void EditRenderer::offer (const RenderJob& job)
    {
        if (job.cue.empty() || job.sourceName.empty() || job.sections.empty())
            return;

        const auto text = doc::editText (job.sections);

        {
            const std::lock_guard<std::mutex> lock { guard };

            if (const auto found = known->find (job.cue); found != known->end()
                  && found->second.editText == text
                  && (found->second.state == renderState::done || found->second.state == renderState::rendering))
                return;

            /*  A job of the same cue still waiting is this one now. */
            queued.erase (std::remove_if (queued.begin(), queued.end(),
                                          [&job] (const RenderJob& waiting) { return waiting.cue == job.cue; }),
                          queued.end());
            queued.push_back (job);

            /*  Said to be rendering from the moment it is asked for, so the
                resolver never plays a render of another edit meanwhile. */
            auto next = std::make_shared<EditRenders> (*known);
            auto& entry = (*next)[job.cue];
            const auto previous = entry.file;
            entry.cue = job.cue;
            entry.kind = kindOf (job.sourceName);
            entry.editText = text;
            entry.state = renderState::rendering;
            entry.percent = 0;
            entry.problem.clear();
            entry.seconds = doc::editedLength (job.sections);

            if (! previous.empty() && std::find (replaced.begin(), replaced.end(), previous) == replaced.end())
                replaced.push_back (previous);

            entry.file.clear();
            entry.key.clear();
            known = std::move (next);
            moved.fetch_add (1, std::memory_order_acq_rel);
        }

        wake.notify_one();
    }

    void EditRenderer::forget (const std::string& cue)
    {
        const std::lock_guard<std::mutex> lock { guard };

        queued.erase (std::remove_if (queued.begin(), queued.end(),
                                      [&cue] (const RenderJob& waiting) { return waiting.cue == cue; }),
                      queued.end());

        const auto found = known->find (cue);

        if (found == known->end())
            return;

        if (! found->second.file.empty())
            replaced.push_back (found->second.file);

        auto next = std::make_shared<EditRenders> (*known);
        next->erase (cue);
        known = std::move (next);
        moved.fetch_add (1, std::memory_order_acq_rel);
    }

    void EditRenderer::sweep (std::vector<RenderJob> openEdits)
    {
        {
            const std::lock_guard<std::mutex> lock { guard };
            sweepWanted = std::move (openEdits);
            sweepAsked = true;
        }

        wake.notify_one();
    }

    std::vector<std::string> EditRenderer::takeStale()
    {
        const std::lock_guard<std::mutex> lock { guard };
        auto out = std::move (replaced);
        replaced.clear();
        return out;
    }

    void EditRenderer::stale (const std::string& relativeFile)
    {
        const std::lock_guard<std::mutex> lock { guard };

        if (std::find (replaced.begin(), replaced.end(), relativeFile) == replaced.end())
            replaced.push_back (relativeFile);
    }

    void EditRenderer::discard (const std::string& relativeFile)
    {
        /*  Only a render: nothing outside `.edits/` is this object's to remove. */
        if (relativeFile.rfind (std::string (editsFolder) + "/", 0) != 0)
            return;

        juce::File (juce::String::fromUTF8 (resolveMediaPath (folder, relativeFile).c_str())).deleteFile();
    }

    std::shared_ptr<const EditRenders> EditRenderer::snapshot() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return known;
    }

    std::string EditRenderer::readoutText (const EditRenders& renders)
    {
        std::string text;

        for (const auto& [cue, render] : renders)
        {
            if (! text.empty())
                text.push_back ('\n');

            text += cue + "\t" + render.state + "\t" + std::to_string (render.percent) + "\t" + render.problem
                  + "\t" + render.kind;
        }

        return text;
    }

    void EditRenderer::setOnFrozen (FreezeDone done)
    {
        const std::lock_guard<std::mutex> lock { guard };
        onFrozen = std::move (done);
    }

    void EditRenderer::freeze (const FreezeJob& job)
    {
        {
            const std::lock_guard<std::mutex> lock { guard };
            freezes.push_back (job);
        }

        wake.notify_one();
    }

    void EditRenderer::publishEntry (const EditRender& entry)
    {
        const std::lock_guard<std::mutex> lock { guard };
        auto next = std::make_shared<EditRenders> (*known);
        (*next)[entry.cue] = entry;
        known = std::move (next);
        moved.fetch_add (1, std::memory_order_acq_rel);
    }

    //==============================================================================
    void EditRenderer::run()
    {
        for (;;)
        {
            std::optional<RenderJob> job;
            std::optional<FreezeJob> freezeJob;
            std::vector<RenderJob> toSweep;
            bool sweeping = false;

            {
                std::unique_lock<std::mutex> lock { guard };
                wake.wait (lock, [this] { return stopping.load (std::memory_order_relaxed)
                                                 || ! queued.empty() || ! freezes.empty() || sweepAsked; });

                if (stopping.load (std::memory_order_relaxed))
                    return;

                /*  A freeze first - somebody is waiting on it - then the renders
                    in the order they were asked for, then the sweep. */
                if (! freezes.empty())
                {
                    freezeJob = std::move (freezes.front());
                    freezes.pop_front();
                }
                else if (! queued.empty())
                {
                    job = std::move (queued.front());
                    queued.pop_front();
                }
                else
                {
                    sweeping = true;
                    toSweep = std::move (sweepWanted);
                    sweepWanted.clear();
                    sweepAsked = false;
                }
            }

            try
            {
                if (freezeJob.has_value())
                    runFreeze (*freezeJob);
                else if (job.has_value())
                    render (*job);
                else if (sweeping)
                    runSweep (toSweep);
            }
            catch (const std::exception&)
            {
            }
        }
    }

    void EditRenderer::render (const RenderJob& job)
    {
        const auto movie = isMovieJob (job.sourceName);
        const auto extension = renderExtensionOf (job.sourceName);

        EditRender entry;
        entry.cue = job.cue;
        entry.kind = kindOf (job.sourceName);
        entry.editText = doc::editText (job.sections);
        entry.key = keyFor (job);
        entry.file = std::string (editsFolder) + "/" + entry.key + extension;
        entry.seconds = doc::editedLength (job.sections);

        const auto target = juce::File (juce::String::fromUTF8 (folder.c_str()))
                              .getChildFile (editsFolder).getChildFile (juce::String (entry.key) + extension);

        /*  A RENDER OF THIS VERY EDIT IS THERE ALREADY - an edit put back as it
            was, an unfreeze: it is the render, and nothing is read. */
        if (target.existsAsFile())
        {
            entry.state = renderState::done;
            entry.percent = 100;
        }
        else
        {
            entry.state = renderState::rendering;
            publishEntry (entry);

            const auto progress = [this, &entry] (int percent)
            {
                if (percent != entry.percent && percent % 10 == 0)
                {
                    entry.percent = percent;
                    publishEntry (entry);
                }
            };

            bool ok = false;
            std::string problem;

            if (movie)
            {
                /*  A MOVIE ON ITS OWN GRID (§55.5, ADV): a variable-rate source
                    lands on its dominant grid, and the readout says so. */
                const auto result = video::movie::renderMovieEdit (resolveMediaPath (folder, job.sourceName), job.sections,
                                                                   target.getFullPathName().toStdString(), &stopping, progress);
                ok = result.ok;
                problem = result.problem;

                if (ok && result.resampled)
                    problem = "resampled onto " + osc::formatDouble (result.frameRate) + " fps";
            }
            else
            {
                const auto result = renderEdit (resolveMediaPath (folder, job.sourceName), job.sections,
                                                target.getFullPathName().toStdString(), &stopping, progress);
                ok = result.ok;
                problem = result.problem;
            }

            if (stopping.load (std::memory_order_relaxed))
                return;

            entry.state = ok ? renderState::done : renderState::failed;
            entry.percent = ok ? 100 : entry.percent;
            entry.problem = problem;

            if (! ok)
                entry.file.clear();
        }

        /*  A STALE OFFER: the edit changed while this rendered, and the newer
            job is queued; what was made is a render nobody will play. */
        {
            const std::lock_guard<std::mutex> lock { guard };

            if (const auto found = known->find (job.cue); found != known->end() && found->second.editText != entry.editText)
            {
                if (! entry.file.empty())
                    replaced.push_back (entry.file);

                return;
            }
        }

        if (entry.state == renderState::done && media != nullptr)
        {
            MediaRecord record;
            record.seconds = entry.seconds;
            media->publish (entry.file, std::move (record));
        }

        publishEntry (entry);
    }

    void EditRenderer::runSweep (const std::vector<RenderJob>& openEdits)
    {
        const auto dir = juce::File (juce::String::fromUTF8 (folder.c_str())).getChildFile (editsFolder);

        if (! dir.isDirectory())
            return;

        std::set<std::string> keep;

        for (const auto& job : openEdits)
            keep.insert (keyFor (job));

        const auto hourAgo = juce::Time::getCurrentTime() - juce::RelativeTime::hours (1);

        for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFiles))
        {
            const auto file = entry.getFile();

            if (stopping.load (std::memory_order_relaxed))
                return;

            if (file.hasFileExtension ("part"))
            {
                if (file.getLastModificationTime() < hourAgo)
                    file.deleteFile();

                continue;
            }

            if (! file.hasFileExtension ("wav") && ! file.hasFileExtension ("mov"))
                continue;

            if (keep.count (file.getFileNameWithoutExtension().toStdString()) == 0)
                file.deleteFile();
        }
    }

    void EditRenderer::runFreeze (const FreezeJob& job)
    {
        FreezeDone done;

        {
            const std::lock_guard<std::mutex> lock { guard };
            done = onFrozen;
        }

        const auto say = [&] (const std::string& bounce, const std::string& soundBounce, const std::string& problem)
        {
            if (! problem.empty())
            {
                const std::lock_guard<std::mutex> lock { guard };

                if (const auto found = known->find (job.cue); found != known->end())
                {
                    auto next = std::make_shared<EditRenders> (*known);
                    (*next)[job.cue].problem = problem;
                    known = std::move (next);
                    moved.fetch_add (1, std::memory_order_acq_rel);
                }
            }

            if (done)
                done (job, bounce, soundBounce, problem);
        };

        /*  ONE BOUNCE: the render copied beside its source - the show's own
            media/, or the folder around it where the source lives - under a
            free name; the name landed, or empty with the problem said. */
        const auto land = [this] (const std::string& renderFile, const std::string& source,
                                  std::string& problem) -> std::string
        {
            const juce::File render { juce::String::fromUTF8 (resolveMediaPath (folder, renderFile).c_str()) };

            if (! render.existsAsFile())
            {
                problem = "the render is not there";
                return {};
            }

            const auto into = mediaRootOf (folder, source);
            const auto name = freeBounceName (into, source);

            if (name.empty())
            {
                problem = "no free name for the bounce";
                return {};
            }

            const auto target = juce::File (juce::String::fromUTF8 (into.c_str())).getChildFile (juce::String::fromUTF8 (name.c_str()));
            const auto partial = target.getSiblingFile ("." + target.getFileName() + ".part");
            partial.deleteFile();

            if (! render.copyFileTo (partial))
            {
                partial.deleteFile();
                problem = "the bounce could not be written";
                return {};
            }

            if (! partial.moveFileTo (target))
            {
                partial.deleteFile();
                problem = "the bounce could not be moved into place";
                return {};
            }

            if (media != nullptr)
            {
                const auto path = target.getFullPathName().toStdString();
                MediaRecord record;
                record.seconds = isMovieJob (source) ? std::max (0.0, video::movie::durationOf (path))
                                                     : mediaDurationSeconds (path);
                media->publish (name, std::move (record));
            }

            return name;
        };

        std::string problem;
        const auto name = land (job.renderFile, job.source, problem);

        if (name.empty())
            return say ({}, {}, problem);

        /*  THE PAIR (§55.5, ADW): the sound's bounce beside its own source,
            and the movie's taken back when it cannot land - one answer, or
            none. */
        std::string soundName;

        if (! job.soundRenderFile.empty())
        {
            soundName = land (job.soundRenderFile, job.soundSource, problem);

            if (soundName.empty())
            {
                juce::File (juce::String::fromUTF8 (mediaRootOf (folder, job.source).c_str()))
                    .getChildFile (juce::String::fromUTF8 (name.c_str())).deleteFile();
                return say ({}, {}, "the sound's " + problem.substr (4));
            }
        }

        say (name, soundName, {});
    }

    //==============================================================================
    void registerEditRenderCommands (CommandRegistry& registry, EditRenderer* renderer, const doc::ShowDocument& document)
    {
        registry.add ({ "media.freeze",
                        "Asks for a sound's edit to be frozen (namespace draft 55, ADN): its render copied into"
                        " media/ as \"<stem> (edit).wav\", after which media.frozen points the cue at it. Refused"
                        " busy until the render of the edit as it now is exists. A movie is frozen with its locked"
                        " sound, as one pair (55.5, ADW), once both renders are there; the sound's own freeze is"
                        " refused locked-to-movie. Taken and ignored where nothing renders.",
                        { { "cue", 's', false } },
                        false,
                        [renderer, &document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto cueId = args.empty() || ! args[0].isString() ? std::string {} : args[0].getString();
                            const auto cue = document.findById (cueId);

                            if (! cue.isValid())
                                return Outcome::rejected (reason::unknownId);

                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            const auto movie = document.isMovieCue (cue);

                            if (! cue.hasType ("Media") && ! movie)
                                return Outcome::rejected (reason::typeMismatch);

                            //  A sound locked to a movie is frozen with it (55.5, ADW): edit the movie.
                            if (cue.hasType ("Media") && cue["lockedTo"].toString().isNotEmpty())
                                return Outcome::rejected (reason::lockedToMovie);

                            if (! document.hasOpenEdit (cue))
                                return Outcome::rejected (reason::badValue);

                            if (renderer == nullptr)
                                return Outcome::ok (args);

                            const auto renders = renderer->snapshot();

                            /*  The render of this cue's edit as it now is: its file, or
                                nothing while it is not there yet. */
                            const auto renderOf = [&renders] (const juce::ValueTree& node) -> std::string
                            {
                                const auto found = renders->find (node["id"].toString().toStdString());

                                if (found == renders->end() || found->second.state != renderState::done
                                      || found->second.editText != cue::editTextOf (node) || found->second.file.empty())
                                    return {};

                                return found->second.file;
                            };

                            EditRenderer::FreezeJob job;
                            job.cue = cueId;
                            job.source = cue["file"].toString().toStdString();
                            job.renderFile = renderOf (cue);

                            if (job.renderFile.empty())
                                return Outcome::rejected (reason::busy);

                            /*  THE PAIR: a movie's locked sound cut in step freezes with
                                it, once its render is of the sections as they now are. */
                            if (movie)
                                for (const auto& sound : document.soundsLockedTo (cue))
                                {
                                    if (! document.hasOpenEdit (sound))
                                        continue;

                                    job.soundCue = sound["id"].toString().toStdString();
                                    job.soundSource = sound["file"].toString().toStdString();
                                    job.soundRenderFile = renderOf (sound);

                                    if (job.soundRenderFile.empty())
                                        return Outcome::rejected (reason::busy);

                                    break;
                                }

                            renderer->freeze (job);
                            return Outcome::ok (args);
                        } });
    }
}
