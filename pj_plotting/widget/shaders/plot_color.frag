// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

// Edge antialiasing and dashes for PlotRhiCanvas geometry (see plot_rhi::Vertex).
// v_across runs from -1 to 1 across a stroke or a box and from 0 to 1 from a
// fan's centre to its rim; it reaches 1 half a device pixel outside the true
// edge, so the coverage ramp spans one device pixel centred on it.
// v_code 0 is solid. 1-4 are Qt's DashLine, DotLine, DashDotLine and
// DashDotDotLine, with v_second the path distance in 64ths of a pen width:
// cut per pixel centre for a cosmetic pen, as QCosmeticStroker steps its
// pattern, and antialiased when square caps lengthen each dash by half the
// width (code + 5, wide pens). 10 is an axis-aligned box: v_second is its
// second axis, 0..65535 for -1..1.

layout(location = 0) in vec4 v_color;
layout(location = 1) in float v_across;
layout(location = 2) in float v_code;
layout(location = 3) in float v_second;

layout(location = 0) out vec4 frag_color;

// Signed distance in pen widths from `u` into the nearest dash, negative in a
// gap. Patterns in pen widths: 4 on 2 off; 1 on 2 off; 4 2 1 2; 4 2 1 2 1 2.
float dashInside(float style, float u, float cap) {
    float period = style < 1.5 ? 6.0 : (style < 2.5 ? 3.0 : (style < 3.5 ? 9.0 : 12.0));
    float first = (style > 1.5 && style < 2.5) ? 1.0 : 4.0;
    float phase = mod(u, period);
    float inside = min(phase + cap, first + cap - phase);
    // The next period's first dash, whose cap reaches back before the period ends.
    inside = max(inside, phase - period + cap);
    if (style > 2.5) {
        inside = max(inside, min(phase - 6.0 + cap, 7.0 + cap - phase));
    }
    if (style > 3.5) {
        inside = max(inside, min(phase - 9.0 + cap, 10.0 + cap - phase));
    }
    return inside;
}

// Coverage from the distance to a rim at |t| = 1, t being linear on screen.
float rimCoverage(float t) {
    float rate = length(vec2(dFdx(t), dFdy(t)));
    return clamp((1.0 - abs(t)) / max(rate, 1e-6), 0.0, 1.0);
}

void main() {
    float code = floor(v_code + 0.5);
    float coverage = rimCoverage(v_across);
    // code is constant over a primitive, so these branches are uniform across
    // each pixel quad and their derivatives are defined.
    if (code > 9.5) {
        coverage *= rimCoverage(v_second / 32767.5 - 1.0);
    } else if (code > 0.5) {
        float dash = v_second * 0.015625;  // 1/64 pen width per step
        if (code > 4.5) {
            float rate = length(vec2(dFdx(dash), dFdy(dash)));
            coverage *= clamp(dashInside(code - 5.0, dash, 0.5) / max(rate, 1e-6) + 0.5, 0.0, 1.0);
        } else {
            coverage *= step(0.0, dashInside(code, dash, 0.0));
        }
    }
    if (coverage <= 0.0) {
        discard;
    }
    frag_color = vec4(v_color.rgb, v_color.a * coverage);
}
