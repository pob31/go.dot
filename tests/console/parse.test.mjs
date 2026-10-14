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

/*  EVERY SCRIPT THE PAGE LOADS PARSES - the one thing no other test here can
    say, because each imports only the modules it is about.

    The page is ES modules with no build step (§14): a browser fetches app.js,
    which imports the rest, and ONE early error anywhere in that graph - a
    name declared twice, a stray brace - stops every module of it, so the page
    shows nothing at all. From 43d15f9 (2026-09-25) to RD.11 (2026-10-15)
    views/strip.js declared `isLocked` twice in one function and the page did
    not start; blackbox.client serves the files and client_page.py reads the
    HTML, and neither ever runs them, so three weeks of green CI said nothing.

    HOW IT IS CHECKED. `node --check` on a file named .js reads it as CommonJS
    and, on the import it meets, takes it for a module by detection - and that
    path passed the broken strip.js with exit 0 under Node 24. So each file is
    handed to Node on standard input, read as a module outright
    (`--input-type=module --check`), which parses it as the browser's module
    loader does and imports nothing: a syntax check only, no DOM needed. */

import { test } from "node:test";
import assert from "node:assert/strict";

import { spawnSync } from "node:child_process";
import { readdirSync, readFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const pageRoot = join(here, "..", "..", "clients", "console");

function scriptsUnder(folder) {
  const found = [];

  for (const entry of readdirSync(folder, { withFileTypes: true })) {
    const path = join(folder, entry.name);

    if (entry.isDirectory())
      found.push(...scriptsUnder(path));
    else if (entry.name.endsWith(".js"))
      found.push(path);
  }

  return found.sort();
}

function parsesAsModule(source) {
  return spawnSync(process.execPath, ["--input-type=module", "--check"],
                   { input: source, encoding: "utf8" });
}

test("the check hears: a name declared twice in one scope is refused", () => {
  const result = parsesAsModule("export function f() {\n  const a = 1;\n  const a = 2;\n  return a;\n}\n");
  assert.notEqual(result.status, 0, "a module that cannot load was passed - the check below would be deaf");
  assert.match(result.stderr, /SyntaxError/);
});

test("every script of the page parses as the browser's module loader reads it", () => {
  const scripts = scriptsUnder(pageRoot);
  assert.ok(scripts.length >= 20, `only ${scripts.length} scripts found under ${pageRoot}`);

  const broken = [];

  for (const path of scripts) {
    const result = parsesAsModule(readFileSync(path, "utf8"));

    if (result.status !== 0)
      broken.push(`${relative(pageRoot, path)}: ${result.stderr.trim().split(/\r?\n/).slice(0, 5).join(" | ")}`);
  }

  assert.deepEqual(broken, [], `the page would not start:\n${broken.join("\n")}`);
});
