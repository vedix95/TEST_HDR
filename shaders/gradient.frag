#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_overlay;

layout(std140, set = 3, binding = 0) uniform Params {
    float width;
    float height;
    float min_nits;
    float max_nits;
    int scale_mode;          // 0 = PQ, 1 = Log, 2 = Linear
    int view_mode;           // 0 = Split, 1 = Smooth, 2 = Bands, 3 = RGBW, 4 = TestPattern, 5 = CieTriangle
    int channel_mode;        // 0 = White, 1 = Red, 2 = Green, 3 = Blue
    int hdr_output_mode;     // 0 = HDR10_ST2084, 1 = HDR_EXTENDED_LINEAR, 2 = SDR10, 3 = SDR8
    int band_count;
    float hud_nits;
    float sdr_preview_max_nits;
    int test_pattern;        // 0..5
    float cie_x0;
    float cie_y0;
    float cie_w;
    float cie_h;
    int active_gamut_id;     // 0 = sRGB, 1 = DCI-P3, 2 = EDID, 3 = BT.2020
    float peak_nits;
    float edid_rx;
    float edid_ry;
    float edid_gx;
    float edid_gy;
    float edid_bx;
    float edid_by;
} u;

const float PQ_M1 = 2610.0 / 16384.0;
const float PQ_M2 = (2523.0 / 4096.0) * 128.0;
const float PQ_C1 = 3424.0 / 4096.0;
const float PQ_C2 = (2413.0 / 4096.0) * 32.0;
const float PQ_C3 = (2392.0 / 4096.0) * 32.0;
const float PQ_MAX_NITS = 10000.0;
const float LOG_OFFSET = 0.01;

// Column-major exact CIE 1931 D65 color conversion matrices
const mat3 XYZ_TO_REC709 = mat3(
    3.2409699, -0.9692436, 0.0556301,
    -1.5373832, 1.8759675, -0.2039770,
    -0.4986108, 0.0415551, 1.0569715
);

const mat3 XYZ_TO_BT2020 = mat3(
    1.7166512, -0.6666844, 0.0176399,
    -0.3556708, 1.6164812, -0.0427706,
    -0.2533663, 0.0157685, 0.9421031
);

const mat3 DCIP3_TO_REC709 = mat3(
    1.2249402, -0.0420570, -0.0196376,
    -0.2249402, 1.0420570, -0.0786360,
    -0.0000000, 0.0000000, 1.0982736
);

const mat3 BT2020_TO_REC709 = mat3(
    1.6604910, -0.1245505, -0.0181508,
    -0.5876411, 1.1328999, -0.1005789,
    -0.0728499, -0.0083494, 1.1187297
);

const mat3 REC709_TO_BT2020 = mat3(
    0.6274039, 0.0690973, 0.0163914,
    0.3292830, 0.9195404, 0.0880133,
    0.0433131, 0.0113623, 0.8955953
);

// CIE 1931 2-degree spectral locus (380 nm to 700 nm, 5 nm steps)
const vec2 CIE_LOCUS[65] = vec2[65](
    vec2(0.1741, 0.0050), vec2(0.1740, 0.0050), vec2(0.1738, 0.0049), vec2(0.1736, 0.0049),
    vec2(0.1733, 0.0048), vec2(0.1730, 0.0048), vec2(0.1726, 0.0048), vec2(0.1721, 0.0048),
    vec2(0.1714, 0.0051), vec2(0.1703, 0.0058), vec2(0.1689, 0.0069), vec2(0.1669, 0.0086),
    vec2(0.1644, 0.0109), vec2(0.1611, 0.0138), vec2(0.1566, 0.0177), vec2(0.1510, 0.0227),
    vec2(0.1440, 0.0297), vec2(0.1355, 0.0399), vec2(0.1241, 0.0578), vec2(0.1096, 0.0868),
    vec2(0.0913, 0.1327), vec2(0.0687, 0.2007), vec2(0.0454, 0.2950), vec2(0.0235, 0.4127),
    vec2(0.0082, 0.5384), vec2(0.0039, 0.6548), vec2(0.0139, 0.7502), vec2(0.0389, 0.8120),
    vec2(0.0743, 0.8338), vec2(0.1142, 0.8262), vec2(0.1547, 0.8059), vec2(0.1929, 0.7816),
    vec2(0.2296, 0.7543), vec2(0.2658, 0.7243), vec2(0.3016, 0.6923), vec2(0.3373, 0.6589),
    vec2(0.3731, 0.6245), vec2(0.4087, 0.5896), vec2(0.4441, 0.5547), vec2(0.4788, 0.5202),
    vec2(0.5125, 0.4866), vec2(0.5448, 0.4544), vec2(0.5752, 0.4242), vec2(0.6029, 0.3965),
    vec2(0.6270, 0.3725), vec2(0.6482, 0.3514), vec2(0.6658, 0.3340), vec2(0.6801, 0.3197),
    vec2(0.6915, 0.3083), vec2(0.7006, 0.2993), vec2(0.7079, 0.2920), vec2(0.7140, 0.2859),
    vec2(0.7190, 0.2809), vec2(0.7230, 0.2770), vec2(0.7260, 0.2740), vec2(0.7283, 0.2717),
    vec2(0.7300, 0.2700), vec2(0.7311, 0.2689), vec2(0.7320, 0.2680), vec2(0.7327, 0.2673),
    vec2(0.7334, 0.2666), vec2(0.7340, 0.2660), vec2(0.7344, 0.2656), vec2(0.7346, 0.2654),
    vec2(0.7347, 0.2653)
);

float nits_to_pq(float nits) {
    float clamped = clamp(nits, 0.0, PQ_MAX_NITS);
    if (clamped <= 0.0) return 0.0;
    float y = clamped / PQ_MAX_NITS;
    float ym1 = pow(y, PQ_M1);
    return pow((PQ_C1 + PQ_C2 * ym1) / (1.0 + PQ_C3 * ym1), PQ_M2);
}

float pq_to_nits(float pq) {
    float clamped = clamp(pq, 0.0, 1.0);
    if (clamped <= 0.0) return 0.0;
    float p = pow(clamped, 1.0 / PQ_M2);
    float num = max(p - PQ_C1, 0.0);
    float den = max(PQ_C2 - PQ_C3 * p, 1e-6);
    return PQ_MAX_NITS * pow(num / den, 1.0 / PQ_M1);
}

float t_to_nits(float t) {
    float u_t = clamp(t, 0.0, 1.0);
    float lo = max(0.0, u.min_nits);
    float hi = max(lo + 1e-5, u.max_nits);

    if (u.scale_mode == 0) {
        float p_lo = nits_to_pq(lo);
        float p_hi = nits_to_pq(hi);
        return pq_to_nits(mix(p_lo, p_hi, u_t));
    } else if (u.scale_mode == 1) {
        float l_lo = log(lo + LOG_OFFSET) / log(10.0);
        float l_hi = log(hi + LOG_OFFSET) / log(10.0);
        float exp_val = mix(l_lo, l_hi, u_t);
        return max(0.0, pow(10.0, exp_val) - LOG_OFFSET);
    } else {
        return mix(lo, hi, u_t);
    }
}

vec3 srgb_to_linear(vec3 c) {
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(lo, hi, step(vec3(0.04045), c));
}

vec3 linear_to_srgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

vec3 apply_channel_mask(int ch, float nits) {
    if (ch == 1) return vec3(nits, 0.0, 0.0);
    if (ch == 2) return vec3(0.0, nits, 0.0);
    if (ch == 3) return vec3(0.0, 0.0, nits);
    return vec3(nits);
}

vec3 hsv_to_rgb(float h, float s, float v) {
    vec3 k = vec3(1.0, 2.0 / 3.0, 1.0 / 3.0);
    vec3 p = abs(fract(vec3(h) + k) * 6.0 - 3.0);
    return v * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), s);
}

float tri_edge(vec2 p, vec2 a, vec2 b) {
    return (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y);
}

bool point_in_tri(vec2 p, vec2 a, vec2 b, vec2 c) {
    float d1 = tri_edge(p, a, b);
    float d2 = tri_edge(p, b, c);
    float d3 = tri_edge(p, c, a);
    bool has_neg = (d1 < 0.0) || (d2 < 0.0) || (d3 < 0.0);
    bool has_pos = (d1 > 0.0) || (d2 > 0.0) || (d3 > 0.0);
    return !(has_neg && has_pos);
}

bool point_in_cie_locus(vec2 p) {
    if (p.x < 0.0 || p.y <= 0.0 || (p.x + p.y) > 1.0) return false;
    bool inside = false;
    int j = 64;
    for (int i = 0; i < 65; i++) {
        vec2 a = CIE_LOCUS[i];
        vec2 b = CIE_LOCUS[j];
        if (((a.y > p.y) != (b.y > p.y)) &&
            (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) {
            inside = !inside;
        }
        j = i;
    }
    return inside;
}

vec3 smpte_bar_rgb(int col) {
    if (col == 0) return vec3(1.0, 1.0, 1.0);  // White
    if (col == 1) return vec3(1.0, 1.0, 0.0);  // Yellow
    if (col == 2) return vec3(0.0, 1.0, 1.0);  // Cyan
    if (col == 3) return vec3(0.0, 1.0, 0.0);  // Green
    if (col == 4) return vec3(1.0, 0.0, 1.0);  // Magenta
    if (col == 5) return vec3(1.0, 0.0, 0.0);  // Red
    return vec3(0.0, 0.0, 1.0);                // Blue
}

// 24 classic ColorChecker sRGB patch values
vec3 colorchecker_srgb(int idx) {
    if (idx == 0)  return vec3(0.451, 0.322, 0.267); // Dark skin
    if (idx == 1)  return vec3(0.761, 0.588, 0.510); // Light skin
    if (idx == 2)  return vec3(0.384, 0.478, 0.616); // Blue sky
    if (idx == 3)  return vec3(0.341, 0.424, 0.263); // Foliage
    if (idx == 4)  return vec3(0.522, 0.502, 0.694); // Blue flower
    if (idx == 5)  return vec3(0.404, 0.741, 0.667); // Bluish green
    if (idx == 6)  return vec3(0.839, 0.494, 0.173); // Orange
    if (idx == 7)  return vec3(0.314, 0.357, 0.651); // Purplish blue
    if (idx == 8)  return vec3(0.757, 0.353, 0.388); // Moderate red
    if (idx == 9)  return vec3(0.369, 0.235, 0.424); // Purple
    if (idx == 10) return vec3(0.616, 0.737, 0.251); // Yellow green
    if (idx == 11) return vec3(0.878, 0.639, 0.180); // Orange yellow
    if (idx == 12) return vec3(0.220, 0.239, 0.588); // Blue
    if (idx == 13) return vec3(0.275, 0.580, 0.286); // Green
    if (idx == 14) return vec3(0.686, 0.212, 0.235); // Red
    if (idx == 15) return vec3(0.906, 0.780, 0.122); // Yellow
    if (idx == 16) return vec3(0.733, 0.337, 0.584); // Magenta
    if (idx == 17) return vec3(0.031, 0.522, 0.631); // Cyan
    if (idx == 18) return vec3(0.953, 0.953, 0.949); // White
    if (idx == 19) return vec3(0.784, 0.784, 0.784); // Neutral 8
    if (idx == 20) return vec3(0.627, 0.627, 0.627); // Neutral 6.5
    if (idx == 21) return vec3(0.478, 0.478, 0.475); // Neutral 5
    if (idx == 22) return vec3(0.333, 0.333, 0.333); // Neutral 3.5
    return vec3(0.204, 0.204, 0.204);                // Black
}

vec3 render_test_pattern(vec2 uv, vec2 px) {
    float peak = max(u.peak_nits, 80.0);
    float sdr_w = 80.0;
    float x = uv.x;
    float y = uv.y;
    vec3 bg = vec3(1.5);

    if (u.test_pattern == 0) {
        // 0: Gamut comparison (Rec.709 vs DCI-P3 vs BT.2020) + ColorChecker + Grayscale
        if (y >= 0.06 && y < 0.36 && x >= 0.03 && x < 0.97) {
            float xt = (x - 0.03) / 0.94;
            float yt = (y - 0.06) / 0.30;
            int row = clamp(int(floor(yt * 3.0)), 0, 2);
            int col = clamp(int(floor(xt * 7.0)), 0, 6);
            float fy = fract(yt * 3.0);
            float fx = fract(xt * 7.0);
            if (fy > 0.05 && fy < 0.95 && fx > 0.02 && fx < 0.98) {
                vec3 p = smpte_bar_rgb(col);
                vec3 lin709 = p;
                if (row == 1) lin709 = DCIP3_TO_REC709 * p;
                else if (row == 2) lin709 = BT2020_TO_REC709 * p;
                return lin709 * (sdr_w * 0.85);
            }
            return bg;
        }
        if (y >= 0.39 && y < 0.71 && x >= 0.03 && x < 0.97) {
            float xt = (x - 0.03) / 0.94;
            float yt = (y - 0.39) / 0.32;
            int col = clamp(int(floor(xt * 6.0)), 0, 5);
            int row = clamp(int(floor(yt * 4.0)), 0, 3);
            float fx = fract(xt * 6.0);
            float fy = fract(yt * 4.0);
            if (fx > 0.06 && fx < 0.94 && fy > 0.08 && fy < 0.92) {
                vec3 srgb = colorchecker_srgb(row * 6 + col);
                return srgb_to_linear(srgb) * sdr_w;
            }
            return vec3(3.0);
        }
        if (y >= 0.74 && y < 0.89 && x >= 0.03 && x < 0.97) {
            float xt = (x - 0.03) / 0.94;
            int step_i = clamp(int(floor(xt * 12.0)), 0, 11);
            float fx = fract(xt * 12.0);
            if (fx > 0.03 && fx < 0.97) {
                float frac = float(step_i) / 11.0;
                float lum = (step_i == 0) ? 0.0 : pow(frac, 2.2) * peak;
                return vec3(lum);
            }
            return bg;
        }
        return bg;
    }

    if (u.test_pattern == 1) {
        // 1: 10-bit/16-bit smooth vs 8-bit quantized ramp (3 bands)
        if (x >= 0.03 && x < 0.97) {
            float xt = clamp((x - 0.03) / 0.94, 0.0, 1.0);
            int band = -1;
            float local_y = 0.0;
            if (y >= 0.08 && y < 0.33) { band = 0; local_y = (y - 0.08) / 0.25; }
            else if (y >= 0.36 && y < 0.61) { band = 1; local_y = (y - 0.36) / 0.25; }
            else if (y >= 0.64 && y < 0.89) { band = 2; local_y = (y - 0.64) / 0.25; }

            if (band >= 0) {
                if (abs(local_y - 0.50) < 0.015) return vec3(0.0);
                bool quant8 = (local_y > 0.50);
                float v = xt;
                if (quant8) {
                    v = floor(v * 255.0 + 0.5) / 255.0;
                }
                if (band == 0) {
                    // Subtle near-dark to mid neutral ramp (0 -> 0.35 * peak in perceptual sRGB/PQ)
                    float lum = srgb_to_linear(vec3(v * 0.45)).r * peak;
                    return vec3(lum);
                } else if (band == 1) {
                    // Shallow sky blue gradient (classic 8-bit sky banding test)
                    vec3 c0 = vec3(0.06, 0.14, 0.30);
                    vec3 c1 = vec3(0.26, 0.42, 0.62);
                    vec3 s = mix(c0, c1, v);
                    if (quant8) s = floor(s * 255.0 + 0.5) / 255.0;
                    return srgb_to_linear(s) * sdr_w;
                } else {
                    // Warm sunset / skin gradient
                    vec3 c0 = vec3(0.18, 0.07, 0.04);
                    vec3 c1 = vec3(0.88, 0.56, 0.36);
                    vec3 s = mix(c0, c1, v);
                    if (quant8) s = floor(s * 255.0 + 0.5) / 255.0;
                    return srgb_to_linear(s) * sdr_w;
                }
            }
        }
        return vec3(1.0);
    }

    if (u.test_pattern == 2) {
        // 2: Black Crush (left) & White Clipping (right)
        if (y >= 0.10 && y < 0.88) {
            if (x >= 0.03 && x < 0.48) {
                // Pure black background
                float xt = (x - 0.03) / 0.45;
                float yt = (y - 0.10) / 0.78;
                int col = clamp(int(floor(xt * 4.0)), 0, 3);
                int row = clamp(int(floor(yt * 3.0)), 0, 2);
                float fx = fract(xt * 4.0);
                float fy = fract(yt * 3.0);
                if (fx > 0.16 && fx < 0.84 && fy > 0.16 && fy < 0.84) {
                    int idx = row * 4 + col;
                    // 12 near-black patches from very dim to clearly visible
                    float vals[12] = float[12](
                        0.005, 0.01, 0.02, 0.05,
                        0.10,  0.20, 0.40, 0.80,
                        1.50,  3.00, 5.00, 8.00
                    );
                    float nits = vals[idx];
                    if (peak <= 90.0) {
                        // In SDR (0..80 nits), spread cleanly across 0.2%..10% of 80 nits
                        float sdr_frac[12] = float[12](
                            0.002, 0.004, 0.007, 0.010,
                            0.015, 0.020, 0.030, 0.040,
                            0.055, 0.070, 0.085, 0.100
                        );
                        return vec3(srgb_to_linear(vec3(sdr_frac[idx])).r * sdr_w);
                    }
                    return vec3(nits);
                }
                return vec3(0.0);
            }
            if (x >= 0.52 && x < 0.97) {
                float xt = (x - 0.52) / 0.45;
                float yt = (y - 0.10) / 0.78;
                int col = clamp(int(floor(xt * 4.0)), 0, 3);
                int row = clamp(int(floor(yt * 3.0)), 0, 2);
                float fx = fract(xt * 4.0);
                float fy = fract(yt * 3.0);
                float bg_white = (peak <= 90.0) ? sdr_w : peak;
                if (fx > 0.16 && fx < 0.84 && fy > 0.16 && fy < 0.84) {
                    int idx = row * 4 + col;
                    if (peak <= 90.0) {
                        // SDR near-white 88%..100% against 100% white background
                        float sdr_hi[12] = float[12](
                            0.88, 0.90, 0.92, 0.94,
                            0.95, 0.96, 0.97, 0.98,
                            0.988, 0.992, 0.996, 1.00
                        );
                        return vec3(srgb_to_linear(vec3(sdr_hi[idx])).r * sdr_w);
                    } else {
                        // HDR patches from 0.45*peak up to 1.60*peak against peak background
                        float mult[12] = float[12](
                            0.50, 0.65, 0.75, 0.85,
                            0.92, 0.96, 1.00, 1.05,
                            1.12, 1.25, 1.40, 1.65
                        );
                        return vec3(peak * mult[idx]);
                    }
                }
                return vec3(bg_white);
            }
        }
        return bg;
    }

    if (u.test_pattern == 3) {
        // 3: Procedural HDR / WCG scene (twilight sky, stars, solar peak, dark ridges, wide-gamut reflections)
        float horizon = 0.56;
        vec2 sun_pos = vec2(0.72, 0.36);
        float aspect = max(u.width, 1.0) / max(u.height, 1.0);
        vec2 d_sun = vec2((x - sun_pos.x) * aspect, y - sun_pos.y);
        float r_sun = length(d_sun);

        // Sky
        float sky_t = clamp(y / horizon, 0.0, 1.0);
        vec3 sky_top = vec3(0.008, 0.015, 0.055) * sdr_w;
        vec3 sky_mid = vec3(0.05, 0.11, 0.28) * sdr_w;
        vec3 sky_hor = vec3(0.45, 0.16, 0.08) * sdr_w;
        vec3 col = mix(sky_top, sky_mid, smoothstep(0.0, 0.65, sky_t));
        col = mix(col, sky_hor, pow(sky_t, 3.0));

        // Specular sun & corona (tests highlight retention > SDR white)
        float corona = exp(-r_sun * 7.5);
        float core = smoothstep(0.065, 0.012, r_sun);
        float ring = 0.15 * sin(r_sun * 180.0) * smoothstep(0.09, 0.02, r_sun);
        col += vec3(1.0, 0.62, 0.25) * corona * (peak * 0.45);
        col += vec3(1.0, 0.94, 0.82) * max(core + ring * core, 0.0) * peak;

        // Crisp procedural stars in upper sky
        vec2 star_cell = floor(px / 48.0);
        vec2 star_f = fract(px / 48.0) - 0.5;
        float h = fract(sin(dot(star_cell, vec2(127.1, 311.7))) * 43758.5453);
        if (h > 0.86 && y < horizon * 0.72) {
            float star_d = length(star_f);
            float star_b = smoothstep(0.10, 0.0, star_d) * (0.4 + 0.6 * fract(h * 31.7));
            col += vec3(0.85, 0.92, 1.0) * star_b * (peak * 0.55);
        }

        // Far mountain silhouette (near-black detail)
        float m1 = horizon - 0.09 * exp(-pow((x - 0.30) * 4.5, 2.0)) - 0.06 * exp(-pow((x - 0.75) * 3.8, 2.0)) - 0.015 * sin(x * 38.0);
        if (y > m1 && y < horizon) {
            float depth = clamp((y - m1) / 0.10, 0.0, 1.0);
            col = mix(vec3(0.015, 0.020, 0.035), vec3(0.004, 0.005, 0.009), depth) * sdr_w;
        }

        // Water reflection + wide-gamut neon aurora strips
        if (y >= horizon) {
            float wt = (y - horizon) / (1.0 - horizon);
            float ripple = sin(y * 140.0 + x * 18.0) * 0.015 * (0.3 + wt);
            float rx = x + ripple;
            vec3 refl = mix(sky_hor * 0.6, sky_top * 0.4, pow(wt, 0.6));
            float sun_col = exp(-abs(rx - sun_pos.x) * (24.0 - 16.0 * wt)) * (1.0 - wt * 0.5);
            refl += vec3(1.0, 0.72, 0.32) * sun_col * (peak * 0.65);

            // Wide-gamut neon beacons reflected on left (Rec.709 vs DCI-P3 vs BT.2020)
            float neon_g = exp(-pow((rx - 0.18) * 28.0, 2.0)) * (0.5 + 0.5 * sin(y * 110.0));
            float neon_r = exp(-pow((rx - 0.30) * 28.0, 2.0)) * (0.5 + 0.5 * sin(y * 95.0));
            float neon_b = exp(-pow((rx - 0.42) * 28.0, 2.0)) * (0.5 + 0.5 * sin(y * 125.0));
            refl += (BT2020_TO_REC709 * vec3(0.0, 1.0, 0.0)) * neon_g * (sdr_w * 1.1);
            refl += (BT2020_TO_REC709 * vec3(1.0, 0.0, 0.0)) * neon_r * (sdr_w * 1.1);
            refl += (BT2020_TO_REC709 * vec3(0.0, 0.0, 1.0)) * neon_b * (sdr_w * 1.1);
            col = refl;
        }
        return col;
    }

    if (u.test_pattern == 4) {
        // 4: Sharpness (1px/2px grid, 4:4:4 chroma stripes) & ANSI 4x4 contrast
        if (y >= 0.07 && y < 0.60) {
            if (x >= 0.03 && x < 0.48) {
                // 4x4 ANSI contrast checkerboard
                int cx = int(floor(((x - 0.03) / 0.45) * 4.0));
                int cy = int(floor(((y - 0.07) / 0.53) * 4.0));
                bool white_sq = ((cx + cy) % 2) == 0;
                return white_sq ? vec3(peak) : vec3(0.0);
            }
            if (x >= 0.52 && x < 0.97) {
                float xt = (x - 0.52) / 0.45;
                float yt = (y - 0.07) / 0.53;
                int qx = int(floor(xt * 2.0));
                int qy = int(floor(yt * 2.0));
                int ix = int(floor(px.x));
                int iy = int(floor(px.y));
                if (qx == 0 && qy == 0) {
                    // 1px monochrome checkerboard
                    return (((ix + iy) & 1) == 0) ? vec3(sdr_w) : vec3(0.0);
                } else if (qx == 1 && qy == 0) {
                    // 1px Red/Cyan vertical lines (chroma 4:4:4 test)
                    return ((ix & 1) == 0) ? vec3(sdr_w, 0.0, 0.0) : vec3(0.0, sdr_w, sdr_w);
                } else if (qx == 0 && qy == 1) {
                    // 1px Green/Magenta horizontal lines
                    return ((iy & 1) == 0) ? vec3(0.0, sdr_w, 0.0) : vec3(sdr_w, 0.0, sdr_w);
                } else {
                    // 2px Blue/Yellow fine grid
                    return ((((ix >> 1) + (iy >> 1)) & 1) == 0) ? vec3(0.0, 0.0, sdr_w) : vec3(sdr_w, sdr_w, 0.0);
                }
            }
        }
        if (y >= 0.64 && y < 0.89 && x >= 0.03 && x < 0.97) {
            float xt = (x - 0.03) / 0.94;
            int idx = clamp(int(floor(xt * 6.0)), 0, 5);
            float fx = fract(xt * 6.0);
            if (fx > 0.03 && fx < 0.97) {
                if (idx == 0) return vec3(peak * 0.05);
                if (idx == 1) return vec3(peak * 0.25);
                if (idx == 2) return vec3(peak * 0.50);
                if (idx == 3) return vec3(sdr_w);
                if (idx == 4) return vec3(peak);
                return vec3(sdr_w * 0.5, sdr_w * 0.5, sdr_w * 0.5);
            }
        }
        return bg;
    }

    // 5: Hue x Saturation/Luminance & Gamut Sweep (Rec.709 -> DCI-P3 -> BT.2020)
    if (y >= 0.06 && y < 0.46 && x >= 0.03 && x < 0.97) {
        float xt = clamp((x - 0.03) / 0.94, 0.0, 1.0);
        float yt = clamp((y - 0.06) / 0.40, 0.0, 1.0);
        // Top = white/bright, middle = pure saturated Rec.709, bottom = dark saturated
        float sat = smoothstep(0.0, 0.5, yt);
        float val = 1.0 - smoothstep(0.5, 1.0, yt) * 0.95;
        vec3 srgb = hsv_to_rgb(xt, sat, val);
        return srgb_to_linear(srgb) * sdr_w;
    }
    if (y >= 0.50 && y < 0.89 && x >= 0.03 && x < 0.97) {
        float xt = clamp((x - 0.03) / 0.94, 0.0, 1.0);
        float yt = clamp((y - 0.50) / 0.39, 0.0, 1.0);
        vec3 base_rgb = hsv_to_rgb(xt, 1.0, 1.0);
        vec3 c709 = base_rgb;
        vec3 cp3 = DCIP3_TO_REC709 * base_rgb;
        vec3 c2020 = BT2020_TO_REC709 * base_rgb;
        vec3 mixed = (yt < 0.5) ? mix(c709, cp3, yt * 2.0) : mix(cp3, c2020, (yt - 0.5) * 2.0);
        return mixed * (sdr_w * 0.85);
    }
    return bg;
}

vec3 render_cie_diagram(vec2 uv) {
    float sdr_w = 80.0;
    vec3 bg = vec3(0.015, 0.018, 0.025) * sdr_w;
    if (u.cie_w <= 1e-4 || u.cie_h <= 1e-4) return bg;

    float rx = (uv.x - u.cie_x0) / u.cie_w;
    float ry = (uv.y - u.cie_y0) / u.cie_h;
    if (rx < 0.0 || rx > 1.0 || ry < 0.0 || ry > 1.0) return bg;

    float x_cie = mix(-0.04, 0.88, rx);
    float y_cie = mix(0.88, -0.04, ry);
    vec2 p = vec2(x_cie, y_cie);

    if (!point_in_cie_locus(p)) {
        // Subtle 0.1 CIE grid inside plot box
        float gx = abs(fract(x_cie * 10.0 + 0.5) - 0.5) / 10.0;
        float gy = abs(fract(y_cie * 10.0 + 0.5) - 0.5) / 10.0;
        float grid = (gx < 0.002 || gy < 0.002) && x_cie >= 0.0 && x_cie <= 0.82 && y_cie >= 0.0 && y_cie <= 0.86 ? 1.0 : 0.0;
        return mix(vec3(0.024, 0.028, 0.038), vec3(0.06, 0.07, 0.09), grid) * sdr_w;
    }

    // CIE xy -> XYZ (Y = 1.0)
    float inv_y = 1.0 / max(p.y, 1e-4);
    vec3 xyz = vec3(p.x * inv_y, 1.0, max(1.0 - p.x - p.y, 0.0) * inv_y);

    // D65 white XYZ (Y = 1.0)
    vec3 d65_xyz = vec3(0.95047, 1.0, 1.08883);

    // Convert to target rendering RGB, smoothly pulling out-of-gamut locus points toward D65
    // so the horseshoe has smooth hue transitions without harsh clipping kinks
    bool use_bt2020_native = (u.hdr_output_mode == 0);
    vec3 rgb_raw = use_bt2020_native ? (XYZ_TO_BT2020 * xyz) : (XYZ_TO_REC709 * xyz);

    float min_c = min(rgb_raw.r, min(rgb_raw.g, rgb_raw.b));
    vec3 rgb_vis = rgb_raw;
    if (min_c < 0.0) {
        // Blend toward D65 white in XYZ until non-negative in rendering primaries
        vec3 w_rgb = vec3(1.0);
        float alpha = -min_c / max(1.0 - min_c, 1e-5);
        // Slightly preserve wide-gamut saturation in scRGB/Sdr10 while keeping displayable
        if ((u.hdr_output_mode == 1 || u.hdr_output_mode == 2) && u.active_gamut_id > 0) {
            alpha *= 0.65;
        }
        rgb_vis = mix(rgb_raw, w_rgb, clamp(alpha, 0.0, 1.0));
    }
    float max_c = max(rgb_vis.r, max(rgb_vis.g, rgb_vis.b));
    if (max_c > 1e-5) {
        rgb_vis /= max_c;
    }

    // Convert back to Rec.709 linear nits if we used BT.2020 matrix for HDR10
    vec3 out_rec709 = use_bt2020_native ? (BT2020_TO_REC709 * rgb_vis) : rgb_vis;

    // Spectrum limits visual mask:
    // Show active gamut at full brightness, EDID panel capability slightly dimmer if outside active mode,
    // and unreachable visible human spectrum clearly dimmer so limits are unmistakable.
    vec2 r_709 = vec2(0.6400, 0.3300), g_709 = vec2(0.3000, 0.6000), b_709 = vec2(0.1500, 0.0600);
    vec2 r_p3  = vec2(0.6800, 0.3200), g_p3  = vec2(0.2650, 0.6900), b_p3  = vec2(0.1500, 0.0600);
    vec2 r_20  = vec2(0.7080, 0.2920), g_20  = vec2(0.1700, 0.7970), b_20  = vec2(0.1310, 0.0460);
    vec2 r_ed  = vec2(u.edid_rx, u.edid_ry), g_ed = vec2(u.edid_gx, u.edid_gy), b_ed = vec2(u.edid_bx, u.edid_by);

    bool in_709  = point_in_tri(p, r_709, g_709, b_709);
    bool in_p3   = point_in_tri(p, r_p3, g_p3, b_p3);
    bool in_2020 = point_in_tri(p, r_20, g_20, b_20);
    bool in_edid = point_in_tri(p, r_ed, g_ed, b_ed);

    bool in_active = in_709;
    if (u.active_gamut_id == 1) in_active = in_p3;
    else if (u.active_gamut_id == 2) in_active = in_edid;
    else if (u.active_gamut_id == 3) in_active = in_2020;

    float lum_scale = 0.24;
    if (in_active && in_edid) {
        lum_scale = 0.92;
    } else if (in_active) {
        // e.g. BT.2020 container beyond physical EDID panel limit
        lum_scale = 0.56;
    } else if (in_edid) {
        // Panel can display it, but current mode (e.g. sRGB) restricts it
        lum_scale = 0.40;
    }

    return out_rec709 * (sdr_w * lum_scale);
}

void main() {
    float w = max(u.width, 1.0);
    float x = v_uv.x * w - 0.5;
    float t = clamp(v_uv.x, 0.0, 1.0);
    float yn = v_uv.y;

    vec3 grad_nits = vec3(0.0);

    if (u.view_mode == 0) {
        if (yn < 0.62) {
            float nits = t_to_nits(t);
            grad_nits = apply_channel_mask(u.channel_mode, nits);
        } else if (yn >= 0.625 && yn < 0.80) {
            int n = max(u.band_count, 2);
            float f_idx = floor(t * float(n));
            int idx = clamp(int(f_idx), 0, n - 1);
            float band_start = float(idx) * (w / float(n));
            float band_end = float(idx + 1) * (w / float(n));
            if ((x - band_start) >= 1.0 && (band_end - x) >= 1.0) {
                float bt = float(idx) / float(n - 1);
                float nits = t_to_nits(bt);
                grad_nits = apply_channel_mask(u.channel_mode, nits);
            }
        }
    } else if (u.view_mode == 1) {
        if (yn < 0.84) {
            float nits = t_to_nits(t);
            grad_nits = apply_channel_mask(u.channel_mode, nits);
        }
    } else if (u.view_mode == 2) {
        if (yn < 0.84) {
            int n = max(u.band_count, 2);
            int idx = clamp(int(floor(t * float(n))), 0, n - 1);
            float band_start = float(idx) * (w / float(n));
            float band_end = float(idx + 1) * (w / float(n));
            if ((x - band_start) >= 1.0 && (band_end - x) >= 1.0) {
                float bt = float(idx) / float(n - 1);
                float nits = t_to_nits(bt);
                grad_nits = apply_channel_mask(u.channel_mode, nits);
            }
        }
    } else if (u.view_mode == 3) {
        if (yn < 0.84) {
            float ry = yn / 0.84;
            int row = clamp(int(floor(ry * 4.0)), 0, 3);
            float row_local = fract(ry * 4.0);
            if (row_local > 0.015 && row_local < 0.985) {
                float nits = t_to_nits(t);
                grad_nits = apply_channel_mask(row, nits);
            }
        }
    } else if (u.view_mode == 4) {
        grad_nits = render_test_pattern(v_uv, vec2(v_uv.x * w, v_uv.y * max(u.height, 1.0)));
    } else {
        grad_nits = render_cie_diagram(v_uv);
    }

    // Composite Cairo/Pango HUD & overlay in linear nits
    vec4 ov = texture(u_overlay, v_uv);
    vec3 final_nits = grad_nits;
    if (ov.a > 0.001) {
        vec3 ov_srgb = clamp(ov.rgb / ov.a, 0.0, 1.0);
        vec3 ov_lin = srgb_to_linear(ov_srgb);
        final_nits = final_nits * (1.0 - ov.a) + ov_lin * u.hud_nits * ov.a;
    }

    // Convert linear Rec.709 nits to swapchain encoding
    if (u.hdr_output_mode == 0) {
        // HDR10 ST.2084 (BT.2020 + PQ)
        vec3 bt2020_nits = max(REC709_TO_BT2020 * final_nits, vec3(0.0));
        vec3 pq = vec3(
            nits_to_pq(bt2020_nits.r),
            nits_to_pq(bt2020_nits.g),
            nits_to_pq(bt2020_nits.b)
        );
        out_color = vec4(pq, 1.0);
    } else if (u.hdr_output_mode == 1) {
        // HDR Extended Linear (16-bit scRGB: 1.0 = 80 nits, allows <0 wide & >1 HDR)
        out_color = vec4(final_nits / 80.0, 1.0);
    } else if (u.hdr_output_mode == 2) {
        // SDR 10-bit via 16-bit linear scRGB swapchain -> 10-bit XRGB2101010 scanout
        float denom = (u.view_mode >= 4) ? 80.0 : max(u.sdr_preview_max_nits, 80.0);
        vec3 sdr_lin = final_nits / denom;
        if (u.active_gamut_id == 0) {
            sdr_lin = max(sdr_lin, vec3(0.0));
        }
        sdr_lin = min(sdr_lin, vec3(1.0));
        out_color = vec4(sdr_lin, 1.0);
    } else {
        // SDR 8-bit sRGB
        float denom = (u.view_mode >= 4) ? 80.0 : max(u.sdr_preview_max_nits, 80.0);
        vec3 sdr_lin = clamp(final_nits / denom, 0.0, 1.0);
        out_color = vec4(linear_to_srgb(sdr_lin), 1.0);
    }
}
