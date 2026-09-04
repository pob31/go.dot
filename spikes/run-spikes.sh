#!/usr/bin/env bash
# This file is part of Go.dot — https://github.com/pob31/go.dot
#
# Copyright (C) 2026 Pierre-Olivier Boulant
#
# Go.dot is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version. Go.dot is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
# (LICENSE, at the repository root) for more details.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the deterministic PRD 6.1 spikes over a grid of parameters.
# THROWAWAY (devplan:19). This script lives in spikes/, not scripts/, so that
# `rm -rf spikes/` takes it with the rest of Phase 0's throwaway code.
#
# CI runs exactly the script a human runs, the same way scripts/install-linux-deps.sh
# is invoked by both. A grid that exists in two places forks, and the CI copy is
# the one nobody reads.
#
# ---------------------------------------------------------------------------
# THE NUMBERS IN THIS FILE, AND WHY THEY ARE ALLOWED TO BE HERE
# ---------------------------------------------------------------------------
# cmake/WfgOptions.cmake:26-32 and README.md promise that the default fixed track
# count and the target sample rates / buffer sizes appear NOWHERE in this tree,
# because a value written down becomes an answer to a question devplan:49-50
# reserves for the author. This script writes numbers. That is deliberate, and
# the two kinds are not the same kind:
#
#   * SAMPLE RATE and BUFFER SIZE below are a TRANSCRIBED DECISION. The author
#     asked for both to be swept - 48k and 96k, buffers 32 through 256 - so these
#     lines are a record of what he chose to measure, not a default the build
#     reads back out.
#
#   * The TRACK LADDER is a PROBE RANGE, not a configured value. 8/16/32/64 are
#     the points measured, chosen to find where behaviour changes. The default
#     fixed track count remains OPEN (devplan:49) and is the author's to take
#     after reading docs/spikes/spike04-graph-stability.md.
#
# Nothing here reaches cmake/, no option(), no preset, no constexpr. A spike
# still refuses to run without all three on argv.
#
# ---------------------------------------------------------------------------
# WHAT IT ASSERTS, AND WHAT IT DOES NOT
# ---------------------------------------------------------------------------
# A spike's exit code covers the MECHANICALLY CHECKABLE half of its criterion:
# did the graph rebuild, did the transient land within tolerance, were there
# xruns. The JUDGEMENT half - what the numbers mean, where the model's edge is -
# is a paragraph a human writes in docs/spikes/. That is why spikes still
# register no add_test() and why this is a plain workflow step.
#
# Timing figures from a Debug CI runner are not the numbers. CI gates invariants;
# the author measures on his own box with --full and a Release build.

set -uo pipefail

usage() {
    cat >&2 <<'USAGE'
run-spikes.sh - run the deterministic PRD 6.1 spikes over a parameter grid

usage: run-spikes.sh (--ci | --full | --selftest) <bindir>

  --ci        small grid, short runs. What CI executes on every push.
  --full      the author's sweep: {48k,96k} x {32,64,128,256} x the track ladder.
              Run this in Release, on the machine whose numbers you intend to quote.
  --selftest  no engine work: asserts every spike exits 2 with no arguments,
              i.e. that the "required, no defaults" contract still holds.

  <bindir>    directory holding the spike executables.

Exit: 0 all invariants held, 1 at least one spike reported a violation,
      2 usage, 3 a spike could not measure (see its own output).
USAGE
    exit 2
}

[ $# -eq 2 ] || usage
MODE="$1"
BINDIR="$2"
[ -d "$BINDIR" ] || { echo "run-spikes.sh: no such directory: $BINDIR" >&2; exit 2; }

# Windows builds produce .exe; Linux and macOS do not.
spike_path() {
    if [ -x "$BINDIR/$1" ]; then echo "$BINDIR/$1"
    elif [ -x "$BINDIR/$1.exe" ]; then echo "$BINDIR/$1.exe"
    else echo ""; fi
}

# Only spikes that are DETERMINISTIC belong here: they run entirely through TE's
# hosted audio device with autoInitialiseDeviceManager() false, so they open no
# hardware and produce the same numbers on a runner as on a desk.
#
# Deliberately absent, and each for a stated reason:
#   MTC      needs a real or virtual MIDI port (PRD 6.1 "Also verify")
#
# spike06 IS here: it measures PDC through the hosted device, which needs no
# hardware. Its --use-rack live-input path against a real interface is the half
# this grid does not reach.
DETERMINISTIC_SPIKES="spike04_graph_stability spike02_launch_offset spike01_bus_routing spike03_join_quality spike05_param_50hz spike06_rack_latency_pdc spike07_proxy_plugin"

# --- selftest: the argument contract, no engine involved --------------------
if [ "$MODE" = "--selftest" ]; then
    rc=0
    for s in $DETERMINISTIC_SPIKES; do
        exe="$(spike_path "$s")"
        if [ -z "$exe" ]; then
            echo "  MISSING  $s"; rc=1; continue
        fi
        "$exe" >/dev/null 2>&1
        got=$?
        if [ "$got" -eq 2 ]; then
            echo "  ok       $s exits 2 with no arguments"
        else
            echo "  FAIL     $s exited $got with no arguments, expected 2"
            echo "           The three required arguments have grown a default, which"
            echo "           silently answers devplan:49-50 on the author's behalf."
            rc=1
        fi
    done
    exit $rc
fi

case "$MODE" in
    --ci)   RATES="48000 96000"; BUFFERS="64 256";           TRACKS="8" ;;
    --full) RATES="48000 96000"; BUFFERS="32 64 128 256";    TRACKS="8 16 32 64" ;;
    *)      usage ;;
esac

overall=0
runs=0

for s in $DETERMINISTIC_SPIKES; do
    exe="$(spike_path "$s")"

    if [ -z "$exe" ]; then
        echo "run-spikes.sh: $s not built in $BINDIR - skipping" >&2
        continue
    fi

    for sr in $RATES; do
        for buf in $BUFFERS; do
            for tr in $TRACKS; do
                runs=$((runs + 1))
                printf '=== %s  sr=%s buf=%s tracks=%s ===\n' "$s" "$sr" "$buf" "$tr"

                "$exe" --tracks="$tr" --sample-rate="$sr" --buffer="$buf" --validate-instrument
                rc=$?

                case $rc in
                    0) ;;
                    1) echo "  ^ INVARIANT VIOLATED - this is a finding, not a flake"; overall=1 ;;
                    2) echo "  ^ usage error: the spike rejected its own grid arguments"; overall=1 ;;
                    3) echo "  ^ could not measure (see VERDICT line); not an engine verdict"
                       [ $overall -eq 0 ] && overall=3 ;;
                    *) echo "  ^ unexpected exit $rc"; overall=1 ;;
                esac
            done
        done
    done
done

echo

# Measuring nothing is not success. Multi-config generators put binaries under
# <bindir>/Debug or <bindir>/Release, so an almost-right bindir finds no
# executables at all - and without this guard that exits 0 and CI goes green
# having run no spike whatsoever.
if [ "$runs" -eq 0 ]; then
    echo "run-spikes.sh: NO SPIKE RAN. Found no executables in $BINDIR." >&2
    echo "               A run that measures nothing is not a pass." >&2
    exit 3
fi

echo "run-spikes.sh: $runs run(s), exit $overall"
exit $overall
