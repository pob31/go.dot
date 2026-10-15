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

# ONE PD FOR EVERY PATCH GO.DOT OPENS (namespace draft §51; the author,
# 2026-10-15: "There are as many console windows as opened patches and you
# can't tell which console belongs to which patch").
#
# Go.dot starts Pd once, with GODOT_PD_EDITOR set to a port and a word, and
# this folder on Pd's search path, where Pd's window loads this plugin. Here
# the window listens on that port, on this machine only, for a line that
# begins with the word:
#   <word> open <file>     the patch's window brought in front if it is open
#                          here, else the file opened as File > Open would;
#   <word> reopen <file>   the patch changed in Go.dot since Pd opened it:
#                          its window here closed - Pd asking first if it has
#                          edits not saved - and the file opened again.
# Each later patch Go.dot is asked to open in Pd is such a line, so they share
# this one Pd and its one main window. A Pd started any other way has no
# GODOT_PD_EDITOR and listens to nothing.

namespace eval ::godot_open {
    variable word {}
}

# The patch window showing this file, or nothing: Pd titles one
# "<name> - <folder>", with " [edit]" after it while it is being edited.
proc ::godot_open::window_of {file} {
    set shown "[file tail $file] - [file dirname $file]"
    foreach window [winfo children .] {
        if {[winfo class $window] eq "PatchWindow"
                && [string first $shown [wm title $window]] == 0} {
            return $window
        }
    }
    return {}
}

proc ::godot_open::accept {channel address port} {
    fconfigure $channel -encoding utf-8 -translation auto -blocking 0
    fileevent $channel readable [list ::godot_open::heard $channel]
}

proc ::godot_open::heard {channel} {
    variable word
    if {[gets $channel line] < 0} {
        if {[eof $channel]} {
            close $channel
        }
        return
    }
    close $channel

    if {$word eq {} || ! [regexp {^(\S+) (open|reopen) (.+)$} $line -> said verb file]
            || $said ne $word} {
        return
    }
    set file [file normalize $file]
    set window [window_of $file]

    if {$window eq {}} {
        open_file $file
    } elseif {$verb eq "open"} {
        wm deiconify $window
        raise $window
        focus $window
    } else {
        pdsend "$window menuclose 0"
        after 500 [list open_file $file]
    }
}

if {[info exists ::env(GODOT_PD_EDITOR)]} {
    lassign $::env(GODOT_PD_EDITOR) ::godot_open::port ::godot_open::word
    if {[string is integer -strict $::godot_open::port] && $::godot_open::word ne {}} {
        if {[catch {socket -server ::godot_open::accept -myaddr 127.0.0.1 $::godot_open::port}]} {
            set ::godot_open::word {}
        }
    }
}
