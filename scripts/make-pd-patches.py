#!/usr/bin/env python3
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
"""Writes Go.dot's ready-made Pd patches (pd/go.*.pd) and their help patches
(namespace draft §51, ACU).

Boxes are named and joined by name, so the numbering Pd reads is made here,
not by hand. Box text is written as typed in a box; the escapes Pd writes
for a dollar, a comma and a semicolon are added here. Vanilla objects only.
The pd/ folder is what ships and what tests/ProcessTests.cpp plays; run this
after changing a patch here, and commit both:

    python scripts/make-pd-patches.py

Standard library only.
"""
import io
import os
import re
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pd')


def pd_escape(text):
    text = text.replace('$', '\\$')
    text = re.sub(r'\s*,\s*', ' \\\\, ', text)
    text = re.sub(r'\s*;\s*', ' \\\\; ', text)
    return ' '.join(text.split())


class Patch:
    def __init__(self, w=640, h=480):
        self.w, self.h = w, h
        self.records = []
        self.names = {}
        self.lines = []

    def _add(self, name, record):
        if name is not None:
            assert name not in self.names, name
            self.names[name] = len(self.records)
        self.records.append(record)

    def obj(self, name, x, y, text):
        self._add(name, f'#X obj {x} {y} {pd_escape(text)};')

    def msg(self, name, x, y, text):
        self._add(name, f'#X msg {x} {y} {pd_escape(text)};')

    def atom(self, name, x, y, width=5):
        self._add(name, f'#X floatatom {x} {y} {width} 0 0 0 - - - 0;')

    def text(self, x, y, words):
        self._add(None, f'#X text {x} {y} {pd_escape(words)};')

    def c(self, a, outlet, b, inlet):
        self.lines.append(f'#X connect {self.names[a]} {outlet} {self.names[b]} {inlet};')

    def write(self, file):
        notice = f'#X text 10 {self.h - 26} ' + pd_escape('Part of Go.dot - https://github.com/pob31/go.dot - GPL-3.0-or-later, Copyright (C) 2026 Pierre-Olivier Boulant') + ';'
        body = [f'#N canvas 0 50 {self.w} {self.h} 12;'] + self.records + [notice] + self.lines
        with io.open(os.path.join(OUT, file), 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(body) + '\n')


# =============================================================================
#  go.avg and go.minmax: a window of the last N values, or of the last N ms.
# =============================================================================

def window(name, about):
    p = Patch(900, 720)
    p.text(10, 10, about)
    p.obj('in', 10, 50, 'inlet')
    p.obj('inR', 640, 50, 'inlet')
    p.obj('route', 10, 80, 'route reset float')
    p.obj('seq', 10, 110, 't b b b f')
    p.obj('xSet', 200, 140, 'v $0-x')
    p.obj('timer', 140, 140, 'timer')
    p.obj('nowSet', 140, 170, 'v $0-now')
    p.c('in', 0, 'route', 0)
    p.c('route', 1, 'seq', 0)
    p.c('seq', 3, 'xSet', 0)
    p.c('seq', 2, 'timer', 1)
    p.c('timer', 0, 'nowSet', 0)

    # MAKE ROOM, WRITE, AND (in ms) DROP WHAT IS OLDER THAN THE WINDOW.
    p.obj('W', 10, 200, 't b b b')
    p.c('seq', 1, 'W', 0)

    p.obj('nR1', 380, 230, 'v $0-n')
    p.obj('room', 380, 260, 'expr if($f1 >= $f2, 1, 0)')
    p.obj('roomSel', 380, 290, 'sel 1')
    p.msg('one', 380, 320, '1')
    p.c('W', 2, 'nR1', 0)
    p.c('nR1', 0, 'room', 0)
    p.c('room', 0, 'roomSel', 0)
    p.c('roomSel', 0, 'one', 0)

    p.obj('nR2', 220, 230, 'v $0-n')
    p.obj('wt', 220, 260, 't f b b f')
    p.obj('xR', 250, 290, 'v $0-x')
    p.obj('nowR', 300, 320, 'v $0-now')
    p.obj('twv', 250, 350, 'tabwrite $0-v')
    p.obj('twt', 300, 380, 'tabwrite $0-t')
    p.obj('plus1', 220, 410, '+ 1')
    p.obj('nSet1', 220, 440, 'v $0-n')
    p.c('W', 1, 'nR2', 0)
    p.c('nR2', 0, 'wt', 0)
    p.c('wt', 3, 'twv', 1)
    p.c('wt', 3, 'twt', 1)
    p.c('wt', 2, 'xR', 0)
    p.c('xR', 0, 'twv', 0)
    p.c('wt', 1, 'nowR', 0)
    p.c('nowR', 0, 'twt', 0)
    p.c('wt', 0, 'plus1', 0)
    p.c('plus1', 0, 'nSet1', 0)

    p.obj('modeR', 10, 230, 'v $0-mode')
    p.obj('modeSel', 10, 260, 'sel 1')
    p.obj('P', 10, 290, 't b b b b')
    p.obj('nowR2', 120, 320, 'v $0-now')
    p.obj('nR3', 170, 350, 'v $0-n')
    p.msg('zeroK', 90, 320, '0')
    p.obj('nR4', 50, 350, 'v $0-n')
    p.obj('until', 50, 380, 'until')
    p.obj('K', 50, 410, 'f')
    p.obj('Kplus', 90, 410, '+ 1')
    p.obj('Q', 50, 440, 't f f f')
    p.obj('KF', 10, 500, 'f')
    p.obj('tr', 50, 470, 'tabread $0-t')
    p.obj('old', 50, 500, 'expr if($f1 < $f2 - $f3 && $f4 < $f5 - 1, 1, 0)')
    p.obj('oldSel', 50, 530, 'sel 0')
    p.obj('KFmoses', 10, 560, 'moses 1')
    p.c('W', 0, 'modeR', 0)
    p.c('modeR', 0, 'modeSel', 0)
    p.c('modeSel', 0, 'P', 0)
    p.c('P', 3, 'nowR2', 0)
    p.c('P', 3, 'nR3', 0)
    p.c('nowR2', 0, 'old', 1)
    p.c('nR3', 0, 'old', 4)
    p.c('P', 2, 'zeroK', 0)
    p.c('zeroK', 0, 'K', 1)
    p.c('P', 1, 'nR4', 0)
    p.c('nR4', 0, 'until', 0)
    p.c('until', 0, 'K', 0)
    p.c('K', 0, 'Kplus', 0)
    p.c('Kplus', 0, 'K', 1)
    p.c('K', 0, 'Q', 0)
    p.c('Q', 2, 'old', 3)
    p.c('Q', 1, 'KF', 1)
    p.c('Q', 0, 'tr', 0)
    p.c('tr', 0, 'old', 0)
    p.c('old', 0, 'oldSel', 0)
    p.c('oldSel', 0, 'until', 1)
    p.c('P', 0, 'KF', 0)
    p.c('KF', 0, 'KFmoses', 0)

    # SHIFT BY k: the oldest k values and times go, n becomes n - k.
    p.obj('SH', 460, 380, 't f b f')
    p.obj('nR5', 490, 410, 'v $0-n')
    p.obj('minus', 490, 440, '-')
    p.obj('SM', 490, 470, 't f f')
    p.obj('nSet2', 560, 500, 'v $0-n')
    p.obj('agv', 460, 530, 'array get $0-v')
    p.obj('agt', 600, 530, 'array get $0-t')
    p.obj('asv', 460, 560, 'array set $0-v')
    p.obj('ast', 600, 560, 'array set $0-t')
    p.c('one', 0, 'SH', 0)
    p.c('KFmoses', 1, 'SH', 0)
    p.c('SH', 2, 'minus', 1)
    p.c('SH', 1, 'nR5', 0)
    p.c('nR5', 0, 'minus', 0)
    p.c('minus', 0, 'SM', 0)
    p.c('SM', 1, 'agv', 1)
    p.c('SM', 1, 'agt', 1)
    p.c('SM', 0, 'nSet2', 0)
    p.c('SH', 0, 'agv', 0)
    p.c('SH', 0, 'agt', 0)
    p.c('agv', 0, 'asv', 0)
    p.c('agt', 0, 'ast', 0)

    # THE ANSWER, over the n values the window holds.
    p.obj('nR6', 10, 600, 'v $0-n')
    p.c('seq', 0, 'nR6', 0)
    if name == 'go.avg':
        p.obj('RD', 10, 630, 't b f f')
        p.obj('asum', 10, 660, 'array sum $0-v')
        p.obj('div', 10, 690, '/')
        p.obj('out', 10, 720, 'outlet')
        p.c('nR6', 0, 'RD', 0)
        p.c('RD', 2, 'div', 1)
        p.c('RD', 1, 'asum', 1)
        p.c('RD', 0, 'asum', 0)
        p.c('asum', 0, 'div', 0)
        p.c('div', 0, 'out', 0)
    else:
        p.obj('RD', 10, 630, 't b b f')
        p.obj('amin', 10, 660, 'array min $0-v')
        p.obj('amax', 200, 660, 'array max $0-v')
        p.obj('outL', 10, 720, 'outlet')
        p.obj('outR', 640, 720, 'outlet')
        p.c('nR6', 0, 'RD', 0)
        p.c('RD', 2, 'amin', 1)
        p.c('RD', 2, 'amax', 1)
        p.c('RD', 1, 'amax', 0)
        p.c('RD', 0, 'amin', 0)
        p.c('amin', 0, 'outL', 0)
        p.c('amax', 0, 'outR', 0)

    # THE WINDOW: N values, or N ms with "ms"; a new one from the right inlet.
    p.obj('arrV', 700, 80, 'array define $0-v 1024')
    p.obj('arrT', 700, 110, 'array define $0-t 1024')
    p.obj('lb', 640, 140, 'loadbang')
    p.obj('IL', 640, 170, 't b b b')
    p.obj('l2', 700, 200, 'list append $2')
    p.obj('ltrim', 700, 230, 'list trim')
    p.obj('rms', 700, 260, 'route ms')
    p.msg('m1', 700, 290, '1')
    p.msg('m0', 760, 290, '0')
    p.obj('modeSet', 700, 320, 'v $0-mode')
    p.obj('w1', 640, 350, 'f $1')
    p.obj('wsel', 640, 380, 'sel 0')
    p.msg('w8', 640, 410, '8')
    p.obj('APPLY', 640, 440, 't b f')
    p.obj('wSet', 700, 470, 'v $0-w')
    p.obj('AP', 640, 500, 't b b b')
    p.obj('wR', 720, 530, 'v $0-w')
    p.obj('modeR2', 680, 560, 'v $0-mode')
    p.obj('capE', 680, 590, 'expr if($f1, 1024, min(max($f2, 1), 1024)); $f2')
    p.msg('zeroN', 640, 620, '0')
    p.obj('nSet3', 640, 650, 'v $0-n')
    p.c('lb', 0, 'IL', 0)
    p.c('IL', 2, 'timer', 0)
    p.c('IL', 1, 'l2', 0)
    p.c('l2', 0, 'ltrim', 0)
    p.c('ltrim', 0, 'rms', 0)
    p.c('rms', 0, 'm1', 0)
    p.c('rms', 1, 'm0', 0)
    p.c('m1', 0, 'modeSet', 0)
    p.c('m0', 0, 'modeSet', 0)
    p.c('IL', 0, 'w1', 0)
    p.c('w1', 0, 'wsel', 0)
    p.c('wsel', 0, 'w8', 0)
    p.c('wsel', 1, 'APPLY', 0)
    p.c('w8', 0, 'APPLY', 0)
    p.c('inR', 0, 'APPLY', 0)
    p.c('APPLY', 1, 'wSet', 0)
    p.c('APPLY', 0, 'AP', 0)
    p.c('AP', 2, 'wR', 0)
    p.c('wR', 0, 'capE', 1)
    p.c('AP', 1, 'modeR2', 0)
    p.c('modeR2', 0, 'capE', 0)
    p.c('capE', 0, 'room', 1)
    p.c('capE', 1, 'old', 2)
    p.c('AP', 0, 'zeroN', 0)
    p.c('route', 0, 'zeroN', 0)
    p.c('zeroN', 0, 'nSet3', 0)
    p.write(name + '.pd')


window('go.avg', 'go.avg - the average of the last values (go.avg 8) or of the values from the last milliseconds (go.avg 500 ms). reset forgets them. The right inlet is a new window.')
window('go.minmax', 'go.minmax - the smallest and the largest of the last values (go.minmax 8) or of the values from the last milliseconds (go.minmax 500 ms). reset forgets them. The right inlet is a new window.')


# =============================================================================
#  go.smooth: each new value moves the output part of the way.
# =============================================================================

p = Patch(640, 360)
p.text(10, 10, 'go.smooth - each value moves the output part of the way: 0.75 keeps three quarters of the last output (the default). The first value is taken whole. Reset forgets. Right inlet: how much is kept, 0 to 0.999.')
p.obj('in', 10, 70, 'inlet')
p.obj('inR', 400, 70, 'inlet')
p.obj('route', 10, 100, 'route reset float')
p.obj('e', 10, 160, 'expr if($f3, $f2*$f4 + $f1*(1-$f4), $f1)')
p.obj('t', 10, 190, 't f f b')
p.msg('one', 120, 220, '1')
p.obj('out', 10, 250, 'outlet')
p.msg('zero', 200, 130, '0')
p.obj('lb', 400, 100, 'loadbang')
p.obj('f1', 400, 130, 'f $1')
p.obj('sel', 400, 160, 'sel 0')
p.msg('def', 400, 190, '0.75')
p.obj('clip', 460, 220, 'clip 0 0.999')
p.c('in', 0, 'route', 0)
p.c('route', 0, 'zero', 0)
p.c('route', 1, 'e', 0)
p.c('e', 0, 't', 0)
p.c('t', 2, 'one', 0)
p.c('one', 0, 'e', 2)
p.c('t', 1, 'e', 1)
p.c('t', 0, 'out', 0)
p.c('zero', 0, 'e', 2)
p.c('lb', 0, 'f1', 0)
p.c('f1', 0, 'sel', 0)
p.c('sel', 0, 'def', 0)
p.c('sel', 1, 'clip', 0)
p.c('def', 0, 'clip', 0)
p.c('inR', 0, 'clip', 0)
p.c('clip', 0, 'e', 3)
p.write('go.smooth.pd')


# =============================================================================
#  go.scale: one range onto another.
# =============================================================================

p = Patch(760, 400)
p.text(10, 10, 'go.scale - a number from one range onto another: go.scale 0 1023 0 1 takes 0..1023 onto 0..1. With no range, 0..1 onto 0..1. Messages: in low high, out low high, clip 1 (or clip as a fifth argument) keeps the answer inside the out range.')
p.obj('in', 10, 90, 'inlet')
p.obj('trim', 10, 105, 'list trim')
p.obj('route', 10, 130, 'route in out clip')
p.obj('uin', 120, 150, 'unpack f f')
p.obj('uout', 220, 150, 'unpack f f')
p.obj('e', 10, 220, 'expr if($f3 == $f2, $f4, if($f6, min(max($f4 + ($f1-$f2)*($f5-$f4)/($f3-$f2), min($f4, $f5)), max($f4, $f5)), $f4 + ($f1-$f2)*($f5-$f4)/($f3-$f2)))')
p.obj('out', 10, 300, 'outlet')
p.obj('lb', 480, 90, 'loadbang')
p.obj('tl', 480, 120, 't b b')
p.obj('args', 480, 150, 'list append $1 $2 $3 $4')
p.obj('defaults', 480, 180, 'expr if($f1 == $f2, 0, $f1); if($f1 == $f2, 1, $f2); if($f1 == $f2, 0, $f3); if($f1 == $f2, 1, $f4)')
p.obj('l5', 600, 150, 'list append $5')
p.obj('ltrim', 600, 180, 'list trim')
p.obj('rclip', 600, 210, 'route clip')
p.msg('c1', 600, 240, '1')
p.c('in', 0, 'trim', 0)
p.c('trim', 0, 'route', 0)
p.c('route', 0, 'uin', 0)
p.c('route', 1, 'uout', 0)
p.c('route', 2, 'e', 5)
p.c('route', 3, 'e', 0)
p.c('uin', 0, 'e', 1)
p.c('uin', 1, 'e', 2)
p.c('uout', 0, 'e', 3)
p.c('uout', 1, 'e', 4)
p.c('e', 0, 'out', 0)
p.c('lb', 0, 'tl', 0)
p.c('tl', 1, 'args', 0)
p.c('args', 0, 'defaults', 0)
p.c('defaults', 0, 'e', 1)
p.c('defaults', 1, 'e', 2)
p.c('defaults', 2, 'e', 3)
p.c('defaults', 3, 'e', 4)
p.c('tl', 0, 'l5', 0)
p.c('l5', 0, 'ltrim', 0)
p.c('ltrim', 0, 'rclip', 0)
p.c('rclip', 0, 'c1', 0)
p.c('c1', 0, 'e', 5)
p.write('go.scale.pd')


# =============================================================================
#  go.deadband and go.change: a value passes only when it has moved.
# =============================================================================

def moved(name, about, symbols):
    p = Patch(640, 380)
    p.text(10, 10, about)
    p.obj('in', 10, 70, 'inlet')
    p.obj('inR', 400, 70, 'inlet')
    p.obj('route', 10, 100, 'route reset float symbol' if symbols else 'route reset float')
    p.obj('tf', 10, 130, 't f f')
    p.obj('e', 60, 160, 'expr if($f3 == 0 || abs($f1-$f2) > $f4, 1, 0)')
    p.obj('gate', 10, 190, 'spigot')
    p.obj('t', 10, 220, 't f f b')
    p.msg('one', 120, 250, '1')
    p.obj('out', 10, 300, 'outlet')
    p.msg('zero', 260, 130, '0')
    p.c('in', 0, 'route', 0)
    p.c('route', 0, 'zero', 0)
    p.c('route', 1, 'tf', 0)
    p.c('tf', 1, 'e', 0)
    p.c('e', 0, 'gate', 1)
    p.c('tf', 0, 'gate', 0)
    p.c('gate', 0, 't', 0)
    p.c('t', 2, 'one', 0)
    p.c('one', 0, 'e', 2)
    p.c('t', 1, 'e', 1)
    p.c('t', 0, 'out', 0)
    p.c('zero', 0, 'e', 2)
    if symbols:
        # A word passes when it is not the last word that passed.
        p.obj('ssel', 160, 220, 'sel go.change-forgotten')
        p.obj('ts', 200, 250, 't s s')
        p.msg('forget', 300, 190, 'symbol go.change-forgotten')
        p.c('route', 2, 'ssel', 0)
        p.c('ssel', 1, 'ts', 0)
        p.c('ts', 1, 'ssel', 1)
        p.c('ts', 0, 'out', 0)
        p.c('route', 0, 'forget', 0)
        p.c('forget', 0, 'ssel', 1)
    else:
        p.obj('lb', 400, 100, 'loadbang')
        p.obj('f1', 400, 130, 'f $1')
        p.obj('abs', 400, 160, 'abs')
        p.c('lb', 0, 'f1', 0)
        p.c('f1', 0, 'abs', 0)
        p.c('inR', 0, 'abs', 0)
        p.c('abs', 0, 'e', 3)
    if symbols:
        p.c('inR', 0, 'zero', 0)
        p.c('inR', 0, 'forget', 0)
    p.write(name + '.pd')


moved('go.deadband', 'go.deadband - a value passes only when it is further than the band from the last one that passed: go.deadband 4 lets 100 then 105 through but not 102. The first value passes. Reset forgets. Right inlet: the band.', False)
moved('go.change', 'go.change - a number or a word passes only when it is not the last one that passed. The first passes. Reset (or a bang on the right inlet) forgets.', True)


# =============================================================================
#  go.edge: a bang when a value crosses a threshold going up, another going down.
# =============================================================================

p = Patch(700, 380)
p.text(10, 10, 'go.edge - a bang on the left when the value rises above the threshold, on the right when it falls below: go.edge 512 20 crosses at 522 going up and 502 going down (a band of 20 so a jittery value crosses once). The first value says where it is and bangs nothing. Reset forgets. Right inlet: the threshold.')
p.obj('in', 10, 90, 'inlet')
p.obj('inR', 460, 90, 'inlet')
p.obj('route', 10, 120, 'route reset float')
p.obj('e', 10, 160, 'expr if($f1 > $f2 + $f3/2, 1, if($f1 < $f2 - $f3/2, 0, $f4)); if($f4 == 0 && $f1 > $f2 + $f3/2, 1, if($f4 == 1 && $f1 < $f2 - $f3/2, 2, 0))')
p.obj('back', 10, 220, 't f')
p.obj('sel', 300, 220, 'sel 1 2')
p.obj('outL', 10, 300, 'outlet')
p.obj('outR', 460, 300, 'outlet')
p.msg('unknown', 200, 120, '-1')
p.obj('lb', 520, 120, 'loadbang')
p.obj('args', 520, 150, 'list append $1 $2')
p.obj('un', 520, 180, 'unpack f f')
p.c('in', 0, 'route', 0)
p.c('route', 0, 'unknown', 0)
p.c('route', 1, 'e', 0)
p.c('e', 0, 'back', 0)
p.c('back', 0, 'e', 3)
p.c('e', 1, 'sel', 0)
p.c('sel', 0, 'outL', 0)
p.c('sel', 1, 'outR', 0)
p.c('unknown', 0, 'e', 3)
p.c('lb', 0, 'args', 0)
p.c('lb', 0, 'unknown', 0)
p.c('args', 0, 'un', 0)
p.c('un', 0, 'e', 1)
p.c('un', 1, 'e', 2)
p.c('inR', 0, 'e', 1)
p.write('go.edge.pd')


# =============================================================================
#  go.hold: a value held for a time, then the resting value.
# =============================================================================

p = Patch(700, 380)
p.text(10, 10, 'go.hold - each number passes at once and is held: when the time (ms) goes by with nothing new, the resting value goes out - 0, or the second argument. go.hold 2000 makes a press of a button last two seconds. A number that is the resting value passes and ends the hold. reset ends it without a word. Right inlet: the time.')
p.obj('in', 10, 90, 'inlet')
p.obj('inR', 460, 90, 'inlet')
p.obj('route', 10, 120, 'route reset float')
p.obj('tf', 10, 150, 't f f')
p.obj('isRest', 60, 180, 'expr $f1 == $f2')
p.obj('sel', 60, 210, 'sel 1 0')
p.msg('stop', 60, 240, 'stop')
p.obj('delay', 160, 270, 'delay 100')
p.obj('rest', 160, 300, 'f')
p.obj('out', 10, 340, 'outlet')
p.obj('lb', 460, 120, 'loadbang')
p.obj('args', 460, 150, 'list append $1 $2')
p.obj('un', 460, 180, 'unpack f f')
p.obj('time', 460, 210, 'sel 0')
p.msg('t100', 460, 240, '100')
p.obj('setTime', 520, 240, 't f')
p.c('in', 0, 'route', 0)
p.c('route', 0, 'stop', 0)
p.c('route', 1, 'tf', 0)
p.c('tf', 1, 'isRest', 0)
p.c('isRest', 0, 'sel', 0)
p.c('sel', 0, 'stop', 0)
p.c('sel', 1, 'delay', 0)
p.c('stop', 0, 'delay', 0)
p.c('tf', 0, 'out', 0)
p.c('delay', 0, 'rest', 0)
p.c('rest', 0, 'out', 0)
p.c('lb', 0, 'args', 0)
p.c('args', 0, 'un', 0)
p.c('un', 1, 'rest', 1)
p.c('un', 1, 'isRest', 1)
p.c('un', 0, 'time', 0)
p.c('time', 0, 't100', 0)
p.c('time', 1, 'setTime', 0)
p.c('inR', 0, 'setTime', 0)
p.c('t100', 0, 'delay', 1)
p.c('setTime', 0, 'delay', 1)
p.write('go.hold.pd')


# =============================================================================
#  go.ratelimit: at most one message every so many ms, the newest kept.
# =============================================================================

p = Patch(760, 420)
p.text(10, 10, 'go.ratelimit - at most one message every so many ms (100 by default): the first passes at once, and of those that come while it waits the newest passes when the time is up. A number, a word or a list. reset forgets what waits. Right inlet: the time.')
p.obj('in', 10, 90, 'inlet')
p.obj('inR', 520, 90, 'inlet')
p.obj('route', 10, 120, 'route reset')
p.obj('keep', 10, 150, 'list')
p.obj('tl', 10, 180, 't l l')
p.obj('busy', 120, 210, 'spigot')
p.obj('tb', 120, 240, 't b l')
p.msg('pend1', 120, 270, '1')
p.obj('pendSet', 120, 300, 'v $0-pend')
p.obj('waiting', 220, 270, 'list')
p.obj('idle', 10, 210, 'spigot 1')
p.obj('go', 10, 330, 't l b')
p.obj('nowBusy', 60, 360, 't b b')
p.msg('close', 30, 390, '0')
p.msg('open', 80, 390, '1')
p.obj('delay', 200, 360, 'delay 100')
p.obj('pendR', 200, 390, 'v $0-pend')
p.obj('psel', 200, 420, 'sel 1 0')
p.obj('again', 200, 450, 't b b')
p.msg('pend0', 280, 480, '0')
p.obj('pendSet2', 280, 510, 'v $0-pend')
p.obj('rest', 340, 420, 't b b')
p.msg('reopen', 340, 450, '1')
p.msg('reclose', 400, 450, '0')
p.obj('out', 10, 520, 'outlet')
p.obj('lb', 520, 120, 'loadbang')
p.obj('f1', 520, 150, 'f $1')
p.obj('sel', 520, 180, 'sel 0')
p.msg('t100', 520, 210, '100')
p.obj('setTime', 580, 210, 't f')
p.obj('reset', 600, 260, 't b b b')
p.msg('stop', 600, 290, 'stop')
p.c('in', 0, 'route', 0)
p.c('route', 1, 'keep', 0)
p.c('keep', 0, 'tl', 0)
# first: while busy, it waits (marked pending, kept in waiting); then: idle passes it
p.c('tl', 1, 'busy', 0)
p.c('busy', 0, 'tb', 0)
p.c('tb', 1, 'waiting', 1)
p.c('tb', 0, 'pend1', 0)
p.c('pend1', 0, 'pendSet', 0)
p.c('tl', 0, 'idle', 0)
p.c('idle', 0, 'go', 0)
# passing: busy from now on, the clock started, then out
p.c('go', 1, 'nowBusy', 0)
p.c('nowBusy', 1, 'close', 0)
p.c('close', 0, 'idle', 1)
p.c('nowBusy', 0, 'open', 0)
p.c('open', 0, 'busy', 1)
p.c('go', 1, 'delay', 0)
p.c('go', 0, 'out', 0)
# the time is up: the newest that waited goes, or Go.dot is idle again
p.c('delay', 0, 'pendR', 0)
p.c('pendR', 0, 'psel', 0)
p.c('psel', 0, 'again', 0)
p.c('again', 1, 'pend0', 0)
p.c('pend0', 0, 'pendSet2', 0)
p.c('again', 0, 'waiting', 0)
p.c('waiting', 0, 'go', 0)
p.c('psel', 1, 'rest', 0)
p.c('rest', 1, 'reclose', 0)
p.c('reclose', 0, 'busy', 1)
p.c('rest', 0, 'reopen', 0)
p.c('reopen', 0, 'idle', 1)
# the time, and reset
p.c('lb', 0, 'f1', 0)
p.c('f1', 0, 'sel', 0)
p.c('sel', 0, 't100', 0)
p.c('sel', 1, 'setTime', 0)
p.c('inR', 0, 'setTime', 0)
p.c('t100', 0, 'delay', 1)
p.c('setTime', 0, 'delay', 1)
p.c('route', 0, 'reset', 0)
p.c('reset', 2, 'stop', 0)
p.c('stop', 0, 'delay', 0)
p.c('reset', 1, 'pend0', 0)
p.c('reset', 0, 'rest', 0)
p.write('go.ratelimit.pd')


# =============================================================================
#  The help patches: each one played with, in Pd or plugdata, beside it.
# =============================================================================

def help_patch(name, intro, examples, notes):
    """examples: (abstraction text, inlet controls, outlets) - controls are
    ('atom'|'msg', text, inlet); a row each."""
    p = Patch(720, 140 + 110 * len(examples) + 30 * len(notes))
    p.text(20, 14, intro)
    y = 70
    for i, (box, controls, outlets) in enumerate(examples):
        x = 20
        for j, (kind, text, inlet) in enumerate(controls):
            nm = f'c{i}_{j}'
            if kind == 'atom':
                p.atom(nm, x, y, 6)
            else:
                p.msg(nm, x, y, text)
            x += 90 if kind == 'atom' else max(60, 12 * len(text))
        p.obj(f'b{i}', 20, y + 40, box)
        for j, (kind, text, inlet) in enumerate(controls):
            p.c(f'c{i}_{j}', 0, f'b{i}', inlet)
        for k in range(abs(outlets)):
            if outlets < 0:
                p.obj(f'o{i}_{k}', 20 + 200 * k, y + 75, 'bng 25 250 50 0 empty empty empty 0 -8 0 10 #fcfcfc #000000 #000000')
            else:
                p.atom(f'o{i}_{k}', 20 + 200 * k, y + 75, 8)
            p.c(f'b{i}', k, f'o{i}_{k}', 0)
        y += 110
    for line in notes:
        p.text(20, y, line)
        y += 30
    p.write(name + '-help.pd')


help_patch('go.avg', 'go.avg - the average of the last values, or of the values that came in the last milliseconds. One of Go.dot\'s ready-made patches.',
           [('go.avg 4', [('atom', '', 0), ('msg', 'reset', 0), ('atom', '', 1)], 1),
            ('go.avg 500 ms', [('atom', '', 0), ('msg', 'reset', 0)], 1)],
           ['Left inlet: a number, and the average goes out. Reset forgets the values.',
            'Right inlet: a new window, in values or in ms as the box was made. It forgets the values.',
            'A window holds at most 1024 values.'])
help_patch('go.minmax', 'go.minmax - the smallest (left) and the largest (right) of the last values, or of the values from the last milliseconds.',
           [('go.minmax 8', [('atom', '', 0), ('msg', 'reset', 0), ('atom', '', 1)], 2),
            ('go.minmax 1000 ms', [('atom', '', 0)], 2)],
           ['Left inlet: a number, and both go out, the largest first. Reset forgets.',
            'Right inlet: a new window, in values or in ms as the box was made. At most 1024 values.'])
help_patch('go.smooth', 'go.smooth - each value moves the output part of the way, which steadies a jittery sensor.',
           [('go.smooth 0.75', [('atom', '', 0), ('msg', 'reset', 0), ('atom', '', 1)], 1),
            ('go.smooth 0.95', [('atom', '', 0)], 1)],
           ['The argument is how much of the last output is kept, 0 to 0.999: 0.95 is slow and smooth, 0.5 quick.',
            'The first value is taken whole. Reset forgets, so the next is too. For a glide over a time use [line].'])
help_patch('go.scale', 'go.scale - a number from one range onto another, upside down if the out range is.',
           [('go.scale 0 1023 0 1', [('atom', '', 0), ('msg', 'in 0 127', 0), ('msg', 'out 1 0', 0), ('msg', 'clip 1', 0)], 1),
            ('go.scale 0 1 -90 90 clip', [('atom', '', 0)], 1)],
           ['Arguments: in-low in-high out-low out-high, and clip to keep the answer inside the out range.',
            'With no range it is 0..1 onto 0..1.'])
help_patch('go.deadband', 'go.deadband - a value passes only when it has moved further than the band from the last one that passed.',
           [('go.deadband 4', [('atom', '', 0), ('msg', 'reset', 0), ('atom', '', 1)], 1)],
           ['Steadies a sensor that wanders by a few steps. The first value passes. Reset forgets.',
            'Right inlet: the band.'])
help_patch('go.change', 'go.change - a number or a word passes only when it is not the last one that passed.',
           [('go.change', [('atom', '', 0), ('msg', 'symbol open', 0), ('msg', 'symbol shut', 0), ('msg', 'reset', 0)], 1)],
           ['A word goes out as a word, a number as a number. reset, or a bang on the right inlet, forgets.'])
help_patch('go.edge', 'go.edge - a bang on the left when the value rises above the threshold, on the right when it falls below.',
           [('go.edge 512 20', [('atom', '', 0), ('msg', 'reset', 0), ('atom', '', 1)], -2)],
           ['Arguments: the threshold, and a band around it: with 20 it crosses at 522 going up, 502 going down.',
            'The first value says which side it is on and bangs nothing. Reset forgets. Right inlet: the threshold.',
            'Fire a cue with it: a message box [; /godot/cmd/cue/fire <cue id>( under the left outlet.'])
help_patch('go.hold', 'go.hold - each number passes at once and is held. The resting value goes out when the time goes by with nothing new.',
           [('go.hold 2000', [('atom', '', 0), ('msg', '1', 0), ('msg', 'reset', 0), ('atom', '', 1)], 1),
            ('go.hold 500 -1', [('atom', '', 0)], 1)],
           ['Arguments: the time in ms (100 by default) and the resting value (0 by default).',
            'A number that is the resting value passes and ends the hold. Reset ends it without a word.'])
help_patch('go.ratelimit', 'go.ratelimit - at most one message every so many ms. The newest of those that wait goes when the time is up.',
           [('go.ratelimit 200', [('atom', '', 0), ('msg', '1 2 3', 0), ('msg', 'reset', 0), ('atom', '', 1)], 1)],
           ['Thins a sensor that sends hundreds a second to what a device can take. A number, a word or a list.',
            'reset forgets what waits. Right inlet: the time (100 ms by default).'])
print('written to', OUT)
