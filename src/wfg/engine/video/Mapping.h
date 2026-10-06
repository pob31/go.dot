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

#pragma once

/*
    AN OUTPUT'S MAPPING (Phase 8a, namespace draft 35, PRD §3.19a and §3.19b):
    the mesh that bends a canvas onto a wall, and the calibration grade that
    matches one projector to another.

    THE MESH is a grid of control points, `columns` across and `rows` down,
    each where that point of the canvas lands on the display - x right, y
    down, 0..1 of the display - read row by row from the top-left. Between
    them the surface is smooth: a bicubic Catmull-Rom patch through the
    points, with a phantom row and column beyond each edge extrapolated in a
    straight line, so a grid nobody moved is exactly the identity and the
    canvas fills the display. Two by two is a keystone corner-pin; more points
    bend it round a column or into a cyclorama's curve.

    THE CALIBRATION GRADE is ASC CDL (VP): per channel slope, offset and power
    - out = (in x slope + offset) ^ power - then a saturation about Rec. 709's
    luma, as colourists exchange it.

    Pure, no allocation but a mesh's points.
*/

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace wfg::video
{
    struct Mesh
    {
        static constexpr int maxPoints = 16;

        int columns = 2;
        int rows = 2;
        std::vector<float> x;   ///< columns x rows, row by row from the top-left
        std::vector<float> y;

        bool isValid() const noexcept
        {
            return columns >= 2 && rows >= 2 && columns <= maxPoints && rows <= maxPoints
                && x.size() == static_cast<std::size_t> (columns * rows) && y.size() == x.size();
        }

        /*  The grid nobody moved: the canvas fills the display. */
        static Mesh identity (int columnsToUse = 2, int rowsToUse = 2)
        {
            Mesh mesh;
            mesh.columns = std::clamp (columnsToUse, 2, maxPoints);
            mesh.rows = std::clamp (rowsToUse, 2, maxPoints);

            for (int row = 0; row < mesh.rows; ++row)
                for (int column = 0; column < mesh.columns; ++column)
                {
                    mesh.x.push_back (static_cast<float> (column) / static_cast<float> (mesh.columns - 1));
                    mesh.y.push_back (static_cast<float> (row) / static_cast<float> (mesh.rows - 1));
                }

            return mesh;
        }

        bool isIdentity() const noexcept
        {
            if (! isValid())
                return true;

            for (int row = 0; row < rows; ++row)
                for (int column = 0; column < columns; ++column)
                {
                    const auto at = static_cast<std::size_t> (row * columns + column);
                    const auto wantX = static_cast<float> (column) / static_cast<float> (columns - 1);
                    const auto wantY = static_cast<float> (row) / static_cast<float> (rows - 1);

                    if (std::abs (x[at] - wantX) > 1e-6f || std::abs (y[at] - wantY) > 1e-6f)
                        return false;
                }

            return true;
        }
    };

    namespace detail
    {
        /*  A control point, the grid extrapolated straight past its edges. */
        inline void controlAt (const Mesh& mesh, int column, int row, double& px, double& py) noexcept
        {
            const auto clampedColumn = std::clamp (column, 0, mesh.columns - 1);
            const auto clampedRow = std::clamp (row, 0, mesh.rows - 1);

            const auto read = [&mesh] (int c, int r, double& ox, double& oy)
            {
                const auto at = static_cast<std::size_t> (r * mesh.columns + c);
                ox = static_cast<double> (mesh.x[at]);
                oy = static_cast<double> (mesh.y[at]);
            };

            if (clampedColumn == column && clampedRow == row)
            {
                read (column, row, px, py);
                return;
            }

            /*  BEYOND AN EDGE: the edge point, plus the step from its inner
                neighbour, as many steps as it is beyond. */
            double ex = 0.0, ey = 0.0;
            controlAt (mesh, clampedColumn, clampedRow, ex, ey);
            px = ex;
            py = ey;

            if (column != clampedColumn)
            {
                const auto inner = clampedColumn + (column < clampedColumn ? 1 : -1);
                double ix = 0.0, iy = 0.0;
                controlAt (mesh, inner, clampedRow, ix, iy);
                const auto steps = static_cast<double> (std::abs (column - clampedColumn));
                px += (ex - ix) * steps;
                py += (ey - iy) * steps;
            }

            if (row != clampedRow)
            {
                const auto inner = clampedRow + (row < clampedRow ? 1 : -1);
                double ix = 0.0, iy = 0.0;
                controlAt (mesh, clampedColumn, inner, ix, iy);
                const auto steps = static_cast<double> (std::abs (row - clampedRow));
                px += (ex - ix) * steps;
                py += (ey - iy) * steps;
            }
        }

        inline void catmullRomWeights (double t, double w[4]) noexcept
        {
            const auto t2 = t * t, t3 = t2 * t;
            w[0] = -0.5 * t3 + t2 - 0.5 * t;
            w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
            w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
            w[3] = 0.5 * t3 - 0.5 * t2;
        }
    }

    /*  WHERE THE CANVAS'S POINT (s, t) LANDS on the display - s right and t
        down, 0..1 of the canvas; the answer in 0..1 of the display, x right,
        y down. An invalid mesh is the identity. */
    inline void meshAt (const Mesh& mesh, double s, double t, double& x, double& y) noexcept
    {
        if (! mesh.isValid())
        {
            x = s;
            y = t;
            return;
        }

        const auto gx = std::clamp (s, 0.0, 1.0) * static_cast<double> (mesh.columns - 1);
        const auto gy = std::clamp (t, 0.0, 1.0) * static_cast<double> (mesh.rows - 1);
        const auto column = std::min (static_cast<int> (gx), mesh.columns - 2);
        const auto row = std::min (static_cast<int> (gy), mesh.rows - 2);

        double wx[4], wy[4];
        detail::catmullRomWeights (gx - column, wx);
        detail::catmullRomWeights (gy - row, wy);

        x = 0.0;
        y = 0.0;

        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
            {
                double px = 0.0, py = 0.0;
                detail::controlAt (mesh, column - 1 + i, row - 1 + j, px, py);
                x += wx[i] * wy[j] * px;
                y += wx[i] * wy[j] * py;
            }
    }

    //==============================================================================
    /*  AN OUTPUT'S CALIBRATION: ASC CDL. Nothing set is the identity. */
    struct Cdl
    {
        double slope[3] { 1.0, 1.0, 1.0 };
        double offset[3] { 0.0, 0.0, 0.0 };
        double power[3] { 1.0, 1.0, 1.0 };
        double saturation = 1.0;

        bool isIdentity() const noexcept
        {
            for (int n = 0; n < 3; ++n)
                if (std::abs (slope[n] - 1.0) > 1e-9 || std::abs (offset[n]) > 1e-9 || std::abs (power[n] - 1.0) > 1e-9)
                    return false;

            return std::abs (saturation - 1.0) < 1e-9;
        }

        /*  Ten numbers: slope r g b, offset r g b, power r g b, saturation -
            the order a .cdl file lists them. Fewer is the identity. */
        static Cdl from (const std::vector<double>& numbers)
        {
            Cdl cdl;

            if (numbers.size() < 10)
                return cdl;

            for (int n = 0; n < 3; ++n)
            {
                cdl.slope[n] = std::max (0.0, numbers[static_cast<std::size_t> (n)]);
                cdl.offset[n] = numbers[static_cast<std::size_t> (3 + n)];
                cdl.power[n] = std::max (0.01, numbers[static_cast<std::size_t> (6 + n)]);
            }

            cdl.saturation = std::max (0.0, numbers[9]);
            return cdl;
        }
    };

    inline void applyCdl (const Cdl& cdl, double& red, double& green, double& blue) noexcept
    {
        double c[3] { red, green, blue };

        for (int n = 0; n < 3; ++n)
            c[n] = std::pow (std::clamp (c[n] * cdl.slope[n] + cdl.offset[n], 0.0, 1.0), cdl.power[n]);

        const auto luma = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];

        for (auto& v : c)
            v = std::clamp (luma + cdl.saturation * (v - luma), 0.0, 1.0);

        red = c[0];
        green = c[1];
        blue = c[2];
    }
}
