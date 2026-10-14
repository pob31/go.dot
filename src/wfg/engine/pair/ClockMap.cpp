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

#include <wfg/engine/pair/ClockMap.h>

#include <wfg/engine/clock/TickClock.h>
#include <wfg/engine/timecode/LineFit.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace wfg::pair
{
    //==========================================================================
    double SoundLine::nanosAt (double atSample) const noexcept
    {
        return static_cast<double> (nanos) + (atSample - static_cast<double> (sample)) * nanosPerSample;
    }

    double SoundLine::sampleAt (double atNanos) const noexcept
    {
        if (! (nanosPerSample > 0.0))
            return static_cast<double> (sample);

        return static_cast<double> (sample) + (atNanos - static_cast<double> (nanos)) / nanosPerSample;
    }

    std::int64_t SoundLine::sampleZeroNanos() const noexcept
    {
        return valid ? static_cast<std::int64_t> (std::llround (nanosAt (0.0))) : std::int64_t { 0 };
    }

    //==========================================================================
    bool SoundLineFit::feed (const BlockStamp::Reading& stamp, int rate)
    {
        if (rate <= 0 || stamp.sample < 0)
            return false;

        /*  ANOTHER CLOCK - its rate moved, or its count went back, which an
            audio side opened again does: everything known of the old one is
            of another count, and is let go. A block longer than a tick is
            read on two ticks and fed once. */
        if (rate != rateFed || (lastSample >= 0 && stamp.sample < lastSample))
        {
            forget();
            rateFed = rate;
        }

        if (stamp.sample == lastSample)
            return false;

        lastSample = stamp.sample;

        if (fed == 0)
            firstMillisecond = static_cast<double> (stamp.nanos) * 1.0e-6;

        ++fed;

        const auto slipped = count.feed (stamp.sample, stamp.nanos, rate);

        //  A SLIP starts the line again from the stamp that told it, at the slope it had.
        if (slipped)
        {
            fed = 1;
            firstMillisecond = static_cast<double> (stamp.nanos) * 1.0e-6;
        }

        return slipped;
    }

    void SoundLineFit::forget() noexcept
    {
        count.forget();
        lastSample = -1;
        rateFed = 0;
        fed = 0;
        firstMillisecond = 0.0;
    }

    SoundLine SoundLineFit::line() const
    {
        SoundLine out;
        const auto& fit = count.line();

        //  THREE STAMPS, so their scatter about the line is known and with it the line's error.
        if (! count.known() || ! fit.valid || fit.residual < 0.0)
            return out;

        auto slopeError = 0.0;
        const auto perMillisecond = count.samplesPerMillisecond (slopeError);

        if (! (perMillisecond > 0.0))
            return out;

        out.nanosPerSample = 1.0e6 / perMillisecond;

        /*  THE ANCHOR, the whole sample nearest the stamps' middle - where a
            line through them is surest whatever its slope - and the moment the
            line puts it at. Whole numbers on the wire, and nothing lost. */
        const auto middle = static_cast<std::int64_t> (std::llround (fit.meanSample * 1.0e6));
        out.sample = static_cast<std::int64_t> (std::llround (count.sampleAt (middle)));
        out.nanos = static_cast<std::int64_t> (std::llround (count.momentAt (static_cast<double> (out.sample))));

        /*  ITS ERROR AT ITS NEWEST STAMP, LineFit's: the stamps' scatter over
            the root of their number, and the slope's error carried out from
            their middle - in samples, read as nanoseconds. */
        const auto newest = count.newestMillisecond();
        out.errorNanos = fit.positionError (newest, fit.residual) * out.nanosPerSample;
        out.seconds = std::max (0.0, (newest - firstMillisecond) / 1000.0);
        out.rate = rateFed;
        out.valid = true;
        return out;
    }

    //==========================================================================
    void HostOffset::add (const HostSample& sample)
    {
        /*  THE TRIP, there and back less the other machine's own time between
            the two: a millisecond below nought is two clocks read a hair
            apart and is nought; more, or a trip past a second, is not one of
            this link's. */
        const auto trip = static_cast<double> ((sample.back - sample.sent) - (sample.left - sample.received));

        if (! (trip >= -1.0e6) || trip > longestTripSeconds * 1.0e9)
            return;

        const auto offset = (static_cast<double> (sample.received - sample.sent)
                               + static_cast<double> (sample.left - sample.back)) / 2.0;

        kept.push_back ({ sample.sent + (sample.back - sample.sent) / 2, offset, std::max (0.0, trip) });

        while (kept.size() > 1 && static_cast<double> (kept.back().at - kept.front().at) > windowSeconds * 1.0e9)
            kept.pop_front();

        refit();
    }

    void HostOffset::forget() noexcept
    {
        kept.clear();
        fitted = {};
    }

    void HostOffset::refit()
    {
        fitted = {};

        if (kept.empty())
            return;

        const auto fastestOf = std::min_element (kept.begin(), kept.end(),
                                                 [] (const Kept& a, const Kept& b) { return a.trip < b.trip; });

        /*  THE FASTEST EXCHANGE OF EACH SECOND: a slower one waited in a queue
            somewhere, and a queue is on one way and not the other - the half
            of its wait it adds to the offset is not noise about the line but a
            lean away from it. ONLY A SECOND OF THREE EXCHANGES OR MORE
            (`fewestPerSecond`): the second still filling, or the one the
            window's edge cut, may hold one exchange and that one queued - and
            at the newest end of the line it is the lever every moment read
            ahead of it turns on. Found by measuring: with every second taken,
            the map was out by up to half a millisecond a few seconds in; with
            three, by ten microseconds over ten minutes (SameSampleTests). */
        std::map<std::int64_t, std::pair<const Kept*, int>> fastest;

        for (const auto& one : kept)
        {
            const auto second = one.at >= 0 ? one.at / 1'000'000'000 : (one.at - 999'999'999) / 1'000'000'000;
            auto& slot = fastest[second];
            ++slot.second;

            if (slot.first == nullptr || one.trip < slot.first->trip)
                slot.first = &one;
        }

        std::erase_if (fastest, [] (const auto& entry) { return entry.second.second < fewestPerSecond; });

        fitted.valid = true;
        fitted.samples = static_cast<int> (kept.size());
        fitted.bestTripNanos = fastestOf->trip;

        const auto halfTrip = fastestOf->trip / 2.0;

        //  A FEW SECONDS: the fastest exchange, as it was, the clocks taken not to drift yet.
        const auto alone = [this, fastestOf, halfTrip]
        {
            fitted.at = fastestOf->at;
            fitted.offset = fastestOf->offset;
            fitted.slope = 0.0;
            fitted.errorNanos = halfTrip;
        };

        if (fastest.size() < 3)
        {
            alone();
            return;
        }

        std::deque<timecode::linefit::Point> points;

        for (const auto& entry : fastest)
            points.push_back ({ static_cast<double> (entry.second.first->at) * 1.0e-6, entry.second.first->offset });

        const auto fit = timecode::linefit::through (points);

        if (! fit.valid)
        {
            alone();
            return;
        }

        /*  THE SLOPE WEIGHED WITH NOUGHT - two host clocks' nominal drift - by
            how well each is known, as CountLine weighs a crystal's: a few
            seconds of exchanges say little of a drift of parts per million,
            and two minutes say it to a fraction of one. In nanoseconds a
            millisecond: a hundred parts per million is a hundred. */
        const auto sigma = fit.residual >= 0.0 ? fit.residual : halfTrip;
        const auto fitError = fit.slopeError (sigma);
        const auto priorError = priorPartsPerMillion;
        const auto weights = fitError * fitError + priorError * priorError;
        const auto slope = weights > 0.0 ? fit.slope * priorError * priorError / weights : fit.slope;

        const auto newest = points.back().sample;
        fitted.at = static_cast<std::int64_t> (std::llround (newest * 1.0e6));
        fitted.offset = fit.positionAt (fit.meanSample) + slope * (newest - fit.meanSample);
        fitted.slope = slope * 1.0e-6;

        /*  ITS ERROR: the line's at its newest, and half the fastest trip -
            what an asymmetry between the two ways could hide, which no number
            of exchanges can see. */
        const auto placeError = fit.positionError (newest, sigma);
        fitted.errorNanos = std::sqrt (placeError * placeError + halfTrip * halfTrip);
    }

    //==========================================================================
    ClockMap ClockMap::compose (const SoundLine& leader, const SoundLine& follower, const OffsetLine& hosts,
                                MeasuredBy measuredBy, bool wasTrusted)
    {
        ClockMap map;
        map.leaderLine = leader;
        map.followerLine = follower;

        if (! leader.valid || ! follower.valid || ! hosts.valid || leader.rate <= 0 || follower.rate <= 0
              || ! (leader.nanosPerSample > 0.0) || ! (follower.nanosPerSample > 0.0)
              || ! std::isfinite (hosts.offset) || ! std::isfinite (hosts.slope) || ! (hosts.slope > -0.5))
            return map;

        /*  THE HOST CLOCKS, as this machine's moment from the leader's. The
            follower measured the leader's clock less its own at its own
            moment; the leader, this machine's less its own at its. */
        const auto at = static_cast<double> (hosts.at);

        if (measuredBy == MeasuredBy::follower)
        {
            map.followerAnchor = at;
            map.leaderAnchor = at + hosts.offset;
            map.ratio = 1.0 / (1.0 + hosts.slope);
        }
        else
        {
            map.leaderAnchor = at;
            map.followerAnchor = at + hosts.offset;
            map.ratio = 1.0 + hosts.slope;
        }

        /*  THE DRIFT: this machine's samples a leader's sample, against what
            the two rates say it should be. */
        const auto perLeaderSample = map.ratio * leader.nanosPerSample / follower.nanosPerSample;
        const auto nominal = static_cast<double> (follower.rate) / static_cast<double> (leader.rate);
        map.drift = (perLeaderSample / nominal - 1.0) * 1.0e6;

        map.residual = std::sqrt (leader.errorNanos * leader.errorNanos + follower.errorNanos * follower.errorNanos
                                  + hosts.errorNanos * hosts.errorNanos)
                     / 1000.0;
        map.composed = true;

        const auto bar = wasTrusted ? untrustedMicroseconds : trustedMicroseconds;

        map.isTrusted = hosts.samples >= fewestExchanges && std::abs (map.drift) <= mostPartsPerMillion
                          && std::isfinite (map.residual) && map.residual <= bar;
        return map;
    }

    double ClockMap::followerSampleOf (double leaderSample) const noexcept
    {
        if (! composed)
            return leaderSample;

        const auto leaderMoment = leaderLine.nanosAt (leaderSample);
        const auto here = followerAnchor + ratio * (leaderMoment - leaderAnchor);
        return followerLine.sampleAt (here);
    }

    double ClockMap::leaderSampleOf (double followerSample) const noexcept
    {
        if (! composed)
            return followerSample;

        const auto here = followerLine.nanosAt (followerSample);
        const auto leaderMoment = leaderAnchor + (here - followerAnchor) / ratio;
        return leaderLine.sampleAt (leaderMoment);
    }

    std::int64_t ClockMap::onThisClock (std::int64_t leaderSample) const noexcept
    {
        return static_cast<std::int64_t> (std::llround (followerSampleOf (static_cast<double> (leaderSample))));
    }

    std::int64_t ClockMap::sampleOnThisClock (std::int64_t leaderSample, std::int64_t byTicks, int sampleRate) const noexcept
    {
        if (isTrusted)
            return onThisClock (leaderSample);

        /*  THE RECORD CLOCK: a tick is a fiftieth of a second's samples,
            exactly, at every rate a show can run at (TickClock.h); a rate that
            is not a sound clock's moves nothing rather than a guess. */
        if (sampleRate <= 0)
            return leaderSample;

        return leaderSample + byTicks * static_cast<std::int64_t> (sampleRate) / TickClock::rateHz;
    }
}
