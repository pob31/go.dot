//  This file is part of Go.dot — https://github.com/pob31/go.dot
//
//  Copyright (C) 2026 Pierre-Olivier Boulant
//
//  Go.dot is free software: you can redistribute it and/or modify it under the
//  terms of the GNU General Public License as published by the Free Software
//  Foundation, either version 3 of the License, or (at your option) any later
//  version. Go.dot is distributed in the hope that it will be useful, but
//  WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
//  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
//  (LICENSE, at the repository root) for more details.
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  THE RENDERER'S SHADERS (namespace draft §44.4), written once and translated
//  by sokol-shdc for Direct3D 11, Metal and OpenGL into video.glsl.h - never
//  edit that header, run scripts/generate-shaders.py. The arithmetic is the
//  OpenGL renderer's, line for line, and Compositor.h's: a test holds the
//  pixels these draw to the reference compositor.
//
//  Colour is premultiplied from the moment a layer is drawn: the blends are
//  the compositor's formulas as fixed-function equations (VD).

@module video

//------------------------------------------------------------------------------
//  A QUAD WITHOUT A BUFFER: its four corners, as a triangle strip, from the
//  uniforms - where each lies in the target and which texel it shows. A layer
//  is one draw, so nothing is written to a buffer between two of them.
@vs quad_vs
layout(binding=0) uniform quad {
    vec4 corner_xy01;   // x0 y0 x1 y1: bottom-left, bottom-right
    vec4 corner_xy23;   // x2 y2 x3 y3: top-left, top-right
    vec4 corner_uv01;
    vec4 corner_uv23;
};

out vec2 at;

void main() {
    int n = gl_VertexIndex;
    vec2 p = n == 0 ? corner_xy01.xy : (n == 1 ? corner_xy01.zw : (n == 2 ? corner_xy23.xy : corner_xy23.zw));
    at = n == 0 ? corner_uv01.xy : (n == 1 ? corner_uv01.zw : (n == 2 ? corner_uv23.xy : corner_uv23.zw));
    gl_Position = vec4(p, 0.0, 1.0);
}
@end

//------------------------------------------------------------------------------
//  THE MESH'S VERTICES: a grid laid where the mesh sends each point (Mapping.h).
@vs mesh_vs
layout(location=0) in vec2 position;
layout(location=1) in vec2 texel;

out vec2 at;

void main() {
    at = texel;
    gl_Position = vec4(position, 0.0, 1.0);
}
@end

//------------------------------------------------------------------------------
//  THE GRADE (Grade.h, line for line): gamma, contrast about mid-grey, the hue
//  turned about the grey axis, the saturation, then the baked tables - one
//  texel a step, red, green and blue.
@block grade_block
layout(binding=1) uniform grade {
    vec4 grade_a;       // gamma, contrast, saturation, curves (0 or 1)
    vec4 grade_b;       // cos (hue), sin (hue), opacity, -
};

layout(binding=1) uniform texture2D grade_tables;
layout(binding=1) uniform sampler grade_smp;

vec3 apply_grade(vec3 c) {
    c = pow(clamp(c, 0.0, 1.0), vec3(1.0 / grade_a.x));
    c = (c - 0.5) * grade_a.y + 0.5;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    vec3 colour = c - luma;
    vec3 axis = vec3(0.57735026918963);
    colour = colour * grade_b.x + cross(axis, colour) * grade_b.y + axis * dot(axis, colour) * (1.0 - grade_b.x);
    c = clamp(luma + colour * grade_a.z, 0.0, 1.0);
    if (grade_a.w > 0.5) {
        vec3 step_at = (c * 255.0 + 0.5) / 256.0;
        c = vec3(texture(sampler2D(grade_tables, grade_smp), vec2(step_at.r, 0.5)).r,
                 texture(sampler2D(grade_tables, grade_smp), vec2(step_at.g, 0.5)).g,
                 texture(sampler2D(grade_tables, grade_smp), vec2(step_at.b, 0.5)).b);
    }
    return c;
}
@end

//------------------------------------------------------------------------------
//  A FILL: a colour, premultiplied by its opacity (the colour's alpha).
@fs fill_fs
layout(binding=1) uniform fill {
    vec4 colour;
};

in vec2 at;
out vec4 frag_colour;

void main() {
    frag_colour = vec4(colour.rgb * colour.a, colour.a);
}
@end

@program fill quad_vs fill_fs

//------------------------------------------------------------------------------
//  A PICTURE: read premultiplied, as the store holds it; graded on its own
//  colour; premultiplied again and taken down by the layer's opacity.
@fs picture_fs
@include_block grade_block

layout(binding=0) uniform texture2D picture;
layout(binding=0) uniform sampler picture_smp;

in vec2 at;
out vec4 frag_colour;

void main() {
    vec4 p = texture(sampler2D(picture, picture_smp), at);
    vec3 rgb = p.a > 0.0 ? apply_grade(p.rgb / p.a) : vec3(0.0);
    frag_colour = vec4(rgb * p.a, p.a) * grade_b.z;
}
@end

@program picture quad_vs picture_fs

//------------------------------------------------------------------------------
//  A MOVIE'S FRAME: DXT's colour, or a preview's, straight - HAP's alpha is not
//  premultiplied - graded, then premultiplied by its own alpha.
@fs movie_fs
@include_block grade_block

layout(binding=0) uniform texture2D picture;
layout(binding=0) uniform sampler picture_smp;

in vec2 at;
out vec4 frag_colour;

void main() {
    vec4 c = texture(sampler2D(picture, picture_smp), at);
    frag_colour = vec4(apply_grade(c.rgb) * c.a, c.a) * grade_b.z;
}
@end

@program movie quad_vs movie_fs

//------------------------------------------------------------------------------
//  HAP Q'S SCALED YCoCg, turned back to RGB as the HAP shader does (Hap.h).
@fs movie_q_fs
@include_block grade_block

layout(binding=0) uniform texture2D picture;
layout(binding=0) uniform sampler picture_smp;

in vec2 at;
out vec4 frag_colour;

void main() {
    vec4 q = texture(sampler2D(picture, picture_smp), at);
    float scale = (q.b * (255.0 / 8.0)) + 1.0;
    float co = (q.r - 0.50196078431373) / scale;
    float cg = (q.g - 0.50196078431373) / scale;
    vec3 rgb = vec3(q.a + co - cg, q.a + cg, q.a - co - cg);
    frag_colour = vec4(apply_grade(clamp(rgb, 0.0, 1.0)), 1.0) * grade_b.z;
}
@end

@program movie_q quad_vs movie_q_fs

//------------------------------------------------------------------------------
//  A MASK (Mask.h, line for line): its shape filled even-odd and feathered,
//  over the canvas-sized quad the geometry put. Two corners a vec4.
@fs mask_fs
layout(binding=1) uniform mask {
    vec4 colour;
    vec4 shape;         // canvas width, canvas height, feather, invert
    vec4 count;         // corners in the shape
    vec4 corners[32];   // x0 y0 x1 y1, ... in 0..1 of the canvas, y down
};

in vec2 at;
out vec4 frag_colour;

vec2 corner(int n) {
    vec4 pair = corners[n / 2];
    return (n % 2) == 0 ? pair.xy : pair.zw;
}

void main() {
    vec2 canvas = shape.xy;
    int points = int(count.x);
    vec2 p = vec2(at.x * canvas.x, (1.0 - at.y) * canvas.y);
    bool inside = false;
    float nearest = 1.0e30;
    int previous = points - 1;
    for (int n = 0; n < 64; ++n) {
        if (n >= points) break;
        vec2 a = corner(previous) * canvas;
        vec2 b = corner(n) * canvas;
        if (((b.y > p.y) != (a.y > p.y)) && (p.x < (a.x - b.x) * (p.y - b.y) / (a.y - b.y) + b.x)) inside = !inside;
        vec2 e = a - b;
        float span = dot(e, e);
        float t = span > 0.0 ? clamp(dot(p - b, e) / span, 0.0, 1.0) : 0.0;
        nearest = min(nearest, length(p - (b + t * e)));
        previous = n;
    }
    float feather = shape.z;
    float cover = points < 3 ? 0.0 : (feather > 0.0 ? clamp(0.5 + (inside ? nearest : -nearest) / feather, 0.0, 1.0)
                                                    : (inside ? 1.0 : 0.0));
    if (shape.w > 0.5) cover = 1.0 - cover;
    float alpha = colour.a * cover;
    frag_colour = vec4(colour.rgb * alpha, alpha);
}
@end

@program mask quad_vs mask_fs

//------------------------------------------------------------------------------
//  THE WARP (Mapping.h): an offscreen picture through the mesh, the output's
//  CDL on the way, and a dither from half-float down to eight bits, so a slow
//  fade to black does not band. Uncalibrated, it is laid by an opacity, for a
//  zone. `flip` reads a picture the GPU drew with its first row at the bottom
//  (OpenGL) the right way up.
@fs warp_fs
layout(binding=1) uniform warp {
    vec4 slope;         // r g b -
    vec4 offset;        // r g b -
    vec4 power;         // r g b -
    vec4 misc;          // saturation, opacity, calibrate (0 or 1), flip (0 or 1)
};

layout(binding=0) uniform texture2D canvas;
layout(binding=0) uniform sampler canvas_smp;

in vec2 at;
out vec4 frag_colour;

float noise(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

void main() {
    vec2 uv = vec2(at.x, misc.w > 0.5 ? 1.0 - at.y : at.y);
    vec3 c = texture(sampler2D(canvas, canvas_smp), uv).rgb;
    if (misc.z < 0.5) {
        frag_colour = vec4(c * misc.y, misc.y);
        return;
    }
    c = pow(clamp(c * slope.rgb + offset.rgb, 0.0, 1.0), power.rgb);
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = clamp(luma + misc.x * (c - luma), 0.0, 1.0);
    c += (noise(gl_FragCoord.xy) - 0.5) / 255.0;
    frag_colour = vec4(c, 1.0);
}
@end

@program warp mesh_vs warp_fs
