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

# PD'S WINDOW ON A SCALED SCREEN (namespace draft §51; the author, 2026-10-15:
# "Menus in Pd are still microscopic").
#
# Go.dot starts Pd with this folder on its search path, and Pd's window loads
# every *-plugin.tcl it finds there before it opens a patch. Pd draws a patch a
# pixel to a pixel, and on Windows it sets its menus, its dialogs and its main
# window in Tahoma at eleven PIXELS, so on a screen scaled to 175 per cent all
# of it is half the size of everything else. Here the screen's scale is read as
# Tk sees it, and
#   - from 125 per cent, the menus, dialogs and main window get their eleven
#     pixels times the scale (Windows: the only one that fixes them in pixels);
#   - from 150 per cent, each patch window is zoomed the first time it shows,
#     as Ctrl and + zoom it, so the patch is drawn twice the size, and the
#     window made twice the size as far as the screen goes; and Pd's own
#     "Zoom New Windows" is set for what is opened from its menus. Pd's
#     setting alone does not reach the patch it was started on, which is the
#     one Go.dot hands it. A window zoomed out by hand stays as the hand left it.
# The Mac scales Pd's window itself and is left alone. A Pd that cannot tell
# its screen's scale (one Windows scales as a picture) reads one and changes
# nothing.

namespace eval ::godot_screen {
    variable scale 1.0
    variable zoomed {}
}

proc ::godot_screen::patch_shown {window} {
    variable zoomed
    if {[lsearch -exact $zoomed $window] >= 0} {
        return
    }
    lappend zoomed $window
    pdsend "$window zoom 2"
    after idle [list ::godot_screen::grow $window]
}

# Twice the size too, as far as the screen goes: Pd's zoom leaves the window
# as it was, so a zoomed patch showed its top left quarter. The size Pd saves
# grows with it; Go.dot keeps the patch's own when it takes a save back.
proc ::godot_screen::grow {window} {
    if {! [winfo exists $window]
            || [scan [wm geometry $window] {%dx%d+%d+%d} width height x y] != 4} {
        return
    }
    set width [expr {min(2 * $width, [winfo screenwidth $window] - 40)}]
    set height [expr {min(2 * $height, [winfo screenheight $window] - 120)}]
    wm geometry $window ${width}x${height}
}

proc ::godot_screen::patch_gone {window} {
    variable zoomed
    set zoomed [lsearch -all -inline -not -exact $zoomed $window]
}

if {$::windowingsystem ne "aqua"} {
    set ::godot_screen::scale [expr {[winfo fpixels . 1i] / 96.0}]

    if {$::godot_screen::scale >= 1.25 && $::windowingsystem eq "win32"
            && [lsearch -exact [font names] menufont] >= 0} {
        font configure menufont -size [expr {-round(11 * $::godot_screen::scale)}]
    }

    if {$::godot_screen::scale >= 1.5} {
        pdsend "pd zoom-open 1"
        bind PatchWindow <Map> {+::godot_screen::patch_shown %W}
        bind PatchWindow <Destroy> {+::godot_screen::patch_gone %W}
    }
}
