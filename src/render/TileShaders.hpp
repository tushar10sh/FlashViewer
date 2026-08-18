#pragma once

// Embedded GLSL shader sources for the raster tile renderer.
//
// Extracted into a header so the exact shaders compiled by TileRenderer are also
// compilable by the render tests (tests/test_render_nodata.cpp,
// tests/test_render_pipeline.cpp, tests/test_render_resampling.cpp).
//
// No-data / non-finite handling (FR-RND-8) and the edge-bleed fix:
//   * The fragment's OWN texel is classified with texelFetch (exact, bypasses
//     filtering): non-finite (NaN/±Inf) is ALWAYS no-data, and a declared/overridden
//     sentinel is matched with an epsilon compare. This handles the interior of the
//     no-data region and the GL_NEAREST (zoomed-in) path.
//   * Interpolation edge bleed: when the displayed value is filtered (bilinear or
//     bicubic), a fragment whose sampling-kernel FOOTPRINT touches a no-data /
//     non-finite texel is itself rendered as no-data — set the extrapolated value to
//     no-data rather than letting interpolation smear it. Bilinear uses a 2×2 guard
//     (textureGather); bicubic uses a 4×4 guard (footprintBad4x4). This replaces the
//     old "sanitize NaN → u_min" which actually produced a white edge line.
//
// Display resampling (FR-RND-10), selected by u_resample:
//   0 = bilinear (hardware GL_LINEAR; default — backward compatible)
//   1 = bicubic 2×2 "fast": cubic B-SPLINE via 4 hardware-bilinear taps
//       (Sigg & Hadwiger / GPU Gems 2 ch.20). Smooth; positive weights → an exact,
//       numerically-stable 4-tap reformulation. Requires GL_LINEAR filtering.
//   2 = bicubic 4×4 "exact": CATMULL-ROM via 16 texelFetch taps. Interpolating
//       (passes through samples) → sharper / detail-preserving; heavier.
//   (The fast path uses B-spline rather than Catmull-Rom because the 4-tap fold is
//    only valid for non-negative weights; Catmull-Rom's negative lobes break it.)

namespace tileshaders {

inline constexpr const char* kTileVert = R"glsl(
#version 410 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
uniform mat4 u_view_proj;
uniform vec4 u_tile_extent;   // xmin, ymin, xmax, ymax
out vec2 v_uv;
void main() {
    vec2 geo = mix(u_tile_extent.xy, u_tile_extent.zw, a_pos);
    gl_Position = u_view_proj * vec4(geo, 0.0, 1.0);
    v_uv = a_uv;
}
)glsl";

inline constexpr const char* kRgbFrag = R"glsl(
#version 410 core
uniform sampler2D u_band_r, u_band_g, u_band_b;
uniform float u_min_r, u_max_r, u_min_g, u_max_g, u_min_b, u_max_b;
uniform float u_opacity;
uniform float u_has_nodata, u_nodata_value;
uniform vec4 u_nodata_color;
uniform float u_bleed_guard;   // 1 = bilinear filtering active → run 2×2 guard
uniform int   u_resample;      // 0 bilinear, 1 bicubic2 (B-spline), 2 bicubic4 (Catmull-Rom)
uniform vec4  u_inner;         // inner rect (x,y,w,h) in texels within the aproned tile texture; (0,0,0,0)=whole texture
uniform int   u_filter_mode;   // 0=None, 1=Checkerboard, 2=VertSwipe, 3=HorizSwipe
uniform int   u_filter_role;   // 0=Normal, 1=Top Layer, 2=Background Layer
uniform float u_check_size;
uniform float u_swipe_pos;
uniform float u_viewport_width;
uniform float u_viewport_height;
in vec2 v_uv; out vec4 frag_color;

#define EMIT_NODATA { if (u_nodata_color.a < 0.01) discard; frag_color = u_nodata_color; return; }

float s(float v, float lo, float hi) { return clamp((v-lo)/max(hi-lo,1e-6),0.,1.); }

bool nbrBad(sampler2D t, vec2 uv, float hasNd, float ndVal) {
    vec4 g = textureGather(t, uv);
    if (isnan(g.x)||isinf(g.x)||isnan(g.y)||isinf(g.y)||
        isnan(g.z)||isinf(g.z)||isnan(g.w)||isinf(g.w)) return true;
    if (hasNd > 0.5) { float e = max(abs(ndVal)*1e-5, 1e-10);
        if (any(lessThan(abs(g - vec4(ndVal)), vec4(e)))) return true; }
    return false;
}

bool footprintBad4x4(sampler2D t, vec2 uv, float hasNd, float ndVal) {
    ivec2 isz = textureSize(t, 0);
    vec2 tc = uv * vec2(isz) - 0.5;
    ivec2 base = ivec2(floor(tc));
    float e = max(abs(ndVal)*1e-5, 1e-10);
    for (int j = -1; j <= 2; ++j) for (int i = -1; i <= 2; ++i) {
        ivec2 p = clamp(base + ivec2(i,j), ivec2(0), isz - ivec2(1));
        float v = texelFetch(t, p, 0).r;
        if (isnan(v) || isinf(v)) return true;
        if (hasNd > 0.5 && abs(v - ndVal) < e) return true;
    }
    return false;
}

vec4 bsplineW(float f) {
    float f2 = f*f, f3 = f2*f;
    return vec4((-f3 + 3.0*f2 - 3.0*f + 1.0) / 6.0,
                ( 3.0*f3 - 6.0*f2 + 4.0)      / 6.0,
                (-3.0*f3 + 3.0*f2 + 3.0*f + 1.0) / 6.0,
                ( f3)                          / 6.0);
}
vec4 catmullW(float f) {
    float f2 = f*f, f3 = f2*f;
    return vec4(0.5*(-f3 + 2.0*f2 - f),
                0.5*( 3.0*f3 - 5.0*f2 + 2.0),
                0.5*(-3.0*f3 + 4.0*f2 + f),
                0.5*( f3 - f2));
}

// Cubic B-spline via 4 hardware-bilinear taps (positive weights → exact fold).
float bicubic2(sampler2D t, vec2 uv) {
    vec2 sz = vec2(textureSize(t, 0));
    vec2 tc = uv * sz - 0.5;
    vec2 base = floor(tc);
    vec2 f = tc - base;
    vec4 wx = bsplineW(f.x), wy = bsplineW(f.y);
    float sAx = wx.x+wx.y, sBx = wx.z+wx.w;
    float sAy = wy.x+wy.y, sBy = wy.z+wy.w;
    float px0 = base.x - 1.0 + wx.y/sAx, px1 = base.x + 1.0 + wx.w/sBx;
    float py0 = base.y - 1.0 + wy.y/sAy, py1 = base.y + 1.0 + wy.w/sBy;
    float c00 = texture(t, vec2(px0+0.5, py0+0.5)/sz).r;
    float c10 = texture(t, vec2(px1+0.5, py0+0.5)/sz).r;
    float c01 = texture(t, vec2(px0+0.5, py1+0.5)/sz).r;
    float c11 = texture(t, vec2(px1+0.5, py1+0.5)/sz).r;
    return (c00*sAx + c10*sBx)*sAy + (c01*sAx + c11*sBx)*sBy;
}

// Catmull-Rom via 16 exact texelFetch taps.
float bicubic4(sampler2D t, vec2 uv) {
    ivec2 isz = textureSize(t, 0);
    vec2 tc = uv * vec2(isz) - 0.5;
    ivec2 base = ivec2(floor(tc));
    vec2 f = tc - vec2(base);
    vec4 wx = catmullW(f.x), wy = catmullW(f.y);
    float acc = 0.0;
    for (int j = -1; j <= 2; ++j) for (int i = -1; i <= 2; ++i) {
        ivec2 p = clamp(base + ivec2(i,j), ivec2(0), isz - ivec2(1));
        acc += texelFetch(t, p, 0).r * wx[i+1] * wy[j+1];
    }
    return acc;
}

float sampleBand(sampler2D t, vec2 uv) {
    if (u_resample == 2) return bicubic4(t, uv);
    if (u_resample == 1) return bicubic2(t, uv);
    return texture(t, uv).r;
}
bool footprintBad(sampler2D t, vec2 uv) {
    if (u_resample != 0) return footprintBad4x4(t, uv, u_has_nodata, u_nodata_value);
    if (u_bleed_guard > 0.5) return nbrBad(t, uv, u_has_nodata, u_nodata_value);
    return false;
}

void main() {
    if (u_filter_mode == 1) {
        ivec2 p = ivec2(gl_FragCoord.x, gl_FragCoord.y) / max(int(u_check_size), 1);
        bool is_even = ((p.x + p.y) % 2) == 0;
        if (u_filter_role == 1 && !is_even) discard;
        if (u_filter_role == 2 && is_even) discard;
    } else if (u_filter_mode == 2) {
        float normX = gl_FragCoord.x / max(u_viewport_width, 1.0);
        if (u_filter_role == 1 && normX > u_swipe_pos) discard;
        if (u_filter_role == 2 && normX <= u_swipe_pos) discard;
    } else if (u_filter_mode == 3) {
        float normY = (u_viewport_height - gl_FragCoord.y) / max(u_viewport_height, 1.0);
        if (u_filter_role == 1 && normY > u_swipe_pos) discard;
        if (u_filter_role == 2 && normY <= u_swipe_pos) discard;
    }

    // Map v_uv to the inner tile rect inside the aproned texture (seamless — FR-RND-10).
    vec2  isz = vec2(textureSize(u_band_r, 0));
    vec2  innerSz = (u_inner.zw == vec2(0.0)) ? isz : u_inner.zw;
    vec2  tcoord = u_inner.xy + v_uv * innerSz;
    vec2  nUV = tcoord / isz;
    ivec2 sz = ivec2(isz);
    // Exact texel (bypasses filtering) for sentinel/non-finite checks.
    ivec2 tc = clamp(ivec2(tcoord), ivec2(0), sz - ivec2(1));
    float er = texelFetch(u_band_r, tc, 0).r;
    float eg = texelFetch(u_band_g, tc, 0).r;
    float eb = texelFetch(u_band_b, tc, 0).r;

    if (isnan(er)||isinf(er)||isnan(eg)||isinf(eg)||isnan(eb)||isinf(eb)) EMIT_NODATA
    if (u_has_nodata > 0.5) {
        float e = max(abs(u_nodata_value)*1e-5, 1e-10);
        if (abs(er-u_nodata_value)<e || abs(eg-u_nodata_value)<e || abs(eb-u_nodata_value)<e) EMIT_NODATA
    }

    // Interpolation edge-bleed / footprint guard (set extrapolated → no-data).
    if (footprintBad(u_band_r, nUV) || footprintBad(u_band_g, nUV) || footprintBad(u_band_b, nUV)) EMIT_NODATA

    float r = sampleBand(u_band_r, nUV);
    float g = sampleBand(u_band_g, nUV);
    float b = sampleBand(u_band_b, nUV);
    if (isnan(r)||isinf(r)||isnan(g)||isinf(g)||isnan(b)||isinf(b)) EMIT_NODATA  // defensive
    frag_color = vec4(s(r,u_min_r,u_max_r), s(g,u_min_g,u_max_g), s(b,u_min_b,u_max_b), u_opacity);
}
)glsl";

inline constexpr const char* kGrayFrag = R"glsl(
#version 410 core
uniform sampler2D u_band;
uniform sampler1D u_colormap;
uniform float u_min, u_max, u_opacity, u_invert;
uniform float u_has_nodata, u_nodata_value;
uniform vec4 u_nodata_color;
uniform float u_bleed_guard;   // 1 = bilinear filtering active → run 2×2 guard
uniform int   u_resample;      // 0 bilinear, 1 bicubic2 (B-spline), 2 bicubic4 (Catmull-Rom)
uniform vec4  u_inner;         // inner rect (x,y,w,h) in texels within the aproned tile texture; (0,0,0,0)=whole texture
uniform int   u_filter_mode;   // 0=None, 1=Checkerboard, 2=VertSwipe, 3=HorizSwipe
uniform int   u_filter_role;   // 0=Normal, 1=Top Layer, 2=Background Layer
uniform float u_check_size;
uniform float u_swipe_pos;
uniform float u_viewport_width;
uniform float u_viewport_height;
in vec2 v_uv; out vec4 frag_color;

#define EMIT_NODATA { if (u_nodata_color.a < 0.01) discard; frag_color = u_nodata_color; return; }

bool nbrBad(sampler2D t, vec2 uv, float hasNd, float ndVal) {
    vec4 g = textureGather(t, uv);
    if (isnan(g.x)||isinf(g.x)||isnan(g.y)||isinf(g.y)||
        isnan(g.z)||isinf(g.z)||isnan(g.w)||isinf(g.w)) return true;
    if (hasNd > 0.5) { float e = max(abs(ndVal)*1e-5, 1e-10);
        if (any(lessThan(abs(g - vec4(ndVal)), vec4(e)))) return true; }
    return false;
}

bool footprintBad4x4(sampler2D t, vec2 uv, float hasNd, float ndVal) {
    ivec2 isz = textureSize(t, 0);
    vec2 tc = uv * vec2(isz) - 0.5;
    ivec2 base = ivec2(floor(tc));
    float e = max(abs(ndVal)*1e-5, 1e-10);
    for (int j = -1; j <= 2; ++j) for (int i = -1; i <= 2; ++i) {
        ivec2 p = clamp(base + ivec2(i,j), ivec2(0), isz - ivec2(1));
        float v = texelFetch(t, p, 0).r;
        if (isnan(v) || isinf(v)) return true;
        if (hasNd > 0.5 && abs(v - ndVal) < e) return true;
    }
    return false;
}

vec4 bsplineW(float f) {
    float f2 = f*f, f3 = f2*f;
    return vec4((-f3 + 3.0*f2 - 3.0*f + 1.0) / 6.0,
                ( 3.0*f3 - 6.0*f2 + 4.0)      / 6.0,
                (-3.0*f3 + 3.0*f2 + 3.0*f + 1.0) / 6.0,
                ( f3)                          / 6.0);
}
vec4 catmullW(float f) {
    float f2 = f*f, f3 = f2*f;
    return vec4(0.5*(-f3 + 2.0*f2 - f),
                0.5*( 3.0*f3 - 5.0*f2 + 2.0),
                0.5*(-3.0*f3 + 4.0*f2 + f),
                0.5*( f3 - f2));
}

float bicubic2(sampler2D t, vec2 uv) {
    vec2 sz = vec2(textureSize(t, 0));
    vec2 tc = uv * sz - 0.5;
    vec2 base = floor(tc);
    vec2 f = tc - base;
    vec4 wx = bsplineW(f.x), wy = bsplineW(f.y);
    float sAx = wx.x+wx.y, sBx = wx.z+wx.w;
    float sAy = wy.x+wy.y, sBy = wy.z+wy.w;
    float px0 = base.x - 1.0 + wx.y/sAx, px1 = base.x + 1.0 + wx.w/sBx;
    float py0 = base.y - 1.0 + wy.y/sAy, py1 = base.y + 1.0 + wy.w/sBy;
    float c00 = texture(t, vec2(px0+0.5, py0+0.5)/sz).r;
    float c10 = texture(t, vec2(px1+0.5, py0+0.5)/sz).r;
    float c01 = texture(t, vec2(px0+0.5, py1+0.5)/sz).r;
    float c11 = texture(t, vec2(px1+0.5, py1+0.5)/sz).r;
    return (c00*sAx + c10*sBx)*sAy + (c01*sAx + c11*sBx)*sBy;
}

float bicubic4(sampler2D t, vec2 uv) {
    ivec2 isz = textureSize(t, 0);
    vec2 tc = uv * vec2(isz) - 0.5;
    ivec2 base = ivec2(floor(tc));
    vec2 f = tc - vec2(base);
    vec4 wx = catmullW(f.x), wy = catmullW(f.y);
    float acc = 0.0;
    for (int j = -1; j <= 2; ++j) for (int i = -1; i <= 2; ++i) {
        ivec2 p = clamp(base + ivec2(i,j), ivec2(0), isz - ivec2(1));
        acc += texelFetch(t, p, 0).r * wx[i+1] * wy[j+1];
    }
    return acc;
}

float sampleBand(sampler2D t, vec2 uv) {
    if (u_resample == 2) return bicubic4(t, uv);
    if (u_resample == 1) return bicubic2(t, uv);
    return texture(t, uv).r;
}
bool footprintBad(sampler2D t, vec2 uv) {
    if (u_resample != 0) return footprintBad4x4(t, uv, u_has_nodata, u_nodata_value);
    if (u_bleed_guard > 0.5) return nbrBad(t, uv, u_has_nodata, u_nodata_value);
    return false;
}

void main() {
    if (u_filter_mode == 1) {
        ivec2 p = ivec2(gl_FragCoord.x, gl_FragCoord.y) / max(int(u_check_size), 1);
        bool is_even = ((p.x + p.y) % 2) == 0;
        if (u_filter_role == 1 && !is_even) discard;
        if (u_filter_role == 2 && is_even) discard;
    } else if (u_filter_mode == 2) {
        float normX = gl_FragCoord.x / max(u_viewport_width, 1.0);
        if (u_filter_role == 1 && normX > u_swipe_pos) discard;
        if (u_filter_role == 2 && normX <= u_swipe_pos) discard;
    } else if (u_filter_mode == 3) {
        float normY = (u_viewport_height - gl_FragCoord.y) / max(u_viewport_height, 1.0);
        if (u_filter_role == 1 && normY > u_swipe_pos) discard;
        if (u_filter_role == 2 && normY <= u_swipe_pos) discard;
    }

    // Map the quad's v_uv to the inner (logical) tile rect inside the aproned
    // texture, so sampling reads real neighbours at tile edges (seamless — FR-RND-10).
    vec2  isz = vec2(textureSize(u_band, 0));
    vec2  innerSz = (u_inner.zw == vec2(0.0)) ? isz : u_inner.zw;
    vec2  tcoord = u_inner.xy + v_uv * innerSz;   // texel-space coord
    vec2  nUV = tcoord / isz;                      // normalized uv for samplers
    ivec2 sz = ivec2(isz);
    ivec2 tc = clamp(ivec2(tcoord), ivec2(0), sz - ivec2(1));
    float exact = texelFetch(u_band, tc, 0).r;

    if (isnan(exact) || isinf(exact)) EMIT_NODATA
    if (u_has_nodata > 0.5 &&
        abs(exact - u_nodata_value) < max(abs(u_nodata_value)*1e-5, 1e-10)) EMIT_NODATA

    if (footprintBad(u_band, nUV)) EMIT_NODATA

    float val = sampleBand(u_band, nUV);
    if (isnan(val) || isinf(val)) EMIT_NODATA  // defensive
    float t = clamp((val-u_min)/max(u_max-u_min,1e-6),0.,1.);
    if (u_invert > 0.5) t = 1.0 - t;
    frag_color = vec4(texture(u_colormap, t).rgb, u_opacity);
}
)glsl";

}  // namespace tileshaders
