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

"""What every preset generator under scripts/presets/ shares: a leaf, a
command, a container, a numbered run of them, the root with its GODOT key, and
the compact write (namespace draft §57, AFG, AFN; presets/devices/README.md).

A generated file is read by the engine and never edited by hand - the script
is - so it is written compact: pretty-printing twelve thousand nodes made the
DiGiCo preset seven megabytes."""

import json
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEVICES = os.path.join(REPO, "presets", "devices")


def span(lo, hi):
    return {"MIN": lo, "MAX": hi}


def vals(*values):
    return {"VALS": list(values)}


def leaf(path, types, access, value, description, ranges=None, unit=None, role=None, kind=None):
    """A node with a value: TYPE, ACCESS (3 read-write, 1 read-only), its VALUE, RANGE, UNIT."""
    node = {"FULL_PATH": path, "TYPE": types, "ACCESS": access}
    if value is not None:
        node["VALUE"] = value
    if ranges:
        node["RANGE"] = ranges if isinstance(ranges, list) else [ranges]
    if unit:
        node["UNIT"] = [unit] * len(types)
    node["DESCRIPTION"] = description
    godot = {}
    if role:
        godot["ROLE"] = role
    if kind:
        godot["KIND"] = kind
    if godot:
        node["GODOT"] = godot
    return node


def command(path, description, types=None, role=None, ranges=None):
    """A write-only node: what a GO is to a desk, a recall to a console."""
    node = {"FULL_PATH": path}
    if types:
        node["TYPE"] = types
    node["ACCESS"] = 2
    if ranges:
        node["RANGE"] = ranges if isinstance(ranges, list) else [ranges]
    node["DESCRIPTION"] = description
    if role:
        node["GODOT"] = {"ROLE": role}
    return node


def midi(node, **shape):
    """The node's MIDI shape (namespace draft 57, AFJ; DP.9): KIND pc, note, cc, nrpn, sysex or msc and its keys."""
    node.setdefault("GODOT", {})["MIDI"] = {key.upper(): value for key, value in shape.items()}
    return node


def container(path, description, contents):
    return {"FULL_PATH": path, "DESCRIPTION": description, "CONTENTS": contents}


def numbered(path, description, count, make, first=1, names=None):
    """A container of `count` children named 1..count (or `names`), each made by `make(child_path, n)`."""
    keys = names if names is not None else [str(n) for n in range(first, first + count)]
    return container(path, description, {k: make(path + "/" + k, k if names is not None else int(k)) for k in keys})


def put(tree, path, node):
    """Place `node` at `path` under the root `tree`, making containers on the way."""
    here = tree
    parts = path.strip("/").split("/")
    built = ""
    for name in parts[:-1]:
        built += "/" + name
        here = here.setdefault("CONTENTS", {}).setdefault(name, {"FULL_PATH": built})
    here.setdefault("CONTENTS", {})[parts[-1]] = node


def root(full_path, description, slug, vendor, model, sources, generated, transport="udp", wire="osc",
         framing=None, port=0, version=1, contents=None, readback=None, get=None, subscribe=None):
    godot = {
        "PRESET": slug,
        "VERSION": version,
        "VENDOR": vendor,
        "MODEL": model,
        "TRANSPORT": transport,
        "WIRE": wire,
    }
    if framing:
        godot["FRAMING"] = framing
    #  How the device is heard back (namespace draft 57, AFL; DP.10): the readback word a device made
    #  from this file starts with, the line that asks it ({address} the node), the line that subscribes.
    if readback:
        godot["READBACK"] = readback
    if get:
        godot["GET"] = get
    if subscribe:
        godot["SUBSCRIBE"] = subscribe
    godot["PORT"] = port
    godot["SOURCES"] = sources
    godot["GENERATED"] = generated
    return {"FULL_PATH": full_path, "DESCRIPTION": description, "GODOT": godot, "CONTENTS": contents or {}}


def count_leaves(node):
    contents = node.get("CONTENTS", {})
    own = 1 if "TYPE" in node or node.get("ACCESS") == 2 else 0
    return own + sum(count_leaves(child) for child in contents.values())


def write(tree, slug, out=None):
    folder = out or DEVICES
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, slug + ".json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(tree, f, separators=(",", ":"), ensure_ascii=False)
        f.write("\n")
    print("wrote %s: %d leaves" % (path, count_leaves(tree)))
    return path
