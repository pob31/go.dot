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
//  is one draw, so nothing is written to a buffer between two of them. The
//  block is not called "quad": Metal reserves the name, and the vertex shader
//  every quad shares would not compile on a Mac (2026-10-08, an M4 Pro).
@vs quad_vs
layout(binding=0) uniform corners {
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
//  texel a step, red, green and blue. Before it, THE KEY (Key.h, line for
//  line; namespace draft 61): a point's measure - its brightness, or for a
//  colour key its colour's distance from the key colour's in the Cb Cr plane,
//  each relative to its own brightness (at least a tenth) so a shadow is still
//  the screen, the key nought and grey one, the spill taken off along the
//  key's direction - then
//  the ramp, which multiplies its alpha; a matte shows the cue's paint and is
//  not graded.
@block grade_block
layout(binding=1) uniform grade {
    vec4 grade_a;       // gamma, contrast, saturation, curves (0 or 1)
    vec4 grade_b;       // cos (hue), sin (hue), opacity, alpha not read (1)
    vec4 key_a;         // mode (0 none, 1 luma, 2 matte, 3 colour), level, softness, invert - 0..1
    vec4 key_b;         // the key colour, or a matte's paint; the spill 0..1
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

void apply_key(inout vec3 c, inout float a) {
    int mode = int(key_a.x + 0.5);
    if (mode == 0) return;
    vec3 weights = vec3(0.2126, 0.7152, 0.0722);
    float y = dot(c, weights);
    float m = y;
    if (mode == 3) {
        float ky = dot(key_b.rgb, weights);
        vec2 kc = vec2((key_b.b - ky) / 1.8556, (key_b.r - ky) / 1.5748);
        vec2 pc = vec2((c.b - y) / 1.8556, (c.r - y) / 1.5748);
        float key_scale = 1.0 / max(ky, 0.1);
        float norm = max(length(kc) * key_scale, 0.001);
        m = length(pc / max(y, 0.1) - kc * key_scale) / norm;
        vec2 dir = kc / max(length(kc), 0.000001);
        pc -= dir * (max(0.0, dot(pc, dir)) * clamp(key_b.w, 0.0, 1.0));
        float r = y + 1.5748 * pc.y;
        float b = y + 1.8556 * pc.x;
        float g = (y - 0.2126 * r - 0.0722 * b) / 0.7152;
        c = clamp(vec3(r, g, b), 0.0, 1.0);
    }
    float k = key_a.z > 0.0 ? clamp((m - key_a.y) / key_a.z + 0.5, 0.0, 1.0) : (m >= key_a.y ? 1.0 : 0.0);
    if (key_a.w > 0.5) k = 1.0 - k;
    if (mode == 2) c = key_b.rgb;
    a *= k;
}

//  The key, then the grade - a matte shows its paint as it is.
void key_and_grade(inout vec3 c, inout float a) {
    apply_key(c, a);
    if (int(key_a.x + 0.5) != 2) c = apply_grade(c);
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
//  A PICTURE: read premultiplied, as the store holds it; keyed and graded on
//  its own colour; premultiplied again and taken down by the layer's opacity.
//  Its alpha not read (61, AKE), it is solid - black where it was clear, the
//  colour there gone once the file was read.
@fs picture_fs
@include_block grade_block

layout(binding=0) uniform texture2D picture;
layout(binding=0) uniform sampler picture_smp;

in vec2 at;
out vec4 frag_colour;

void main() {
    vec4 p = texture(sampler2D(picture, picture_smp), at);
    float a = grade_b.w > 0.5 ? 1.0 : p.a;
    vec3 rgb = grade_b.w > 0.5 ? p.rgb : (p.a > 0.0 ? p.rgb / p.a : vec3(0.0));
    key_and_grade(rgb, a);
    frag_colour = vec4(rgb * a, a) * grade_b.z;
}
@end

@program picture quad_vs picture_fs

//------------------------------------------------------------------------------
//  A MOVIE'S FRAME: DXT's colour, or a preview's, straight - HAP's alpha is not
//  premultiplied - keyed and graded, then premultiplied by its own alpha. And a
//  capture's picture (namespace draft 44, YC), whose alpha is read only when its
//  input says it sends one (61, AKF; grade_b.w): many programs send BGRX, its
//  fourth byte whatever it happens to be.
@fs movie_fs
@include_block grade_block

layout(binding=0) uniform texture2D picture;
layout(binding=0) uniform sampler picture_smp;

in vec2 at;
out vec4 frag_colour;

void main() {
    vec4 c = texture(sampler2D(picture, picture_smp), at);
    float a = grade_b.w > 0.5 ? 1.0 : c.a;
    vec3 rgb = c.rgb;
    key_and_grade(rgb, a);
    frag_colour = vec4(rgb * a, a) * grade_b.z;
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
    vec3 rgb = clamp(vec3(q.a + co - cg, q.a + cg, q.a - co - cg), 0.0, 1.0);
    float a = 1.0;
    key_and_grade(rgb, a);
    frag_colour = vec4(rgb * a, a) * grade_b.z;
}
@end

@program movie_q quad_vs movie_q_fs

//------------------------------------------------------------------------------
//  AN OUTLINE'S COVER (Mask.h, line for line; namespace draft 61): a mask's, a
//  blur cue's. Asked at a target's own pixel: the point on the canvas from its
//  middle, y up - the target's rows run down (Direct3D, Metal) or up (OpenGL),
//  said by `top-left` - carried back onto the frame the layer's geometry moved
//  by two rows of an affine map the Painter works out from `Placement`; off
//  that frame, nothing, or everything turned inside out (AKP); on it, the
//  shape filled even-odd and feathered. A blur cue with no outline covers all.
@block cover_block
layout(binding=2) uniform cover {
    vec4 cover_shape;       // target width, target height, feather, invert
    vec4 cover_count;       // corners, whole (1), top-left origin (1), -
    vec4 cover_u;           // frame u = x * u.x + y * u.y + u.z, 0..1 left to right
    vec4 cover_v;           // frame v = x * v.x + y * v.y + v.z, 0..1 bottom to top
    vec4 cover_corners[32]; // x0 y0 x1 y1, ... in 0..1 of the canvas, y down
};

vec2 cover_corner(int n) {
    vec4 pair = cover_corners[n >> 1];
    return (n & 1) == 0 ? pair.xy : pair.zw;
}

float cover_at(vec2 frag) {
    if (cover_count.y > 0.5) return 1.0;
    vec2 canvas = cover_shape.xy;
    float x = frag.x - 0.5 * canvas.x;
    float y = cover_count.z > 0.5 ? 0.5 * canvas.y - frag.y : frag.y - 0.5 * canvas.y;
    vec2 uv = vec2(x * cover_u.x + y * cover_u.y + cover_u.z, x * cover_v.x + y * cover_v.y + cover_v.z);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return cover_shape.w > 0.5 ? 1.0 : 0.0;
    int points = clamp(int(cover_count.x), 0, 64);
    vec2 p = vec2(uv.x * canvas.x, (1.0 - uv.y) * canvas.y);
    bool inside = false;
    float nearest = 1.0e30;
    int previous = points - 1;
    //  Bounded by the shape's own corners, never a fixed 64 a compiler may
    //  unroll: Mesa's software rasteriser ran out of memory doing so (CI, 2026-10-08).
    for (int n = 0; n < points; ++n) {
        vec2 a = cover_corner(previous) * canvas;
        vec2 b = cover_corner(n) * canvas;
        if (((b.y > p.y) != (a.y > p.y)) && (p.x < (a.x - b.x) * (p.y - b.y) / (a.y - b.y) + b.x)) inside = !inside;
        vec2 e = a - b;
        float span = dot(e, e);
        float t = span > 0.0 ? clamp(dot(p - b, e) / span, 0.0, 1.0) : 0.0;
        nearest = min(nearest, length(p - (b + t * e)));
        previous = n;
    }
    float feather = cover_shape.z;
    float cover = points < 3 ? 0.0 : (feather > 0.0 ? clamp(0.5 + (inside ? nearest : -nearest) / feather, 0.0, 1.0)
                                                    : (inside ? 1.0 : 0.0));
    if (cover_shape.w > 0.5) cover = 1.0 - cover;
    return cover;
}
@end

//------------------------------------------------------------------------------
//  A MASK: its colour where its outline covers, over the whole target.
@fs mask_fs
@include_block cover_block
layout(binding=1) uniform mask {
    vec4 colour;
};

in vec2 at;
out vec4 frag_colour;

void main() {
    float alpha = colour.a * cover_at(gl_FragCoord.xy);
    frag_colour = vec4(colour.rgb * alpha, alpha);
}
@end

@program mask quad_vs mask_fs

//------------------------------------------------------------------------------
//  A BLUR CUE (Blur.h's pyramid, step for step; namespace draft 61, AKO). Every
//  read is a texel by its integer place - the target's own pixel - so a pass
//  reads and writes in one orientation whichever way the backend runs its rows.
//
//  HALVED: each point the average of the four beneath it, the edges held.
@fs down_fs
layout(binding=1) uniform down {
    vec4 down_size;         // the level read: width, height
};

layout(binding=0) uniform texture2D level;
layout(binding=0) uniform sampler level_smp;

in vec2 at;
out vec4 frag_colour;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) * 2;
    ivec2 most = ivec2(down_size.xy) - 1;
    vec3 sum = texelFetch(sampler2D(level, level_smp), min(p, most), 0).rgb
             + texelFetch(sampler2D(level, level_smp), min(p + ivec2(1, 0), most), 0).rgb
             + texelFetch(sampler2D(level, level_smp), min(p + ivec2(0, 1), most), 0).rgb
             + texelFetch(sampler2D(level, level_smp), min(p + ivec2(1, 1), most), 0).rgb;
    frag_colour = vec4(sum * 0.25, 1.0);
}
@end

@program down quad_vs down_fs

//  ONE PASS OF THE GAUSSIAN, across or down: the taps to three spreads,
//  summing to one, the edges held.
@fs blur_fs
layout(binding=1) uniform blur {
    vec4 blur_step;         // direction x, direction y, spread, reach
    vec4 blur_size;         // the level: width, height
};

layout(binding=0) uniform texture2D level;
layout(binding=0) uniform sampler level_smp;

in vec2 at;
out vec4 frag_colour;

void main() {
    ivec2 here = ivec2(gl_FragCoord.xy);
    ivec2 most = ivec2(blur_size.xy) - 1;
    ivec2 dir = ivec2(blur_step.xy);
    float spread = max(blur_step.z, 0.000001);
    int reach = int(blur_step.w + 0.5);
    vec3 sum = texelFetch(sampler2D(level, level_smp), here, 0).rgb;
    float total = 1.0;
    for (int n = 1; n <= reach; ++n) {
        float w = exp(-0.5 * float(n * n) / (spread * spread));
        sum += w * (texelFetch(sampler2D(level, level_smp), clamp(here + dir * n, ivec2(0), most), 0).rgb
                  + texelFetch(sampler2D(level, level_smp), clamp(here - dir * n, ivec2(0), most), 0).rgb);
        total += 2.0 * w;
    }
    frag_colour = vec4(sum / total, 1.0);
}
@end

@program blur quad_vs blur_fs

//  LAID BACK on the canvas: the blurred level read by straight lines between
//  its points' middles, through the cue's cover times its opacity.
@fs blur_mix_fs
@include_block cover_block
layout(binding=1) uniform blur_mix {
    vec4 mix_a;             // 2^halvings, the level's width, height, opacity
};

layout(binding=0) uniform texture2D level;
layout(binding=0) uniform sampler level_smp;

in vec2 at;
out vec4 frag_colour;

void main() {
    vec2 f = gl_FragCoord.xy / mix_a.x - 0.5;
    ivec2 i0 = ivec2(floor(f));
    vec2 t = f - vec2(i0);
    ivec2 most = ivec2(mix_a.yz) - 1;
    vec3 a = texelFetch(sampler2D(level, level_smp), clamp(i0, ivec2(0), most), 0).rgb;
    vec3 b = texelFetch(sampler2D(level, level_smp), clamp(i0 + ivec2(1, 0), ivec2(0), most), 0).rgb;
    vec3 c = texelFetch(sampler2D(level, level_smp), clamp(i0 + ivec2(0, 1), ivec2(0), most), 0).rgb;
    vec3 d = texelFetch(sampler2D(level, level_smp), clamp(i0 + ivec2(1, 1), ivec2(0), most), 0).rgb;
    vec3 blurred = mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
    float m = mix_a.w * cover_at(gl_FragCoord.xy);
    frag_colour = vec4(blurred * m, m);
}
@end

@program blur_mix quad_vs blur_mix_fs

//------------------------------------------------------------------------------
//  THE WARP (Mapping.h): an offscreen picture through the mesh, the output's
//  CDL on the way, and a dither from half-float down to eight bits, so a slow
//  fade to black does not band. Uncalibrated, it is laid by an opacity, for a
//  zone. `flip` reads a picture the GPU drew with its first row at the bottom
//  (OpenGL) the right way up.
@fs warp_fs
layout(binding=1) uniform warp {
    vec4 slope;         // r g b, and 1 to keep the canvas's alpha (a keyed card output, §71)
    vec4 offset;        // r g b -
    vec4 power;         // r g b -
    vec4 misc;          // saturation, opacity, calibrate (0 or 1), flip (0 or 1)
};

layout(binding=0) uniform texture2D canvas;
layout(binding=0) uniform sampler canvas_smp;

in vec2 at;
out vec4 frag_colour;

float noise(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

//  KEPT ALPHA (namespace draft §71, APG): for a keyed card output alone, the
//  canvas's own alpha goes out with its colour, premultiplied as the canvas
//  holds it; laid, by its opacity too; calibrated, the CDL on the colour the
//  alpha covers. Every other output's arithmetic is as it was.
void main() {
    vec2 uv = vec2(at.x, misc.w > 0.5 ? 1.0 - at.y : at.y);
    vec4 t = texture(sampler2D(canvas, canvas_smp), uv);
    vec3 c = t.rgb;
    bool keep = slope.w > 0.5;
    if (misc.z < 0.5) {
        frag_colour = vec4(c * misc.y, (keep ? clamp(t.a, 0.0, 1.0) : 1.0) * misc.y);
        return;
    }
    float a = keep ? clamp(t.a, 0.0, 1.0) : 1.0;
    if (keep)
        c = a > 0.0 ? c / a : vec3(0.0);
    c = pow(clamp(c * slope.rgb + offset.rgb, 0.0, 1.0), power.rgb);
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = clamp(luma + misc.x * (c - luma), 0.0, 1.0);
    c += (noise(gl_FragCoord.xy) - 0.5) / 255.0;
    frag_colour = keep ? vec4(c * a, a) : vec4(c, 1.0);
}
@end

@program warp mesh_vs warp_fs

//------------------------------------------------------------------------------
//  A PICTURE AS A CARD TAKES IT, IN 10-BIT YUV (namespace draft §71, APD):
//  each texel of the target one 32-bit word of v210 - three 10-bit samples,
//  the first in the low bits - its four bytes the texel's red, green, blue and
//  alpha. Six pixels to four words: Cb Y Cr Y Cb Y Cr Y Cb Y Cr Y. BT.709 at
//  legal levels, 64 to 940 for black to white, each chroma sample the mean of
//  its two pixels; past the picture's right edge, its last pixel. Every read a
//  texel by its integer place, as the blur's are, so a row is read and written
//  in one orientation whichever way the backend runs its rows.
@fs v210_fs
layout(binding=1) uniform v210 {
    vec4 v210_size;         // the picture read: width, height
};

layout(binding=0) uniform texture2D card_picture;
layout(binding=0) uniform sampler card_picture_smp;

in vec2 at;
out vec4 frag_colour;

vec3 rgb_at(int x, int y) {
    int most = int(v210_size.x) - 1;
    return clamp(texelFetch(sampler2D(card_picture, card_picture_smp), ivec2(min(x, most), y), 0).rgb, 0.0, 1.0);
}

float luma_of(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

uint sample_of(int group, int i, int y) {
    int first = group * 6;

    if ((i & 1) == 1) {
        float l = luma_of(rgb_at(first + (i - 1) / 2, y));
        return uint(clamp(floor(64.0 + 876.0 * l + 0.5), 4.0, 1019.0));
    }

    int pair = i / 4;
    vec3 c = (rgb_at(first + 2 * pair, y) + rgb_at(first + 2 * pair + 1, y)) * 0.5;
    float l = luma_of(c);
    float d = (i % 4) == 0 ? (c.b - l) / 1.8556 : (c.r - l) / 1.5748;
    return uint(clamp(floor(512.0 + 896.0 * d + 0.5), 4.0, 1019.0));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int group = p.x / 4;
    int k = p.x % 4;
    uint w = sample_of(group, 3 * k, p.y) | (sample_of(group, 3 * k + 1, p.y) << 10u) | (sample_of(group, 3 * k + 2, p.y) << 20u);
    frag_colour = vec4(float(w & 255u), float((w >> 8u) & 255u), float((w >> 16u) & 255u), float((w >> 24u) & 255u)) / 255.0;
}
@end

@program v210 quad_vs v210_fs
