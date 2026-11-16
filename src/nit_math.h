#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace testhdr {

enum class ScaleMode : int32_t {
    PQ = 0,
    Log = 1,
    Linear = 2,
};

enum class ViewMode : int32_t {
    Split = 0,        // Continuous gradient + discrete stepped bands strip
    Smooth = 1,       // Fullscreen continuous gradient
    Bands = 2,        // Fullscreen discrete stepped bands
    Rgbw = 3,         // 4-row White / Red / Green / Blue gradients
    TestPattern = 4,  // Built-in calibration & display test images
    CieTriangle = 5,  // CIE 1931 chromaticity diagram & spectrum limits
};

enum class TestPattern : int32_t {
    GamutBars = 0,           // ColorChecker + SMPTE + Rec.709 vs DCI-P3 vs BT.2020
    Banding10v8 = 1,         // Native 10/16-bit smooth ramp vs quantized 8-bit ramp
    ClippingBlackWhite = 2,  // Near-black crush & peak-white clipping patches
    HdrScene = 3,            // Procedural HDR/WCG scene (sky, shadows, neon, sun)
    SharpnessAnsi = 4,       // 1px/2px grid, 4:4:4 chroma lines & ANSI contrast
    HueSpectrum = 5,         // Full Hue x Saturation/Luminance spectrum sweep
};

inline constexpr int kViewModeCount = 6;
inline constexpr int kTestPatternCount = 6;

enum class ChannelMode : int32_t {
    White = 0,
    Red = 1,
    Green = 2,
    Blue = 3,
};

enum class HdrOutputMode : int32_t {
    Hdr10Pq = 0,
    HdrExtendedLinear = 1,
    Sdr10 = 2,  // 16-bit float scRGB/linear SDR quantized to 10-bit SDR scanout
    Sdr = 3,    // 8-bit sRGB swapchain
};

enum class LightQualityTier : int32_t {
    Low = 0,
    Medium = 1,
    High = 2,
    UltraHigh = 3,
};

inline double estimated_sdr_full_nits(double avg_nits, bool has_avg, double max_nits, bool has_max) {
    if (has_avg && avg_nits > 80.0) {
        return avg_nits;
    }
    if (has_max && max_nits > 80.0) {
        return max_nits;
    }
    return 250.0;
}

inline LightQualityTier classify_light_quality(double peak_nits, bool /*hdr_active*/) {
    if (peak_nits < 250.0) {
        return LightQualityTier::Low;
    }
    if (peak_nits < 450.0) {
        return LightQualityTier::Medium;
    }
    if (peak_nits < 850.0) {
        return LightQualityTier::High;
    }
    return LightQualityTier::UltraHigh;
}

inline const char* light_quality_name(LightQualityTier tier) {
    switch (tier) {
        case LightQualityTier::Low: return "LOW";
        case LightQualityTier::Medium: return "MEDIUM";
        case LightQualityTier::High: return "HIGH";
        case LightQualityTier::UltraHigh: return "ULTRA HIGH";
    }
    return "LOW";
}

// SMPTE ST.2084 (PQ) constants
constexpr double kPqM1 = 2610.0 / 16384.0;
constexpr double kPqM2 = (2523.0 / 4096.0) * 128.0;
constexpr double kPqC1 = 3424.0 / 4096.0;
constexpr double kPqC2 = (2413.0 / 4096.0) * 32.0;
constexpr double kPqC3 = (2392.0 / 4096.0) * 32.0;
constexpr double kPqMaxNits = 10000.0;
constexpr double kLogOffset = 0.01;
constexpr double kScRgbRefWhiteNits = 80.0;

inline double nits_to_pq(double nits) {
    double clamped = std::clamp(nits, 0.0, kPqMaxNits);
    if (clamped <= 0.0) {
        return 0.0;
    }
    double y = clamped / kPqMaxNits;
    double ym1 = std::pow(y, kPqM1);
    double num = kPqC1 + kPqC2 * ym1;
    double den = 1.0 + kPqC3 * ym1;
    return std::pow(num / den, kPqM2);
}

inline double pq_to_nits(double pq) {
    double clamped = std::clamp(pq, 0.0, 1.0);
    if (clamped <= 0.0) {
        return 0.0;
    }
    double p = std::pow(clamped, 1.0 / kPqM2);
    double num = std::max(p - kPqC1, 0.0);
    double den = std::max(kPqC2 - kPqC3 * p, 1e-12);
    return kPqMaxNits * std::pow(num / den, 1.0 / kPqM1);
}

inline int nits_to_pq10(double nits) {
    return static_cast<int>(std::llround(nits_to_pq(nits) * 1023.0));
}

struct ColorQualityInfo {
    int display_bpc = 8;
    int swapchain_bpc = 8;
    int effective_bpc = 8;
    uint64_t levels_per_channel = 256;
    uint64_t total_colors = 16777216ULL;
    int luminance_steps = 256;
    std::string gamut = "sRGB";
    LightQualityTier tier = LightQualityTier::Low;
};

inline int parse_format_bpc(const std::string& hypr_format, int fallback_bpc = 8) {
    if (hypr_format.find("16161616") != std::string::npos) return 16;
    if (hypr_format.find("2101010") != std::string::npos ||
        hypr_format.find("101010") != std::string::npos) {
        return 10;
    }
    if (hypr_format.find("8888") != std::string::npos) return 8;
    return fallback_bpc;
}

inline int swapchain_bpc_for_mode(HdrOutputMode mode) {
    switch (mode) {
        case HdrOutputMode::Hdr10Pq: return 10;
        case HdrOutputMode::HdrExtendedLinear: return 16;
        case HdrOutputMode::Sdr10: return 16;
        case HdrOutputMode::Sdr:
        default: return 8;
    }
}

inline ColorQualityInfo evaluate_color_quality(int display_bpc,
                                               HdrOutputMode swapchain_mode,
                                               bool hdr_active,
                                               uint32_t wl_primaries,
                                               const std::string& hypr_cm_preset,
                                               double active_min_nits,
                                               double active_max_nits) {
    ColorQualityInfo info;
    info.display_bpc = std::max(8, display_bpc);
    info.swapchain_bpc = swapchain_bpc_for_mode(swapchain_mode);
    info.effective_bpc = std::min(info.display_bpc, info.swapchain_bpc);

    if (info.effective_bpc >= 16) {
        info.levels_per_channel = 65536ULL;
        info.total_colors = 281474976710656ULL;  // 65536^3
    } else {
        info.levels_per_channel = 1ULL << info.effective_bpc;
        info.total_colors = info.levels_per_channel * info.levels_per_channel * info.levels_per_channel;
    }

    // Wayland wp_color_manager_v1.primaries: 1=srgb, 6=bt2020, 8=dci_p3, 9=display_p3
    if (wl_primaries == 6 || (hdr_active && swapchain_mode == HdrOutputMode::Hdr10Pq)) {
        info.gamut = "BT.2020";
    } else if (wl_primaries == 8 || wl_primaries == 9 ||
               hypr_cm_preset == "dcip3" || hypr_cm_preset == "dp3" ||
               hypr_cm_preset == "wide" || hypr_cm_preset == "adobe" ||
               (hdr_active && swapchain_mode == HdrOutputMode::HdrExtendedLinear)) {
        info.gamut = "DCI-P3";
    } else {
        info.gamut = "sRGB";
    }

    if (info.effective_bpc >= 10 && hdr_active) {
        int lo = nits_to_pq10(active_min_nits);
        int hi = nits_to_pq10(active_max_nits);
        info.luminance_steps = std::max(1, hi - lo + 1);
    } else {
        info.luminance_steps = static_cast<int>(std::min<uint64_t>(info.levels_per_channel, 65536ULL));
    }

    if (info.effective_bpc < 10) {
        info.tier = (info.gamut == "sRGB") ? LightQualityTier::Low : LightQualityTier::Medium;
    } else {
        if (info.gamut == "BT.2020" && hdr_active) {
            info.tier = LightQualityTier::UltraHigh;
        } else if (info.gamut == "DCI-P3" || info.gamut == "BT.2020") {
            info.tier = LightQualityTier::High;
        } else {
            info.tier = LightQualityTier::Medium;
        }
    }
    return info;
}

inline double t_to_nits(double t, double min_nits, double max_nits, ScaleMode mode) {
    double u = std::clamp(t, 0.0, 1.0);
    double lo = std::max(0.0, min_nits);
    double hi = std::max(lo + 1e-6, max_nits);

    switch (mode) {
        case ScaleMode::PQ: {
            double p_lo = nits_to_pq(lo);
            double p_hi = nits_to_pq(hi);
            return pq_to_nits(p_lo + u * (p_hi - p_lo));
        }
        case ScaleMode::Log: {
            double l_lo = std::log10(lo + kLogOffset);
            double l_hi = std::log10(hi + kLogOffset);
            double nits = std::pow(10.0, l_lo + u * (l_hi - l_lo)) - kLogOffset;
            return std::max(0.0, nits);
        }
        case ScaleMode::Linear:
        default:
            return lo + u * (hi - lo);
    }
}

inline double nits_to_t(double nits, double min_nits, double max_nits, ScaleMode mode) {
    double lo = std::max(0.0, min_nits);
    double hi = std::max(lo + 1e-6, max_nits);

    switch (mode) {
        case ScaleMode::PQ: {
            double p_lo = nits_to_pq(lo);
            double p_hi = nits_to_pq(hi);
            double p = nits_to_pq(nits);
            if (p_hi <= p_lo) return 0.0;
            return (p - p_lo) / (p_hi - p_lo);
        }
        case ScaleMode::Log: {
            double l_lo = std::log10(lo + kLogOffset);
            double l_hi = std::log10(hi + kLogOffset);
            double l = std::log10(std::max(0.0, nits) + kLogOffset);
            if (l_hi <= l_lo) return 0.0;
            return (l - l_lo) / (l_hi - l_lo);
        }
        case ScaleMode::Linear:
        default:
            return (nits - lo) / (hi - lo);
    }
}

inline double band_index_to_nits(int index, int band_count, double min_nits, double max_nits, ScaleMode mode) {
    if (band_count <= 1) {
        return min_nits;
    }
    int idx = std::clamp(index, 0, band_count - 1);
    double t = static_cast<double>(idx) / static_cast<double>(band_count - 1);
    return t_to_nits(t, min_nits, max_nits, mode);
}

inline double t_to_band_nits(double t, int band_count, double min_nits, double max_nits, ScaleMode mode) {
    if (band_count <= 1) {
        return t_to_nits(t, min_nits, max_nits, mode);
    }
    double u = std::clamp(t, 0.0, 1.0);
    int idx = std::min(band_count - 1, static_cast<int>(std::floor(u * band_count)));
    return band_index_to_nits(idx, band_count, min_nits, max_nits, mode);
}

}  // namespace testhdr
