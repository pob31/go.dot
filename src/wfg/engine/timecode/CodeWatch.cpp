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

#include <wfg/engine/timecode/CodeWatch.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace wfg::timecode
{
    namespace
    {
        bool startsWith (std::string_view text, std::string_view prefix) noexcept
        {
            return text.size() >= prefix.size() && text.substr (0, prefix.size()) == prefix;
        }

        /*  THE FRAME OF THE DAY NEAREST A POSITION ON THE LINE, unwrapped as
            the line is: a code running across midnight reads on, its frames
            never a day apart from the ones before them. */
        double nearest (double frameOfDay, double reference, double perDay) noexcept
        {
            auto offset = std::fmod (frameOfDay - reference, perDay);

            if (offset < -perDay / 2.0)
                offset += perDay;
            else if (offset >= perDay / 2.0)
                offset -= perDay;

            return reference + offset;
        }

        //  The rate that numbers frames as labels counting to `nominal` do, dropping or not.
        Rate countingOf (int nominal, bool drop) noexcept
        {
            if (drop)
                return Rate::fps2997df;

            return nominal <= 24 ? Rate::fps24 : nominal <= 25 ? Rate::fps25 : Rate::fps30;
        }

        //  Two rates whose labels name the same frames: 24 and 23.976, 30 and 29.97.
        bool sameCounting (Rate a, Rate b) noexcept
        {
            return nominalOf (a) == nominalOf (b) && dropsFrames (a) == dropsFrames (b);
        }

        double framesPerSecond (Rate rate) noexcept
        {
            return static_cast<double> (numOf (rate)) / static_cast<double> (denOf (rate));
        }

        double squared (double value) noexcept { return value * value; }

        //  The most observations a window keeps, whatever a second holds: MTC at four times speed and some.
        constexpr std::size_t mostPoints = 1024;
    }

    //==========================================================================
    std::string_view wordOf (CodeTurn turn) noexcept
    {
        switch (turn)
        {
            case CodeTurn::running:  return "running";
            case CodeTurn::stopped:  return "stopped";
            case CodeTurn::jumped:   break;
        }

        return "jumped";
    }

    std::optional<CodeTurn> codeTurnFromWord (std::string_view word) noexcept
    {
        if (word == "jumped")   return CodeTurn::jumped;
        if (word == "running")  return CodeTurn::running;
        if (word == "stopped")  return CodeTurn::stopped;
        return std::nullopt;
    }

    bool isCodeSource (std::string_view source) noexcept
    {
        if (source == "clock")
            return true;

        return (startsWith (source, "ltc:") || startsWith (source, "mtc:")) && source.size() > 4;
    }

    std::vector<osc::Value> codeHeardArgs (const std::string& source, const CodeSaid& said)
    {
        return { osc::Value::string (source),
                 osc::Value::string (std::string (wordOf (said.what))),
                 osc::Value::string (unitWordOf (said.map.unit)),
                 osc::Value::int64 (said.map.frame),
                 osc::Value::int64 (said.map.sample),
                 osc::Value::float64 (said.map.speed) };
    }

    std::optional<CodeHeard> readCodeHeard (const std::vector<osc::Value>& args, int sampleRate)
    {
        using namespace codeheard;

        if (args.size() <= speedArg
              || ! args[sourceArg].isString() || ! args[whatArg].isString() || ! args[rateArg].isString()
              || ! args[frameArg].isInt64() || ! args[sampleArg].isInt64() || ! args[speedArg].isFloat64())
            return std::nullopt;

        CodeHeard heard;
        heard.source = args[sourceArg].getString();

        const auto what = codeTurnFromWord (args[whatArg].getString());
        const auto unit = unitFromWord (args[rateArg].getString());

        if (! isCodeSource (heard.source) || ! what.has_value() || ! unit.has_value())
            return std::nullopt;

        //  The wall clock counts milliseconds, and nothing else does.
        if ((heard.source == "clock") != ! unit->rate.has_value())
            return std::nullopt;

        const auto frame = args[frameArg].getInt64();
        const auto speed = args[speedArg].getFloat64();

        if (frame < 0 || frame >= unit->perDay || ! std::isfinite (speed))
            return std::nullopt;

        heard.what = *what;
        heard.map.state = *what == CodeTurn::stopped ? CodeState::stopped : CodeState::running;
        heard.map.unit = *unit;
        heard.map.frame = frame;
        heard.map.sample = args[sampleArg].getInt64();
        heard.map.speed = speed;
        heard.map.sampleRate = sampleRate;
        return heard;
    }

    //==========================================================================
    CodeWatch::CodeWatch (Kind kindToUse) noexcept
        : kind (kindToUse)
    {
    }

    void CodeWatch::setSampleRate (int rate) noexcept
    {
        if (rate == sampleRate)
            return;

        sampleRate = rate;
        forget();
    }

    void CodeWatch::setShowRate (Rate rate) noexcept
    {
        showRate = rate;
    }

    void CodeWatch::setLatency (std::int64_t samples) noexcept
    {
        latency = std::max<std::int64_t> (0, samples);
    }

    void CodeWatch::setGrace (std::int64_t samples) noexcept
    {
        grace = std::max<std::int64_t> (0, samples);
    }

    void CodeWatch::forget() noexcept
    {
        haveCounting = false;
        largestField = -1;
        previousField = -1;
        knownNominal = 0;

        current = CodeState::absent;
        locked = false;
        pendingJump = false;
        jumpWhileRunning = false;

        window.clear();
        candidates.clear();
        live = {};
        written = {};

        lastHeard = -1;
        lastPoint = {};
        havePoint = false;
    }

    //==========================================================================
    void CodeWatch::restartCounting (Rate counting) noexcept
    {
        /*  FRAMES NUMBERED ANOTHER WAY ARE NOT THE LINE'S: a code first read at
            the show's 25 whose labels go on to 29 counts to 30, and every frame
            heard so far has another number. The line is dropped and the code
            read again from its next two frames - a `jumped`, the map's anchor
            and rate said afresh. Rare: once at the start, when the show's rate
            is not the code's. */
        countingRate = counting;
        haveCounting = true;
        locked = false;
        pendingJump = false;
        window.clear();
        candidates.clear();
        live = {};
    }

    void CodeWatch::noteField (int field) noexcept
    {
        largestField = std::max (largestField, field);

        /*  A SECOND'S WRAP SAYS WHAT THE LABELS COUNT TO: a frame field of
            nought beside one of 23 or more, either way round (a code read
            backwards wraps from nought to its largest). Never less than what
            has been seen since. */
        if (previousField >= 0 && ((field == 0 && previousField >= 23) || (previousField == 0 && field >= 23)))
            knownNominal = std::max (knownNominal, std::max (field, previousField) + 1);

        if (knownNominal > 0)
            knownNominal = std::max (knownNominal, largestField + 1);

        previousField = field;
    }

    Rate CodeWatch::ltcCounting (bool drop) const noexcept
    {
        if (drop)
            return Rate::fps2997df;

        if (knownNominal > 0)
            return countingOf (knownNominal, false);

        /*  NOT YET KNOWN: the show's rate while its labels allow every frame
            heard, and otherwise the least that does. */
        if (! dropsFrames (showRate) && nominalOf (showRate) > largestField)
            return countingOf (nominalOf (showRate), false);

        return countingOf (largestField < 24 ? 24 : largestField < 25 ? 25 : 30, false);
    }

    //==========================================================================
    void CodeWatch::hear (const LtcHeard& frame)
    {
        if (frame.lost || sampleRate <= 0)
            return;

        //  A LINE STARTED AGAIN on the audio side: its samples are another count's.
        if (! generationSeen || frame.generation != generation)
        {
            if (generationSeen)
                forget();

            generationSeen = true;
            generation = frame.generation;
        }

        noteField (frame.label.frames);

        if (const auto counting = ltcCounting (frame.drop); ! haveCounting || counting != countingRate)
            restartCounting (counting);

        /*  WHERE IT BEGAN, and when it had been heard whole: forwards its last
            bit's end, eighty bits on; backwards its start is its last edge. */
        const auto start = static_cast<double> (frame.startSample) + static_cast<double> (frame.startFraction);
        const auto whole = frame.reverse ? start : start + 80.0 * frame.bitPeriod;

        heardAt (static_cast<std::int64_t> (std::ceil (whole)));
        place (start, static_cast<double> (frameOf (frame.label, countingRate)));
    }

    void CodeWatch::hear (const MtcObservation& observation)
    {
        if (observation.sample < 0 || sampleRate <= 0)
            return;

        if (const auto counting = rateOfMtcCode (observation.rateCode); ! haveCounting || counting != countingRate)
            restartCounting (counting);

        heardAt (observation.sample);
        place (static_cast<double> (observation.sample),
               static_cast<double> (observation.frame) + static_cast<double> (observation.quarter) / 4.0);
    }

    void CodeWatch::heardAt (std::int64_t sample) noexcept
    {
        lastHeard = std::max (lastHeard, sample);
    }

    //==========================================================================
    void CodeWatch::place (double sample, double frameOfDay)
    {
        const auto perDay = static_cast<double> (framesPerDay (countingRate));

        if (locked && live.valid)
        {
            const auto predicted = live.positionAt (sample);
            const auto position = nearest (frameOfDay, predicted, perDay);

            //  ON THE LINE: believed, and any frame waiting off it was a glitch.
            if (std::abs (position - predicted) <= locateFrames)
            {
                window.push_back ({ sample, position });
                lastPoint = window.back();
                havePoint = true;
                candidates.clear();
                return;
            }

            candidate ({ sample, position });
            return;
        }

        const auto reference = candidates.empty() ? frameOfDay : candidates.back().position;
        candidate ({ sample, nearest (frameOfDay, reference, perDay) });
    }

    void CodeWatch::candidate (Point point)
    {
        /*  TWO FRAMES THAT AGREE (AIR): the later further on than the first by
            a speed a code can have - a quarter of nominal to four times it -
            and each step the same way. One that does not agree with what waits
            starts the wait again: what waited was the glitch. */
        if (! candidates.empty())
        {
            const auto& first = candidates.front();
            const auto elapsed = point.sample - first.sample;
            const auto moved = point.position - first.position;
            const auto implied = elapsed > 0.0 ? std::abs (moved) / (elapsed * nominalSlope (countingRate)) : 0.0;
            const auto step = point.position - candidates.back().position;

            const auto agrees = elapsed > 0.0 && implied >= slowest && implied <= fastest
                                  && (candidates.size() < 2 || step * moved > 0.0);

            if (! agrees)
                candidates.clear();
        }

        candidates.push_back (point);

        if (candidates.size() >= 2 && std::abs (candidates.back().position - candidates.front().position) >= 1.0 - 1.0e-9)
            confirm();
    }

    void CodeWatch::confirm()
    {
        //  A LOCATE WHILE IT RAN keeps the speed it ran at, if the new frames allow it (`sayJump`).
        jumpWhileRunning = locked && written.valid && written.map.state == CodeState::running;

        window.assign (candidates.begin(), candidates.end());
        lastPoint = window.back();
        havePoint = true;
        candidates.clear();
        locked = true;
        pendingJump = true;
        fit();
    }

    //==========================================================================
    void CodeWatch::trim()
    {
        if (window.empty())
            return;

        const auto horizon = window.back().sample - windowSeconds * static_cast<double> (sampleRate);

        while (window.size() > 2 && (window.front().sample < horizon || window.size() > mostPoints))
            window.pop_front();

        //  And a frame that waited too long for a second one is no candidate.
        if (! candidates.empty() && candidates.front().sample < horizon)
            candidates.clear();
    }

    void CodeWatch::fit()
    {
        live = {};

        const auto count = window.size();

        if (count < 2)
            return;

        /*  ABOUT THE NEWEST POINT, so the sums are of small numbers however
            many hours of samples the clock has counted. */
        const auto& reference = window.back();
        auto sumX = 0.0;
        auto sumY = 0.0;

        for (const auto& point : window)
        {
            sumX += point.sample - reference.sample;
            sumY += point.position - reference.position;
        }

        const auto n = static_cast<double> (count);
        const auto meanX = sumX / n;
        const auto meanY = sumY / n;
        auto spread = 0.0;
        auto together = 0.0;

        for (const auto& point : window)
        {
            const auto dx = point.sample - reference.sample - meanX;
            const auto dy = point.position - reference.position - meanY;
            spread += dx * dx;
            together += dx * dy;
        }

        if (! (spread > 0.0))
            return;

        const auto slope = together / spread;
        auto leftOver = 0.0;

        for (const auto& point : window)
        {
            const auto dx = point.sample - reference.sample - meanX;
            const auto dy = point.position - reference.position - meanY;
            leftOver += squared (dy - slope * dx);
        }

        live.valid = true;
        live.sample = reference.sample;
        live.position = reference.position + meanY - slope * meanX;
        live.slope = slope;
        live.meanSample = reference.sample + meanX;
        live.spread = spread;
        live.count = count;
        live.residual = count >= 3 ? std::sqrt (leftOver / (n - 2.0)) : -1.0;
    }

    double CodeWatch::sigmaFrames() const noexcept
    {
        const auto perSample = std::abs (live.slope);
        const auto rate = static_cast<double> (sampleRate);
        const auto assumed = (kind == Kind::ltc ? ltcJitterSamples : mtcJitterSeconds * rate) * perSample;
        const auto least = (kind == Kind::ltc ? ltcJitterFloorSamples : mtcJitterFloorSeconds * rate) * perSample;

        if (live.residual < 0.0)
            return assumed;

        return std::max (live.residual, least);
    }

    double CodeWatch::slopeError() const noexcept
    {
        return live.spread > 0.0 ? sigmaFrames() / std::sqrt (live.spread) : 0.0;
    }

    double CodeWatch::positionError (double atSample) const noexcept
    {
        if (! live.valid || live.count == 0 || ! (live.spread > 0.0))
            return 0.0;

        return sigmaFrames() * std::sqrt (1.0 / static_cast<double> (live.count)
                                            + squared (atSample - live.meanSample) / live.spread);
    }

    double CodeWatch::jitterSamples() const noexcept
    {
        if (! live.valid || live.residual < 0.0 || ! (std::abs (live.slope) > 0.0))
            return 0.0;

        return live.residual / std::abs (live.slope);
    }

    //==========================================================================
    double CodeWatch::nominalSlope (Rate rate) const noexcept
    {
        return sampleRate > 0 ? framesPerSecond (rate) / static_cast<double> (sampleRate) : 0.0;
    }

    Rate CodeWatch::rateFor (double slope, double error) const noexcept
    {
        const auto measured = std::abs (slope) * static_cast<double> (sampleRate);
        const auto writtenRate = written.valid ? written.map.unit.rate : std::nullopt;
        const auto keepsWritten = writtenRate.has_value() && sameCounting (*writtenRate, countingRate);

        /*  WHAT WAS WRITTEN STAYS while the frames still run within 600 parts
            per million of it - a hundred more than `detectRate` allows - so a
            code half way between 24 and 23.976 is not said twice a second. */
        if (keepsWritten && std::abs (measured / framesPerSecond (*writtenRate) - 1.0) < 600.0e-6)
            return *writtenRate;

        /*  THE PERIOD TELLS 23.976 FROM 24 AND 29.97 FROM 30 (AIL), once the
            labels have shown what they count to and the speed is known to a
            hundred parts per million. */
        const auto counts = kind == Kind::mtc || knownNominal > 0;

        if (counts && std::abs (slope) > 0.0 && error <= 1.0e-4 * std::abs (slope))
            if (const auto detected = detectRate (nominalOf (countingRate) - 1, dropsFrames (countingRate),
                                                  1.0 / std::abs (slope), sampleRate))
                return *detected;

        if (keepsWritten)
            return *writtenRate;

        if (sameCounting (showRate, countingRate))
            return showRate;

        return countingRate;
    }

    double CodeWatch::snapped (double slope, double error, Rate rate) const noexcept
    {
        const auto perUnit = nominalSlope (rate);

        if (! (perUnit > 0.0))
            return slope;

        const auto speed = slope / perUnit;

        if (std::abs (speed - 1.0) <= std::max (unitTolerance, 3.0 * error / perUnit))
            return perUnit;

        return slope;
    }

    //==========================================================================
    CodeSaid CodeWatch::shifted (const CodeMap& raw, CodeTurn turn) const
    {
        CodeSaid said;
        said.what = turn;
        said.map = raw;
        said.map.sample -= written.latency;
        return said;
    }

    CodeSaid CodeWatch::say (CodeTurn turn, double slope, Rate rate)
    {
        /*  THE ANCHOR: the whole frame the code is in at the newest point (the
            one it last passed, backwards), and the sample it began on - the
            first at or after the moment, on the line about to be written
            through the code's place now. */
        const auto& newest = window.back();
        const auto here = live.positionAt (newest.sample);
        const auto whole = slope >= 0.0 ? std::floor (here + 1.0e-9) : std::ceil (here - 1.0e-9);
        const auto began = std::abs (slope) > 0.0 ? newest.sample + (whole - here) / slope : newest.sample;
        const auto unit = unitOf (rate);

        written.valid = true;
        written.map.state = CodeState::running;
        written.map.unit = unit;
        written.map.frame = codemap::wrapped (static_cast<std::int64_t> (whole), unit.perDay);
        written.map.sample = static_cast<std::int64_t> (std::ceil (began - 1.0e-6));
        written.map.speed = slope / nominalSlope (rate);
        written.map.sampleRate = sampleRate;
        written.position = whole;
        written.began = began;
        written.slope = slope;
        written.positionError = positionError (newest.sample);
        written.latency = latency;

        current = CodeState::running;
        return shifted (written.map, turn);
    }

    CodeSaid CodeWatch::sayJump()
    {
        pendingJump = false;
        trim();
        fit();

        const auto error = slopeError();
        auto slope = live.slope;

        /*  A LOCATE KEEPS THE SPEED THE CODE RAN AT when its two new frames
            cannot tell it from that - a desk jumping while it plays plays on at
            its own speed, and two frames know it to forty parts per million
            where a minute of running knew it to a fraction of one. */
        if (jumpWhileRunning && written.slope * slope > 0.0 && std::abs (slope - written.slope) <= 3.0 * error)
            slope = written.slope;

        jumpWhileRunning = false;

        const auto rate = rateFor (slope, error);
        return say (CodeTurn::jumped, snapped (slope, error, rate), rate);
    }

    CodeSaid CodeWatch::sayStopped()
    {
        /*  WHERE IT STOPPED: the last frame believed - its whole frame and the
            sample it began on - standing from then on. */
        auto position = written.position;
        auto began = static_cast<double> (written.map.sample);
        const auto slope = live.valid ? live.slope : written.slope;

        if (havePoint)
        {
            position = slope >= 0.0 ? std::floor (lastPoint.position + 1.0e-9) : std::ceil (lastPoint.position - 1.0e-9);
            began = std::abs (slope) > 0.0 ? lastPoint.sample + (position - lastPoint.position) / slope : lastPoint.sample;
        }

        const auto unit = written.valid ? written.map.unit : unitOf (countingRate);

        written.valid = true;
        written.map.state = CodeState::stopped;
        written.map.unit = unit;
        written.map.frame = codemap::wrapped (static_cast<std::int64_t> (position), unit.perDay);
        written.map.sample = static_cast<std::int64_t> (std::ceil (began - 1.0e-6));
        written.map.speed = 0.0;
        written.map.sampleRate = sampleRate;
        written.position = position;
        written.began = began;
        written.slope = 0.0;
        written.positionError = 0.0;
        written.latency = latency;

        current = CodeState::stopped;
        locked = false;
        pendingJump = false;
        window.clear();
        candidates.clear();
        live = {};
        havePoint = false;

        return shifted (written.map, CodeTurn::stopped);
    }

    //==========================================================================
    std::optional<CodeSaid> CodeWatch::update (std::int64_t now)
    {
        if (sampleRate <= 0)
            return std::nullopt;

        //  APPEARED, CAME BACK, OR LOCATED: two frames agreed since the last tick.
        if (pendingJump)
            return sayJump();

        if (current != CodeState::running)
            return std::nullopt;

        /*  STOPPED (AIR): nothing heard for four frames beyond the one due
            next - LTC's next frame ends a frame after the last, MTC's next
            piece a quarter of one - and the time a code takes to reach the
            tick. Frames at the speed the code ran at, never slower than a
            quarter of nominal. */
        {
            const auto nominal = nominalSlope (countingRate);
            const auto slope = live.valid ? std::abs (live.slope) : std::abs (written.slope);
            const auto perFrame = nominal > 0.0 ? 1.0 / std::max (slope, nominal * slowest) : 0.0;
            const auto due = (kind == Kind::ltc ? 1.0 : 0.25) + freewheelFrames;

            if (lastHeard >= 0 && static_cast<double> (now - lastHeard) > due * perFrame + static_cast<double> (grace))
                return sayStopped();
        }

        if (! locked || window.size() < 2)
            return std::nullopt;

        trim();
        fit();

        if (! live.valid)
            return std::nullopt;

        /*  HOW FAR THE CODE IS FROM THE LINE LAST WRITTEN, at the newest point,
            in the code's own seconds. */
        const auto newest = window.back().sample;
        const auto here = live.positionAt (newest);
        const auto there = written.position + written.slope * (newest - static_cast<double> (written.map.sample));
        const auto drift = here - there;
        const auto writtenRate = written.map.unit.rate.value_or (countingRate);
        const auto driftSeconds = std::abs (drift) / framesPerSecond (writtenRate);

        /*  THE SPEED WORTH WRITING: the last second's, or - when they agree
            within their errors - that weighed with the speed shown since the
            line was written, which is as good as the time it covers is long.
            Measured from where the line put the anchor, not from the sample it
            was rounded up to, whose part of a sample would otherwise read as
            speed over a short span. */
        auto slope = live.slope;
        auto error = slopeError();
        const auto since = newest - written.began;

        if (since > 0.0)
        {
            const auto fromLine = here - (written.position + written.slope * since);
            const auto longSlope = written.slope + fromLine / since;
            const auto longError = std::sqrt (squared (positionError (newest)) + squared (written.positionError)) / since;

            if (std::abs (slope - longSlope) <= 3.0 * std::sqrt (squared (error) + squared (longError)))
            {
                if (! (longError > 0.0))
                {
                    slope = longSlope;
                    error = 0.0;
                }
                else if (error > 0.0)
                {
                    const auto weightFit = 1.0 / squared (error);
                    const auto weightLong = 1.0 / squared (longError);
                    slope = (slope * weightFit + longSlope * weightLong) / (weightFit + weightLong);
                    error = 1.0 / std::sqrt (weightFit + weightLong);
                }
            }
        }

        const auto rate = rateFor (slope, error);
        const auto rateMoved = ! written.map.unit.rate.has_value() || *written.map.unit.rate != rate;
        const auto limit = kind == Kind::ltc ? ltcDriftSeconds : mtcDriftSeconds;

        if (driftSeconds > limit || rateMoved || latency != written.latency)
            return say (CodeTurn::running, snapped (slope, error, rate), rate);

        return std::nullopt;
    }

    std::optional<CodeSaid> CodeWatch::resay()
    {
        if (sampleRate <= 0)
            return std::nullopt;

        if (pendingJump)
            return sayJump();

        if (current == CodeState::running && locked && window.size() >= 2)
        {
            trim();
            fit();

            if (live.valid)
            {
                /*  ITS OWN LINE, said in full: the speed it wrote last, which is
                    the best it knows, unless the last second disagrees with it. */
                const auto error = slopeError();
                auto slope = live.slope;

                if (written.valid && written.slope * slope > 0.0 && std::abs (slope - written.slope) <= 3.0 * error)
                    slope = written.slope;

                const auto rate = rateFor (slope, error);
                return say (CodeTurn::jumped, snapped (slope, error, rate), rate);
            }
        }

        if (current == CodeState::running && written.valid)
            return shifted (written.map, CodeTurn::jumped);

        if (current == CodeState::stopped && written.valid)
            return shifted (written.map, CodeTurn::stopped);

        return std::nullopt;
    }
}
