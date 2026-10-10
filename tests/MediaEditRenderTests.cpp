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

/*  THE RENDER OF A SOUND'S EDIT (namespace draft §55, ADL, ADM).

    The oracle is a ramp: a file whose every sample says where in the file it
    came from, so a cut, a reorder, a removal and a crossfade are each checked
    sample by sample against the arithmetic they claim. Then the service: a
    job rendered on its thread and published with its length, a render of the
    very same edit found rather than made again, a stale one let go of, the
    cache swept, and a freeze copied beside its source under a free name.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "HapMovieWriter.h"
#include "TestSupport.h"

#include <wfg/engine/audio/EditRenderer.h>
#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/document/MediaEdit.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/video/Movie.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace wfg;
using doc::Section;

namespace
{
    constexpr int rate = 48000;
    constexpr int rampSeconds = 4;
    constexpr int rampFrames = rate * rampSeconds;

    /** The ramp's value at a frame of the file: channel one is the negative of channel nought. */
    float rampValue (std::int64_t frame, int channel = 0)
    {
        if (frame < 0 || frame >= rampFrames)
            return 0.0f;

        const auto value = 0.8f * static_cast<float> (frame) / static_cast<float> (rampFrames);
        return channel == 0 ? value : -value;
    }

    /*  A four-second stereo ramp, 24-bit, so a swapped channel or an
        off-by-one frame would be caught. */
    juce::File writeStereoRamp (const juce::File& folder, const juce::String& name = "ramp.wav")
    {
        folder.createDirectory();
        const auto file = folder.getChildFile (name);

        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };
        REQUIRE (stream != nullptr);

        auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                          .withSampleRate (static_cast<double> (rate))
                                                          .withNumChannels (2)
                                                          .withBitsPerSample (24));
        REQUIRE (writer != nullptr);

        juce::AudioBuffer<float> buffer { 2, rampFrames };

        for (int n = 0; n < rampFrames; ++n)
        {
            buffer.setSample (0, n, rampValue (n, 0));
            buffer.setSample (1, n, rampValue (n, 1));
        }

        writer->writeFromAudioSampleBuffer (buffer, 0, rampFrames);
        return file;
    }

    juce::AudioBuffer<float> readWhole (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (file) };
        REQUIRE (reader != nullptr);

        juce::AudioBuffer<float> buffer { static_cast<int> (reader->numChannels), static_cast<int> (reader->lengthInSamples) };
        reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
        return buffer;
    }

    Section piece (const char* id, double in, double out, double trim = 0.0, double crossfade = 0.0)
    {
        return { id, in, out, trim, crossfade };
    }

    struct Scratch
    {
        Scratch()
        {
            folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("wfg-edit-render-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
            media = folder.getChildFile ("media");
            source = writeStereoRamp (media);
        }

        ~Scratch() { folder.deleteRecursively(); }

        juce::File target (const juce::String& name) const { return media.getChildFile (".edits").getChildFile (name); }

        juce::File folder, media, source;
    };

    /*  24-bit in, float out: a sample is where it was to within one step. */
    constexpr float oneStep = 2.0f / 16777216.0f;
}

//==============================================================================
TEST_CASE ("edit render: a plain cut and a reorder put each section's material where the timeline says")
{
    Scratch scratch;

    /*  The last second first, then the first second: two seconds of output. */
    const std::vector<Section> sections { piece ("B", 3.0, 4.0), piece ("A", 0.0, 1.0) };
    const auto result = audio::renderEdit (scratch.source.getFullPathName().toStdString(), sections,
                                           scratch.target ("cut.wav").getFullPathName().toStdString());

    REQUIRE_MESSAGE (result.ok, result.problem);
    CHECK (result.seconds == doctest::Approx (2.0));

    const auto out = readWhole (scratch.target ("cut.wav"));
    REQUIRE (out.getNumChannels() == 2);
    REQUIRE (out.getNumSamples() == 2 * rate);

    /*  Across a hard cut at 10 ms by default? No: these sections carry no
        crossfade, so the join is a plain cut and every sample is exact. */
    for (const int t : { 0, 100, rate / 2, rate - 1, rate, rate + 100, 2 * rate - 1 })
    {
        const auto fileFrame = t < rate ? 3 * rate + t : t - rate;
        CHECK (out.getSample (0, t) == doctest::Approx (rampValue (fileFrame, 0)).epsilon (oneStep).scale (1.0));
        CHECK (out.getSample (1, t) == doctest::Approx (rampValue (fileFrame, 1)).epsilon (oneStep).scale (1.0));
    }
}

TEST_CASE ("edit render: a trim scales a section, and a continuous join with equal trims is the file itself")
{
    Scratch scratch;

    /*  Split at two seconds, the second half at -6 dB: the first half is the
        file, the second the file at half amplitude, the join plain. */
    const std::vector<Section> sections { piece ("A", 0.0, 2.0), piece ("B", 2.0, 4.0, -6.0206, 0.5) };
    const auto result = audio::renderEdit (scratch.source.getFullPathName().toStdString(), sections,
                                           scratch.target ("trim.wav").getFullPathName().toStdString());
    REQUIRE_MESSAGE (result.ok, result.problem);

    const auto out = readWhole (scratch.target ("trim.wav"));
    REQUIRE (out.getNumSamples() == 4 * rate);

    for (const int t : { 0, rate, 2 * rate - 15000 })   // the ramp is the half second around the join
        CHECK (out.getSample (0, t) == doctest::Approx (rampValue (t, 0)).epsilon (oneStep).scale (1.0));

    for (const int t : { 2 * rate + 15000, 3 * rate, 4 * rate - 1 })
        CHECK (out.getSample (0, t) == doctest::Approx (0.5f * rampValue (t, 0)).epsilon (2.0f * oneStep).scale (1.0));

    /*  Across the join the trim ramps straight in dB over the crossfade's
        half second: halfway through, -3 dB. */
    const auto mid = 2 * rate;
    CHECK (out.getSample (0, mid) == doctest::Approx (rampValue (mid, 0) * std::pow (10.0f, -3.0103f / 20.0f)).epsilon (0.01));

    /*  Equal trims: bit for bit the file, through a 0.5 s crossfade that is
        not heard since the join is one in the file. */
    const std::vector<Section> same { piece ("A", 0.0, 2.0), piece ("B", 2.0, 4.0, 0.0, 0.5) };
    REQUIRE (audio::renderEdit (scratch.source.getFullPathName().toStdString(), same,
                                scratch.target ("same.wav").getFullPathName().toStdString()).ok);
    const auto plain = readWhole (scratch.target ("same.wav"));

    for (const int t : { 2 * rate - 1000, 2 * rate - 1, 2 * rate, 2 * rate + 1000 })
        CHECK (plain.getSample (0, t) == doctest::Approx (rampValue (t, 0)).epsilon (oneStep).scale (1.0));
}

TEST_CASE ("edit render: a crossfade is equal power, centred on the join, from material beyond the edges, and the length does not move")
{
    Scratch scratch;

    /*  The third second then the first, with a 100 ms crossfade into the
        second piece: over the 100 ms around the join the outgoing goes on
        past its out point (frame 3 s + t) fading out, while the incoming
        starts before its in point (frame 1 s - 50 ms + t) fading in. */
    const std::vector<Section> sections { piece ("B", 2.0, 3.0), piece ("A", 1.0, 2.0, 0.0, 0.1) };
    const auto result = audio::renderEdit (scratch.source.getFullPathName().toStdString(), sections,
                                           scratch.target ("fade.wav").getFullPathName().toStdString());
    REQUIRE_MESSAGE (result.ok, result.problem);
    CHECK (result.seconds == doctest::Approx (2.0));

    const auto out = readWhole (scratch.target ("fade.wav"));
    REQUIRE (out.getNumSamples() == 2 * rate);

    const auto fade = static_cast<int> (0.1 * rate);
    const auto join = rate;

    for (const int t : { join - fade / 2, join - fade / 4, join, join + fade / 4, join + fade / 2 - 1 })
    {
        const auto theta = static_cast<double> (t - (join - fade / 2)) / fade * 1.5707963267948966;
        const auto outgoing = rampValue (3 * rate + (t - join), 0) * static_cast<float> (std::cos (theta));
        const auto incoming = rampValue (rate + (t - join), 0) * static_cast<float> (std::sin (theta));
        CHECK (out.getSample (0, t) == doctest::Approx (outgoing + incoming).epsilon (0.001));
    }

    /*  Outside the fade, plain. */
    CHECK (out.getSample (0, join - fade) == doctest::Approx (rampValue (2 * rate + (join - fade) , 0)).epsilon (oneStep).scale (1.0));
    CHECK (out.getSample (0, join + fade) == doctest::Approx (rampValue (rate + fade, 0)).epsilon (oneStep).scale (1.0));
}

TEST_CASE ("edit render: a crossfade past the file's edges reads silence there, and a section at the file's start gets a hard cut")
{
    Scratch scratch;

    /*  The first second last: nothing before frame nought for its fade, so
        the door's clamp - applied again here - makes it a hard cut. */
    const std::vector<Section> sections { piece ("B", 2.0, 3.0), piece ("A", 0.0, 1.0, 0.0, 0.2) };
    REQUIRE (audio::renderEdit (scratch.source.getFullPathName().toStdString(), sections,
                                scratch.target ("edge.wav").getFullPathName().toStdString()).ok);
    const auto out = readWhole (scratch.target ("edge.wav"));

    CHECK (out.getSample (0, rate - 1) == doctest::Approx (rampValue (3 * rate - 1, 0)).epsilon (oneStep).scale (1.0));
    CHECK (out.getSample (0, rate) == doctest::Approx (rampValue (0, 0)).epsilon (oneStep).scale (1.0));

    /*  The last second then the first, a fade into the first: the outgoing
        goes on past the file's end, which is silence - so at the join only
        the incoming is heard, at its sine. */
    const std::vector<Section> past { piece ("B", 3.0, 4.0), piece ("A", 1.0, 2.0, 0.0, 0.1) };
    REQUIRE (audio::renderEdit (scratch.source.getFullPathName().toStdString(), past,
                                scratch.target ("past.wav").getFullPathName().toStdString()).ok);
    const auto end = readWhole (scratch.target ("past.wav"));
    const auto fade = static_cast<int> (0.1 * rate);
    const auto t = rate + fade / 4;
    const auto theta = static_cast<double> (t - (rate - fade / 2)) / fade * 1.5707963267948966;
    CHECK (end.getSample (0, t) == doctest::Approx (rampValue (rate + fade / 4, 0) * static_cast<float> (std::sin (theta))).epsilon (0.001));
}

TEST_CASE ("edit render: a stop leaves no file behind, and a file that cannot be read says so")
{
    Scratch scratch;

    std::atomic<bool> stop { true };
    const std::vector<Section> sections { piece ("A", 0.0, 4.0) };
    const auto stopped = audio::renderEdit (scratch.source.getFullPathName().toStdString(), sections,
                                            scratch.target ("stopped.wav").getFullPathName().toStdString(), &stop);
    CHECK_FALSE (stopped.ok);
    CHECK_FALSE (scratch.target ("stopped.wav").exists());
    CHECK_FALSE (scratch.target ("stopped.wav.part").exists());

    const auto unreadable = audio::renderEdit (scratch.media.getChildFile ("nothing.wav").getFullPathName().toStdString(), sections,
                                               scratch.target ("none.wav").getFullPathName().toStdString());
    CHECK_FALSE (unreadable.ok);
    CHECK (unreadable.problem == "the file could not be read");

    CHECK_FALSE (audio::renderEdit (scratch.source.getFullPathName().toStdString(), {},
                                    scratch.target ("empty.wav").getFullPathName().toStdString()).ok);
}

TEST_CASE ("edit render: the key names a distinct edit of a distinct file, and the bounce's name is free")
{
    const auto a = audio::renderKeyOf ("rain.wav", 100, 200, "0 10 0 0;");
    CHECK (a.size() == 16);
    CHECK (a == audio::renderKeyOf ("rain.wav", 100, 200, "0 10 0 0;"));
    CHECK (a != audio::renderKeyOf ("rain.wav", 100, 200, "0 12 0 0;"));
    CHECK (a != audio::renderKeyOf ("rain.wav", 101, 200, "0 10 0 0;"));
    CHECK (a != audio::renderKeyOf ("wind.wav", 100, 200, "0 10 0 0;"));

    Scratch scratch;
    CHECK (audio::freeBounceName (scratch.media.getFullPathName().toStdString(), "ramp.wav") == "ramp (edit).wav");
    scratch.media.getChildFile ("ramp (edit).wav").create();
    CHECK (audio::freeBounceName (scratch.media.getFullPathName().toStdString(), "ramp.wav") == "ramp (edit) 2.wav");
    CHECK (audio::freeBounceName (scratch.media.getFullPathName().toStdString(), "takes/ramp.wav") == "ramp (edit) 2.wav");
}

//==============================================================================
namespace
{
    template <typename Predicate>
    bool soon (Predicate&& done, int tenths = 100)
    {
        for (int i = 0; i < tenths; ++i)
        {
            if (done())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }

        return done();
    }

    audio::RenderJob jobFor (const std::string& cue, const std::vector<Section>& sections)
    {
        return { cue, "ramp.wav", sections };
    }
}

TEST_CASE ("edit renderer: a job offered is rendered on its thread, published with its length, and found again rather than made again")
{
    Scratch scratch;
    doc::ShowDocument document;
    audio::MediaInfo media { document, scratch.media.getFullPathName().toStdString() };
    audio::EditRenderer renderer { media, scratch.media.getFullPathName().toStdString() };
    REQUIRE (renderer.start());

    const std::vector<Section> sections { piece ("B", 3.0, 4.0), piece ("A", 0.0, 1.0) };
    renderer.offer (jobFor ("CUE00001", sections));

    REQUIRE (soon ([&] { const auto s = renderer.snapshot(); const auto f = s->find ("CUE00001");
                         return f != s->end() && f->second.state == audio::renderState::done; }));

    const auto first = *renderer.snapshot();
    const auto& render = first.at ("CUE00001");
    CHECK (render.editText == doc::editText (sections));
    CHECK (render.file.rfind (".edits/", 0) == 0);
    CHECK (render.seconds == doctest::Approx (2.0));
    CHECK (render.percent == 100);
    CHECK (scratch.media.getChildFile (juce::String (render.file)).existsAsFile());

    /*  Its length reaches the lengths, by the name the cue will be armed with. */
    const auto lengths = media.durations();
    REQUIRE (lengths->count (render.file) == 1);
    CHECK (lengths->at (render.file) == doctest::Approx (2.0));

    /*  The readout. */
    CHECK (audio::EditRenderer::readoutText (first) == "CUE00001\tdone\t100\t\tsound");
    CHECK (render.kind == "sound");

    /*  Offered again as it is: nothing moves. */
    const auto before = renderer.changes();
    renderer.offer (jobFor ("CUE00001", sections));
    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    CHECK (renderer.changes() == before);

    /*  Changed, then put back: the second render is made; the first is found
        on disk and not made again, and its file is the same one. */
    const std::vector<Section> changed { piece ("B", 3.0, 4.0), piece ("A", 0.0, 2.0) };
    renderer.offer (jobFor ("CUE00001", changed));
    REQUIRE (soon ([&] { const auto s = renderer.snapshot(); const auto f = s->find ("CUE00001");
                         return f != s->end() && f->second.state == audio::renderState::done && f->second.editText == doc::editText (changed); }));
    const auto second = renderer.snapshot()->at ("CUE00001").file;
    CHECK (second != render.file);

    /*  The first render is now stale, handed over once. */
    const auto stale = renderer.takeStale();
    REQUIRE (stale.size() == 1);
    CHECK (stale.front() == render.file);
    CHECK (renderer.takeStale().empty());

    const auto wasWritten = scratch.media.getChildFile (juce::String (render.file)).getLastModificationTime();
    renderer.offer (jobFor ("CUE00001", sections));
    REQUIRE (soon ([&] { const auto s = renderer.snapshot(); const auto f = s->find ("CUE00001");
                         return f != s->end() && f->second.state == audio::renderState::done && f->second.editText == doc::editText (sections); }));
    CHECK (renderer.snapshot()->at ("CUE00001").file == render.file);
    CHECK (scratch.media.getChildFile (juce::String (render.file)).getLastModificationTime() == wasWritten);

    /*  Discarded, a render is gone; anything outside .edits is not this
        object's to remove. */
    renderer.discard (second);
    CHECK_FALSE (scratch.media.getChildFile (juce::String (second)).exists());
    renderer.discard ("ramp.wav");
    CHECK (scratch.source.existsAsFile());

    /*  Forgotten, the cue has no entry. */
    renderer.forget ("CUE00001");
    CHECK (renderer.snapshot()->count ("CUE00001") == 0);

    renderer.stop();
}

TEST_CASE ("edit renderer: a sweep keeps the renders of the open edits and a young part file, and removes the rest")
{
    Scratch scratch;
    doc::ShowDocument document;
    audio::MediaInfo media { document, scratch.media.getFullPathName().toStdString() };
    audio::EditRenderer renderer { media, scratch.media.getFullPathName().toStdString() };

    const auto edits = scratch.media.getChildFile (".edits");
    edits.createDirectory();
    edits.getChildFile ("0123456789abcdef.wav").create();
    edits.getChildFile ("young.wav.part").create();
    const auto old = edits.getChildFile ("old.wav.part");
    old.create();
    old.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::hours (2));
    scratch.media.getChildFile ("note.txt").create();

    const std::vector<Section> sections { piece ("A", 0.0, 1.0) };
    REQUIRE (renderer.start());
    renderer.offer (jobFor ("CUE00001", sections));
    REQUIRE (soon ([&] { const auto s = renderer.snapshot(); const auto f = s->find ("CUE00001");
                         return f != s->end() && f->second.state == audio::renderState::done; }));
    const auto kept = renderer.snapshot()->at ("CUE00001").file;

    renderer.sweep ({ jobFor ("CUE00001", sections) });
    REQUIRE (soon ([&] { return ! edits.getChildFile ("0123456789abcdef.wav").exists(); }));

    CHECK (scratch.media.getChildFile (juce::String (kept)).existsAsFile());
    CHECK (edits.getChildFile ("young.wav.part").exists());
    CHECK_FALSE (old.exists());
    CHECK (scratch.media.getChildFile ("note.txt").exists());
    CHECK (scratch.source.existsAsFile());

    renderer.stop();
}

TEST_CASE ("edit renderer: a freeze copies the render beside its source under a free name and says so")
{
    Scratch scratch;
    doc::ShowDocument document;
    audio::MediaInfo media { document, scratch.media.getFullPathName().toStdString() };
    audio::EditRenderer renderer { media, scratch.media.getFullPathName().toStdString() };

    std::atomic<bool> told { false };
    std::string bounce, problem;
    renderer.setOnFrozen ([&] (const audio::EditRenderer::FreezeJob&, const std::string& name, const std::string& soundName,
                               const std::string& why)
                          {
                              CHECK (soundName.empty());
                              bounce = name;
                              problem = why;
                              told.store (true);
                          });

    REQUIRE (renderer.start());

    const std::vector<Section> sections { piece ("B", 3.0, 4.0), piece ("A", 0.0, 1.0) };
    renderer.offer (jobFor ("CUE00001", sections));
    REQUIRE (soon ([&] { const auto s = renderer.snapshot(); const auto f = s->find ("CUE00001");
                         return f != s->end() && f->second.state == audio::renderState::done; }));
    const auto render = renderer.snapshot()->at ("CUE00001").file;

    renderer.freeze ({ "CUE00001", "ramp.wav", render });
    REQUIRE (soon ([&] { return told.load(); }));

    CHECK (problem.empty());
    CHECK (bounce == "ramp (edit).wav");
    REQUIRE (scratch.media.getChildFile ("ramp (edit).wav").existsAsFile());
    CHECK (scratch.media.getChildFile (juce::String (render)).existsAsFile());   // copied, never moved
    CHECK (readWhole (scratch.media.getChildFile ("ramp (edit).wav")).getNumSamples() == 2 * rate);
    CHECK (media.durations()->at ("ramp (edit).wav") == doctest::Approx (2.0));

    /*  A render that is not there is said so. */
    told.store (false);
    renderer.freeze ({ "CUE00001", "ramp.wav", ".edits/nothing.wav" });
    REQUIRE (soon ([&] { return told.load(); }));
    CHECK (bounce.empty());
    CHECK (problem == "the render is not there");
    CHECK (renderer.snapshot()->at ("CUE00001").problem == "the render is not there");

    renderer.stop();
}

TEST_CASE ("media.freeze: refused until the render of the edit as it now is exists; taken and ignored with no renderer")
{
    doc::ShowDocument document;
    const auto listId = document.createList ("Main").id;
    const auto cueId = document.createCue (listId, 0, "media", "Rain").id;
    REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/file", "rain.wav").ok);

    CommandRegistry registry;
    audio::registerEditRenderCommands (registry, nullptr, document);
    const auto* freeze = registry.find ("media.freeze");
    REQUIRE (freeze != nullptr);

    CommandContext context;
    CHECK (freeze->handler (context, { osc::Value::string ("NOTACUE1") }).reason == "unknown-id");
    CHECK (freeze->handler (context, { osc::Value::string (cueId) }).reason == "bad-value");   // no open edit

    REQUIRE (document.splitSection (cueId, 10.0, 30.0, {}).ok);
    CHECK (freeze->handler (context, { osc::Value::string (cueId) }).applied);   // taken and ignored

    const auto memo = document.createCue (listId, 1, "memo", "Note").id;
    CHECK (freeze->handler (context, { osc::Value::string (memo) }).reason == "type-mismatch");

    REQUIRE (document.setAttribute ("/godot/document/locked", "true").ok);
    CHECK (freeze->handler (context, { osc::Value::string (cueId) }).reason == "locked");
}

//==============================================================================
/*  A MOVIE'S EDIT ON THE SAME THREAD (namespace draft §55.5, ADV, ADW): a job
    whose source is a movie renders to a .mov, the sweep keeps it, and a freeze
    is the pair's - the movie's bounce beside its source and its sound's beside
    its own, one answer or none. */

namespace
{
    using namespace wfg::testing::hapmovie;

    /*  Three seconds of 10 fps HAP, red then green then blue, into media/. */
    juce::File writeThreeColours (const juce::File& media, const char* name = "three.mov", const char* codec = "Hap1")
    {
        std::vector<Bytes> frames;
        const std::uint32_t colours[3] { 0xFF0000, 0x00FF00, 0x0000FF };

        for (int n = 0; n < 30; ++n)
            frames.push_back (section (0xAB, solidDxt1 (16, 8, colours[n / 10])));

        return writeMovie (media, name, hapMovie (16, 8, frames, 10, codec));
    }

    bool rendered (audio::EditRenderer& renderer, const std::string& cue)
    {
        return soon ([&] { const auto s = renderer.snapshot(); const auto f = s->find (cue);
                           return f != s->end() && f->second.state == audio::renderState::done; });
    }

    std::size_t framesIn (const juce::File& movie)
    {
        video::movie::MovieFile file;
        std::string why;
        REQUIRE_MESSAGE (file.open (movie.getFullPathName().toStdString(), why), why);
        return file.info().frames.size();
    }
}

TEST_CASE ("edit renderer: a movie's job renders to a .mov on the grid, published with its length and its kind, and the sweep keeps it")
{
    Scratch scratch;
    writeThreeColours (scratch.media);
    doc::ShowDocument document;
    audio::MediaInfo media { document, scratch.media.getFullPathName().toStdString() };
    audio::EditRenderer renderer { media, scratch.media.getFullPathName().toStdString() };
    REQUIRE (renderer.start());

    const std::vector<Section> sections { piece ("B", 2.0, 3.0), piece ("A", 0.0, 1.0) };
    renderer.offer ({ "MOV00001", "three.mov", sections });
    REQUIRE (rendered (renderer, "MOV00001"));

    const auto render = renderer.snapshot()->at ("MOV00001");
    CHECK (render.kind == "movie");
    CHECK (render.file.rfind (".edits/", 0) == 0);
    CHECK (render.file.substr (render.file.size() - 4) == ".mov");
    CHECK (render.seconds == doctest::Approx (2.0));
    CHECK (render.problem.empty());
    CHECK (media.durations()->at (render.file) == doctest::Approx (2.0));
    CHECK (framesIn (scratch.media.getChildFile (juce::String (render.file))) == 20u);
    CHECK (audio::EditRenderer::readoutText (*renderer.snapshot()) == "MOV00001\tdone\t100\t\tmovie");

    /*  Beside a sound's, and the sweep keeps both and removes the rest. */
    renderer.offer (jobFor ("CUE00001", sections));
    REQUIRE (rendered (renderer, "CUE00001"));
    const auto soundRender = renderer.snapshot()->at ("CUE00001").file;
    CHECK (soundRender.substr (soundRender.size() - 4) == ".wav");
    CHECK (renderer.snapshot()->at ("CUE00001").kind == "sound");

    REQUIRE (scratch.target ("orphan.mov").replaceWithText ("x"));
    REQUIRE (scratch.target ("orphan.wav").replaceWithText ("x"));
    renderer.sweep ({ { "MOV00001", "three.mov", sections }, jobFor ("CUE00001", sections) });
    REQUIRE (soon ([&] { return ! scratch.target ("orphan.mov").exists() && ! scratch.target ("orphan.wav").exists(); }));
    CHECK (scratch.media.getChildFile (juce::String (render.file)).existsAsFile());
    CHECK (scratch.media.getChildFile (juce::String (soundRender)).existsAsFile());

    /*  A movie that is not HAP fails with the convert words, and plays nothing. */
    writeThreeColours (scratch.media, "preview.mov", "avc1");
    renderer.offer ({ "MOV00002", "preview.mov", { piece ("A", 0.0, 1.0) } });
    REQUIRE (soon ([&] { const auto s = renderer.snapshot(); const auto f = s->find ("MOV00002");
                         return f != s->end() && f->second.state == audio::renderState::failed; }));
    CHECK (renderer.snapshot()->at ("MOV00002").problem.find ("convert the movie to HAP first") != std::string::npos);
    CHECK (renderer.snapshot()->at ("MOV00002").file.empty());

    renderer.stop();
}

TEST_CASE ("edit renderer: a movie's freeze is the pair's - its bounce beside its source and its sound's beside its own, one answer or none")
{
    Scratch scratch;
    writeThreeColours (scratch.media);
    doc::ShowDocument document;
    audio::MediaInfo media { document, scratch.media.getFullPathName().toStdString() };
    audio::EditRenderer renderer { media, scratch.media.getFullPathName().toStdString() };

    std::atomic<bool> told { false };
    std::string bounce, soundBounce, problem;
    renderer.setOnFrozen ([&] (const audio::EditRenderer::FreezeJob&, const std::string& name, const std::string& soundName,
                               const std::string& why)
                          {
                              bounce = name;
                              soundBounce = soundName;
                              problem = why;
                              told.store (true);
                          });
    REQUIRE (renderer.start());

    const std::vector<Section> sections { piece ("B", 2.0, 3.0), piece ("A", 0.0, 1.0) };
    renderer.offer ({ "MOV00001", "three.mov", sections });
    renderer.offer (jobFor ("CUE00001", sections));
    REQUIRE (rendered (renderer, "MOV00001"));
    REQUIRE (rendered (renderer, "CUE00001"));
    const auto movieRender = renderer.snapshot()->at ("MOV00001").file;
    const auto soundRender = renderer.snapshot()->at ("CUE00001").file;

    renderer.freeze ({ "MOV00001", "three.mov", movieRender, "CUE00001", "ramp.wav", soundRender });
    REQUIRE (soon ([&] { return told.load(); }, 300));
    CHECK (problem.empty());
    CHECK (bounce == "three (edit).mov");
    CHECK (soundBounce == "ramp (edit).wav");
    REQUIRE (scratch.media.getChildFile ("three (edit).mov").existsAsFile());
    REQUIRE (scratch.media.getChildFile ("ramp (edit).wav").existsAsFile());
    CHECK (framesIn (scratch.media.getChildFile ("three (edit).mov")) == 20u);
    CHECK (media.durations()->at ("three (edit).mov") == doctest::Approx (2.0));
    CHECK (media.durations()->at ("ramp (edit).wav") == doctest::Approx (2.0));
    CHECK (readWhole (scratch.media.getChildFile ("ramp (edit).wav")).getNumSamples() == 2 * rate);

    /*  The sound's render missing: nothing lands, not even the movie's. */
    told.store (false);
    renderer.freeze ({ "MOV00001", "three.mov", movieRender, "CUE00001", "ramp.wav", ".edits/nothing.wav" });
    REQUIRE (soon ([&] { return told.load(); }, 300));
    CHECK (bounce.empty());
    CHECK (soundBounce.empty());
    CHECK (problem == "the sound's render is not there");
    CHECK_FALSE (scratch.media.getChildFile ("three (edit) 2.mov").exists());

    /*  A movie alone, with no sound cut in step: its own bounce, under the
        next free name. */
    told.store (false);
    renderer.freeze ({ "MOV00001", "three.mov", movieRender });
    REQUIRE (soon ([&] { return told.load(); }, 300));
    CHECK (problem.empty());
    CHECK (bounce == "three (edit) 2.mov");
    CHECK (soundBounce.empty());

    renderer.stop();
}

TEST_CASE ("media.freeze: a movie is frozen with its sound once both renders are of the edits as they now are; a sound locked to a movie says to edit the movie")
{
    Scratch scratch;
    writeThreeColours (scratch.media);

    doc::ShowDocument document;
    const auto listId = document.createList ("Main").id;
    const auto movieId = document.createCue (listId, 0, "video", "Clip").id;
    REQUIRE (document.setAttribute ("/godot/cue/" + movieId + "/source", "movie").ok);
    REQUIRE (document.setAttribute ("/godot/cue/" + movieId + "/file", "three.mov").ok);
    const auto soundId = document.createCue (listId, 1, "media", "Clip (sound)").id;
    REQUIRE (document.setAttribute ("/godot/cue/" + soundId + "/file", "ramp.wav").ok);
    REQUIRE (document.setAttribute ("/godot/cue/" + soundId + "/lockedTo", movieId).ok);

    audio::MediaInfo media { document, scratch.media.getFullPathName().toStdString() };
    audio::EditRenderer renderer { media, scratch.media.getFullPathName().toStdString() };

    std::atomic<bool> told { false };
    audio::EditRenderer::FreezeJob asked;
    std::string bounce, soundBounce;
    renderer.setOnFrozen ([&] (const audio::EditRenderer::FreezeJob& job, const std::string& name, const std::string& soundName,
                               const std::string&)
                          {
                              asked = job;
                              bounce = name;
                              soundBounce = soundName;
                              told.store (true);
                          });
    REQUIRE (renderer.start());

    CommandRegistry registry;
    audio::registerEditRenderCommands (registry, &renderer, document);
    const auto* freeze = registry.find ("media.freeze");
    REQUIRE (freeze != nullptr);
    CommandContext context;

    CHECK (freeze->handler (context, { osc::Value::string (movieId) }).reason == "bad-value");   // no edit yet

    /*  THE MOVIE CUT AND REORDERED; its sound follows. */
    REQUIRE (document.splitSection (movieId, 1.0, 3.0, {}).ok);
    const auto cut = document.sectionsOf (document.findById (movieId));
    REQUIRE (cut.size() == 2u);
    REQUIRE (document.moveSection (cut[1].id, 0).ok);
    const auto movieSections = document.sectionsOf (document.findById (movieId));
    const auto soundSections = document.sectionsOf (document.findById (soundId));
    REQUIRE (soundSections.size() == 2u);

    CHECK (freeze->handler (context, { osc::Value::string (soundId) }).reason == "locked-to-movie");
    CHECK (freeze->handler (context, { osc::Value::string (movieId) }).reason == "busy");   // nothing rendered

    renderer.offer ({ movieId, "three.mov", movieSections });
    REQUIRE (rendered (renderer, movieId));
    CHECK (freeze->handler (context, { osc::Value::string (movieId) }).reason == "busy");   // the sound's not yet

    renderer.offer ({ soundId, "ramp.wav", soundSections });
    REQUIRE (rendered (renderer, soundId));
    INFO ("movie " << movieId << ", sound " << soundId << "; after both: " << audio::EditRenderer::readoutText (*renderer.snapshot()));
    const auto outcome = freeze->handler (context, { osc::Value::string (movieId) });
    INFO ("handler: applied=" << outcome.applied << " reason=" << outcome.reason);
    CHECK (outcome.applied);

    /*  A copy of two files; thirty seconds, since a scanner may hold a new file a while. */
    const auto landed = soon ([&] { return told.load(); }, 300);
    INFO ("readout: " << audio::EditRenderer::readoutText (*renderer.snapshot()));
    INFO ("movie bounce there: " << scratch.media.getChildFile ("three (edit).mov").existsAsFile()
          << ", sound bounce there: " << scratch.media.getChildFile ("ramp (edit).wav").existsAsFile());
    REQUIRE (landed);
    CHECK (asked.cue == movieId);
    CHECK (asked.source == "three.mov");
    CHECK (asked.soundCue == soundId);
    CHECK (asked.soundSource == "ramp.wav");
    CHECK (bounce == "three (edit).mov");
    CHECK (soundBounce == "ramp (edit).wav");

    renderer.stop();
}
