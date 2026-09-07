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
"""Turns the parameter table into C++.

Produces: src/wfg/engine/document/SchemaTable.generated.h, from
          docs/parameters/godot-parameters.csv.
Usage:    python3 scripts/generate-schema.py [--check] [--out PATH]
          --check writes nothing and exits 1 if the committed file has drifted.
Build requirements: python3 and its standard library. Nothing else.

WHY GENERATE IT AT ALL

The parameter table is meant to be added to as the engine grows, and a row in it
has to reach four places: the document schema, the parameter tree, the RELAX NG
schema and the OSCQuery reply. WFS-DIY keeps three of those by hand and pays for
it with a runtime auditor whose whole job is to log the drift; the point of
generating is that adding a row is one edit rather than four, and that the four
cannot disagree.

WHY THE OUTPUT IS COMMITTED

So that a clone with no Python builds. The generated header is a normal source
file in git; the generator is how you change it, and `--check` in CI is what
stops the two diverging. Same shape as scripts/check-claude-md.py, and for the
same reason: two things that must agree need something that checks they do.

WHAT THIS FILE DOES NOT DECIDE

Containment - which element may contain which - is not in the CSV, because the
CSV has one row per attribute and containment is a property of elements. It is
seven lines in Schema.cpp, written by hand, next to a comment saying so.
"""

import argparse
import csv
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
CSV_PATH = REPO_ROOT / "docs" / "parameters" / "godot-parameters.csv"
OUT_PATH = REPO_ROOT / "src" / "wfg" / "engine" / "document" / "SchemaTable.generated.h"

# The owners a row may name. What each one MEANS - which document element
# carries it, whether a Group inherits a Cue's rows - is Schema.cpp's business,
# not this script's: a generator that decides things is a generator people have
# to read before they can trust its output. This one transcribes.
#   The plural tokens are CONTAINERS rather than objects: `lists` is the
#   collection of cue lists and `runs` is the collection of runs, so their rows
#   are addressed /godot/list/order and /godot/run/order - with no identifier in
#   the middle, because there is only one of each. The address segment stays
#   singular on purpose: /godot/list/<id>/standby and /godot/list/focus are the
#   same container read two ways, and a client walking the tree should not have
#   to know that one of them is spelled differently.
KNOWN_OWNERS = ("engine", "document", "list", "lists", "cue", "group", "mount",
                "audio", "bus", "media", "route", "range", "run", "runs",
                "fade", "stop", "osc", "trigger", "midi", "port")

VALUE_TYPES = {
    "s": "string",
    "i": "integer",
    "h": "integer64",
    "f": "number",
    "d": "number",
    "T": "boolean",
    "b": "blob",
}

# A trailing `*` on the type tag means "a list of these", not a different type.
# `d*` is a run of doubles: one attribute holding N numbers, space separated in
# the document, N arguments on the wire. It is spelled as a suffix rather than as
# its own tag because everything else about the row - the range, the unit, the
# panic value - applies to each ELEMENT, and a separate type would have made all
# of that ambiguous.
LIST_SUFFIX = "*"

ACCESS = {"r": "read", "w": "write", "rw": "readWrite"}
KINDS = {"state": "state", "event": "event"}
PERSIST = {"none": "none", "show": "show", "state": "state"}


def fail(message):
    print("generate-schema: " + message, file=sys.stderr)
    raise SystemExit(2)


def cpp_string(text):
    """A C++ string literal. Escapes only what has to be escaped, so the
    generated file stays readable in a diff."""
    out = []
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ord(ch) < 0x20:
            out.append("\\x%02x" % ord(ch))
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def parse_range(text, row_label):
    """`min..max` with either end optional, or an enum as `a|b|c`, or blank.

    Returns (has_min, min, has_max, max, [enum values])."""
    text = (text or "").strip()
    if not text:
        return (False, 0.0, False, 0.0, [])

    if "|" in text:
        values = [v.strip() for v in text.split("|")]
        if any(not v for v in values):
            fail("%s: empty value in enum %r" % (row_label, text))
        return (False, 0.0, False, 0.0, values)

    if ".." not in text:
        fail("%s: range %r is neither `min..max` nor an `a|b|c` enum" % (row_label, text))

    low, _, high = text.partition("..")
    low, high = low.strip(), high.strip()

    def number(part):
        try:
            return float(part)
        except ValueError:
            fail("%s: %r in range %r is not a number" % (row_label, part, text))

    return (bool(low), number(low) if low else 0.0,
            bool(high), number(high) if high else 0.0, [])


# Where csv.DictReader files the fields of a row that has too many of them. It
# has to be given a name, because the default is None and a row with too FEW
# fields uses None as its missing VALUES - so without this the two cases would
# be indistinguishable.
OVERFLOW = "__overflow__"


def read_rows():
    if not CSV_PATH.is_file():
        fail("no parameter table at %s" % CSV_PATH)

    with CSV_PATH.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle, restkey=OVERFLOW, restval=None)
        expected = ["owner", "address", "type", "access", "default", "range",
                    "unit", "kind", "rate_cap", "anticipatable", "panic",
                    "persist", "description"]
        if reader.fieldnames != expected:
            fail("the table's columns changed.\n    expected: %s\n    found:    %s"
                 % (",".join(expected), ",".join(reader.fieldnames or [])))
        rows = list(reader)

    # THE FIELD COUNT, and this check exists because its absence cost nine
    # truncated descriptions before anyone noticed.
    #
    # `description` is the last column and it is prose, so it is the one that
    # attracts commas. RFC 4180 says a field containing one must be quoted;
    # DictReader does not object when it is not. It files everything past the
    # last column under restkey and hands back a description cut off at the
    # first comma, and the row still looks perfectly well formed.
    #
    # Those descriptions are not decoration. They become the DESCRIPTION an
    # OSCQuery client shows someone at 2 a.m. and the documentation in the
    # published RELAX NG grammar, and a sentence that stops mid-clause is
    # exactly the kind of wrong that reads as deliberate.
    for number, row in enumerate(rows, start=2):     # line 1 is the header
        label = "%s:%d" % (CSV_PATH.name, number)

        if OVERFLOW in row:
            fail("%s: %d fields, expected %d.\n"
                 "    A field containing a comma must be double-quoted (RFC 4180).\n"
                 "    kept: %r\n"
                 "    lost: %r\n"
                 "    fix:  wrap the whole description in double quotes."
                 % (label, len(reader.fieldnames) + len(row[OVERFLOW]),
                    len(reader.fieldnames), row["description"],
                    ",".join(row[OVERFLOW])))

        short = [name for name, value in row.items() if value is None]

        if short:
            fail("%s: %d fields, expected %d. Missing: %s"
                 % (label, len(reader.fieldnames) - len(short),
                    len(reader.fieldnames), ", ".join(short)))

    return rows


def build(rows):
    """One entry per row, in the table's own order, so the generated array
    reads the way a person grouped it rather than the way a sort would."""
    entries = []
    seen = set()

    for number, row in enumerate(rows, start=2):     # line 1 is the header
        label = "%s:%d" % (CSV_PATH.name, number)
        owner = row["owner"].strip()

        if owner not in KNOWN_OWNERS:
            fail("%s: unknown owner %r (expected one of %s)"
                 % (label, owner, ", ".join(KNOWN_OWNERS)))

        address = row["address"].strip()
        if not address:
            fail("%s: empty address" % label)

        type_tag = row["type"].strip()
        is_list = type_tag.endswith(LIST_SUFFIX)

        if is_list:
            type_tag = type_tag[:-len(LIST_SUFFIX)]

        if type_tag not in VALUE_TYPES:
            fail("%s: unknown type %r" % (label, type_tag))

        # A list of booleans or blobs has no spelling in the document and no
        # caller; refusing it here is cheaper than discovering it in RelaxNg.
        if is_list and type_tag in ("T", "b"):
            fail("%s: %r cannot be a list" % (label, type_tag + LIST_SUFFIX))

        access = row["access"].strip()
        if access not in ACCESS:
            fail("%s: unknown access %r" % (label, access))

        kind = row["kind"].strip()
        if kind not in KINDS:
            fail("%s: unknown kind %r" % (label, kind))

        persist = row["persist"].strip()
        if persist not in PERSIST:
            fail("%s: unknown persist %r" % (label, persist))

        rate_cap = row["rate_cap"].strip()
        try:
            rate_cap_value = float(rate_cap) if rate_cap else 0.0
        except ValueError:
            fail("%s: rate_cap %r is not a number" % (label, rate_cap))

        anticipatable = row["anticipatable"].strip().lower()
        if anticipatable not in ("yes", "no"):
            fail("%s: anticipatable must be yes or no, found %r" % (label, anticipatable))

        has_min, low, has_max, high, enum_values = parse_range(row["range"], label)

        default = (row["default"] or "").strip()

        # An enum's default has to be one of its values, or the document can be
        # written with a value the schema then rejects.
        if enum_values and default and default not in enum_values:
            fail("%s: default %r is not one of %s" % (label, default, "|".join(enum_values)))

        key = (owner, address)
        if key in seen:
            fail("%s: %s/%s is defined twice" % (label, owner, address))
        seen.add(key)

        entries.append({
                "owner": owner,
                "name": address,
                "type": VALUE_TYPES[type_tag],
                "typeTag": type_tag,
                "isList": is_list,
                "access": ACCESS[access],
                "kind": KINDS[kind],
                "default": default,
                "hasDefault": bool(default),
                "hasMin": has_min, "min": low,
                "hasMax": has_max, "max": high,
                "enum": enum_values,
                "unit": (row["unit"] or "").strip(),
                "rateCap": rate_cap_value,
                "anticipatable": anticipatable == "yes",
                "panic": (row["panic"] or "").strip(),
                "persist": PERSIST[persist],
                "description": (row["description"] or "").strip(),
        })

    return entries


def number_literal(value):
    """A C++ double literal that reads back as the same value. Kept short for
    the common cases so the generated table stays legible."""
    if value == int(value) and abs(value) < 1e15:
        return "%d.0" % int(value)
    return repr(value)


def render(entries):
    lines = []
    add = lines.append

    add("/*")
    add("    This file is part of Go.dot — https://github.com/pob31/go.dot")
    add("")
    add("    Copyright (C) 2026 Pierre-Olivier Boulant")
    add("")
    add("    Go.dot is free software: you can redistribute it and/or modify it under the")
    add("    terms of the GNU General Public License as published by the Free Software")
    add("    Foundation, either version 3 of the License, or (at your option) any later")
    add("    version. Go.dot is distributed in the hope that it will be useful, but")
    add("    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY")
    add("    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License")
    add("    (LICENSE, at the repository root) for more details.")
    add("")
    add("    SPDX-License-Identifier: GPL-3.0-or-later")
    add("*/")
    add("")
    add("#pragma once")
    add("")
    add("/*")
    add("    GENERATED FILE — do not edit.")
    add("")
    add("    Written by scripts/generate-schema.py from")
    add("    docs/parameters/godot-parameters.csv. To change anything here, change a")
    add("    row in that file and run the generator; the test `schema.generated` fails")
    add("    if the two disagree, so a hand edit here does not survive CI.")
    add("")
    add("    It is committed rather than generated at build time so that a clone with")
    add("    no Python still builds.")
    add("*/")
    add("")
    add("#include <wfg/engine/document/SchemaTypes.h>")
    add("")
    add("namespace wfg::doc::generated")
    add("{")

    # Enum values need somewhere to live: AttributeRow holds a pointer and a
    # count, so a braced list inline in the table would have nothing to point
    # at. One named array per row that has an enum, emitted first.
    enum_arrays = {}

    for e in entries:
        if not e["enum"]:
            continue
        symbol = "enum_%s_%s" % (e["owner"], e["name"])
        enum_arrays[(e["owner"], e["name"])] = symbol
        add("    inline constexpr std::string_view %s[] = { %s };"
            % (symbol, ", ".join(cpp_string(v) for v in e["enum"])))

    if enum_arrays:
        add("")

    add("    inline constexpr AttributeRow attributes[] =")
    add("    {")

    for e in entries:
        enum_literal = enum_arrays.get((e["owner"], e["name"]), "nullptr")
        add("        { %s, %s," % (cpp_string(e["owner"]), cpp_string(e["name"])))
        add("          ValueType::%s, '%s', %s, Access::%s, Kind::%s, Persist::%s,"
            % (e["type"], e["typeTag"], "true" if e["isList"] else "false",
               e["access"], e["kind"], e["persist"]))
        add("          %s, %s," % ("true" if e["hasDefault"] else "false", cpp_string(e["default"])))
        add("          %s, %s, %s, %s," % ("true" if e["hasMin"] else "false", number_literal(e["min"]),
                                           "true" if e["hasMax"] else "false", number_literal(e["max"])))
        add("          %s, %d," % (enum_literal, len(e["enum"])))
        add("          %s, %s, %s, %s," % (cpp_string(e["unit"]), number_literal(e["rateCap"]),
                                           "true" if e["anticipatable"] else "false",
                                           cpp_string(e["panic"])))
        add("          %s }," % cpp_string(e["description"]))

    add("    };")
    add("}")
    add("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="write nothing; exit 1 if the committed file has drifted")
    parser.add_argument("--out", default=None, help="write somewhere else than the default")
    args = parser.parse_args()

    entries = build(read_rows())
    text = render(entries)
    out = Path(args.out) if args.out else OUT_PATH

    if args.check:
        if not out.is_file():
            print("generate-schema: %s does not exist; run the generator" % out, file=sys.stderr)
            return 1

        current = out.read_text(encoding="utf-8")
        if current == text:
            print("generate-schema: %s is up to date" % out.name)
            return 0

        print("generate-schema: %s has drifted from %s.\n"
              "    Run: python3 scripts/generate-schema.py\n"
              "    The table is the source; the header is its output."
              % (out.name, CSV_PATH.name), file=sys.stderr)

        import difflib
        for line in difflib.unified_diff(current.splitlines(), text.splitlines(),
                                         fromfile="committed", tofile="generated",
                                         lineterm=""):
            print("      " + line, file=sys.stderr)
        return 1

    out.parent.mkdir(parents=True, exist_ok=True)
    # newline="\n" on purpose: this is a source file, and .gitattributes pins
    # *.h to LF. Letting Python translate would make the drift check fail on
    # Windows and pass everywhere else.
    out.write_text(text, encoding="utf-8", newline="\n")
    print("generate-schema: wrote %s (%d rows)" % (out.name, len(entries)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
