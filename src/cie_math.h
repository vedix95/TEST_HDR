#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace testhdr {

struct CiePoint {
    double x = 0.0;
    double y = 0.0;
};

struct GamutTriangle {
    CiePoint r;
    CiePoint g;
    CiePoint b;
    CiePoint w;
};

struct CieLocusSample {
    int wavelength_nm;
    double x;
    double y;
};

// CIE 1931 2-degree Standard Observer spectral locus (380 nm to 700 nm, 5 nm steps).
inline constexpr std::array<CieLocusSample, 65> kCie1931Locus = {{
    {380, 0.1741, 0.0050}, {385, 0.1740, 0.0050}, {390, 0.1738, 0.0049}, {395, 0.1736, 0.0049},
    {400, 0.1733, 0.0048}, {405, 0.1730, 0.0048}, {410, 0.1726, 0.0048}, {415, 0.1721, 0.0048},
    {420, 0.1714, 0.0051}, {425, 0.1703, 0.0058}, {430, 0.1689, 0.0069}, {435, 0.1669, 0.0086},
    {440, 0.1644, 0.0109}, {445, 0.1611, 0.0138}, {450, 0.1566, 0.0177}, {455, 0.1510, 0.0227},
    {460, 0.1440, 0.0297}, {465, 0.1355, 0.0399}, {470, 0.1241, 0.0578}, {475, 0.1096, 0.0868},
    {480, 0.0913, 0.1327}, {485, 0.0687, 0.2007}, {490, 0.0454, 0.2950}, {495, 0.0235, 0.4127},
    {500, 0.0082, 0.5384}, {505, 0.0039, 0.6548}, {510, 0.0139, 0.7502}, {515, 0.0389, 0.8120},
    {520, 0.0743, 0.8338}, {525, 0.1142, 0.8262}, {530, 0.1547, 0.8059}, {535, 0.1929, 0.7816},
    {540, 0.2296, 0.7543}, {545, 0.2658, 0.7243}, {550, 0.3016, 0.6923}, {555, 0.3373, 0.6589},
    {560, 0.3731, 0.6245}, {565, 0.4087, 0.5896}, {570, 0.4441, 0.5547}, {575, 0.4788, 0.5202},
    {580, 0.5125, 0.4866}, {585, 0.5448, 0.4544}, {590, 0.5752, 0.4242}, {595, 0.6029, 0.3965},
    {600, 0.6270, 0.3725}, {605, 0.6482, 0.3514}, {610, 0.6658, 0.3340}, {615, 0.6801, 0.3197},
    {620, 0.6915, 0.3083}, {625, 0.7006, 0.2993}, {630, 0.7079, 0.2920}, {635, 0.7140, 0.2859},
    {640, 0.7190, 0.2809}, {645, 0.7230, 0.2770}, {650, 0.7260, 0.2740}, {655, 0.7283, 0.2717},
    {660, 0.7300, 0.2700}, {665, 0.7311, 0.2689}, {670, 0.7320, 0.2680}, {675, 0.7327, 0.2673},
    {680, 0.7334, 0.2666}, {685, 0.7340, 0.2660}, {690, 0.7344, 0.2656}, {695, 0.7346, 0.2654},
    {700, 0.7347, 0.2653},
}};

inline constexpr GamutTriangle kGamutSrgb = {
    {0.6400, 0.3300}, {0.3000, 0.6000}, {0.1500, 0.0600}, {0.3127, 0.3290},
};

inline constexpr GamutTriangle kGamutDciP3 = {
    {0.6800, 0.3200}, {0.2650, 0.6900}, {0.1500, 0.0600}, {0.3127, 0.3290},
};

inline constexpr GamutTriangle kGamutBt2020 = {
    {0.7080, 0.2920}, {0.1700, 0.7970}, {0.1310, 0.0460}, {0.3127, 0.3290},
};

inline constexpr double kCiePlotMin = -0.04;
inline constexpr double kCiePlotMax = 0.88;
inline constexpr double kCiePlotSpan = kCiePlotMax - kCiePlotMin;

inline double polygon_area(const std::vector<CiePoint>& poly) {
    if (poly.size() < 3) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const CiePoint& a = poly[i];
        const CiePoint& b = poly[(i + 1) % poly.size()];
        sum += a.x * b.y - b.x * a.y;
    }
    return 0.5 * std::abs(sum);
}

inline double triangle_area(const GamutTriangle& t) {
    return std::abs((t.r.x * (t.g.y - t.b.y) + t.g.x * (t.b.y - t.r.y) + t.b.x * (t.r.y - t.g.y)) * 0.5);
}

inline bool point_in_triangle(const CiePoint& p, const GamutTriangle& t) {
    auto sign = [](const CiePoint& p1, const CiePoint& p2, const CiePoint& p3) {
        return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
    };
    double d1 = sign(p, t.r, t.g);
    double d2 = sign(p, t.g, t.b);
    double d3 = sign(p, t.b, t.r);
    bool has_neg = (d1 < -1e-9) || (d2 < -1e-9) || (d3 < -1e-9);
    bool has_pos = (d1 > 1e-9) || (d2 > 1e-9) || (d3 > 1e-9);
    return !(has_neg && has_pos);
}

inline bool point_in_cie_locus(const CiePoint& p) {
    if (p.x < 0.0 || p.y <= 0.0 || p.x + p.y > 1.0) {
        return false;
    }
    bool inside = false;
    const size_t n = kCie1931Locus.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        double xi = kCie1931Locus[i].x, yi = kCie1931Locus[i].y;
        double xj = kCie1931Locus[j].x, yj = kCie1931Locus[j].y;
        bool intersect = ((yi > p.y) != (yj > p.y)) &&
                         (p.x < (xj - xi) * (p.y - yi) / ((yj - yi) != 0.0 ? (yj - yi) : 1e-12) + xi);
        if (intersect) inside = !inside;
    }
    return inside;
}

inline std::vector<CiePoint> clip_polygon_to_triangle(std::vector<CiePoint> subject, const GamutTriangle& clip_tri) {
    std::vector<CiePoint> clip = {clip_tri.r, clip_tri.g, clip_tri.b};
    // Ensure CCW winding for clip triangle
    double signed_area = (clip[1].x - clip[0].x) * (clip[2].y - clip[0].y) -
                         (clip[2].x - clip[0].x) * (clip[1].y - clip[0].y);
    if (signed_area < 0.0) {
        std::swap(clip[1], clip[2]);
    }

    auto is_inside = [](const CiePoint& p, const CiePoint& e1, const CiePoint& e2) {
        return ((e2.x - e1.x) * (p.y - e1.y) - (e2.y - e1.y) * (p.x - e1.x)) >= -1e-12;
    };

    auto intersect = [](const CiePoint& s, const CiePoint& e, const CiePoint& e1, const CiePoint& e2) {
        double a1 = e.y - s.y;
        double b1 = s.x - e.x;
        double c1 = a1 * s.x + b1 * s.y;
        double a2 = e2.y - e1.y;
        double b2 = e1.x - e2.x;
        double c2 = a2 * e1.x + b2 * e1.y;
        double det = a1 * b2 - a2 * b1;
        if (std::abs(det) < 1e-15) return s;
        return CiePoint{(b2 * c1 - b1 * c2) / det, (a1 * c2 - a2 * c1) / det};
    };

    for (size_t i = 0; i < clip.size(); ++i) {
        if (subject.empty()) break;
        CiePoint e1 = clip[i];
        CiePoint e2 = clip[(i + 1) % clip.size()];
        std::vector<CiePoint> input = std::move(subject);
        subject.clear();
        CiePoint s = input.back();
        for (const CiePoint& e : input) {
            if (is_inside(e, e1, e2)) {
                if (!is_inside(s, e1, e2)) {
                    subject.push_back(intersect(s, e, e1, e2));
                }
                subject.push_back(e);
            } else if (is_inside(s, e1, e2)) {
                subject.push_back(intersect(s, e, e1, e2));
            }
            s = e;
        }
    }
    return subject;
}

// Percentage of `reference` gamut area covered by `measured` gamut [0..100].
inline double gamut_coverage_percent(const GamutTriangle& measured, const GamutTriangle& reference) {
    double ref_area = triangle_area(reference);
    if (ref_area <= 1e-12) return 0.0;
    std::vector<CiePoint> poly = {measured.r, measured.g, measured.b};
    std::vector<CiePoint> clipped = clip_polygon_to_triangle(std::move(poly), reference);
    double inter = polygon_area(clipped);
    return std::clamp((inter / ref_area) * 100.0, 0.0, 100.0);
}

// Ratio of `measured` area relative to `reference` area (can exceed 100%).
inline double gamut_area_ratio_percent(const GamutTriangle& measured, const GamutTriangle& reference) {
    double ref_area = triangle_area(reference);
    if (ref_area <= 1e-12) return 0.0;
    return (triangle_area(measured) / ref_area) * 100.0;
}

// CIE 1976 (u', v') uniform chromaticity coordinates from CIE 1931 (x, y).
inline CiePoint cie_xy_to_uv_prime(const CiePoint& p) {
    double denom = -2.0 * p.x + 12.0 * p.y + 3.0;
    if (std::abs(denom) < 1e-9) return {0.0, 0.0};
    return {(4.0 * p.x) / denom, (9.0 * p.y) / denom};
}

}  // namespace testhdr
