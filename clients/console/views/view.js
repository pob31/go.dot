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

/*  THE ONE DOOR EVERY RENDER GOES THROUGH. app.js puts `render` and its five
    parts here; the poll draws through `view.render`, a click that changes what
    is folded or picked redraws through it, and `render` calls each part as
    `view.renderLists()` and the rest - never by a name bound in some module.

    That is not ceremony. scripts/measure-console-render.py times M24 by
    putting a wrapper in place of each of these once the page has loaded,
    through the `window.goDot` app.js publishes. A call that went round this
    object - to an imported name - would never reach its wrapper, and its cost
    would drop out of the number it belongs in without anybody being told. */

const view = {};

export { view };
