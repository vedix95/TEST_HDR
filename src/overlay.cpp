#include "overlay.h"
#include "cie_math.h"
#include "icc_gen.h"

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace testhdr {
namespace {

std::string format_nits(double nits, bool with_unit = false) {
    char buf[64];
    if (nits < 0.0005) {
        std::snprintf(buf, sizeof(buf), "0");
    } else if (nits < 0.1) {
        std::snprintf(buf, sizeof(buf), "%.3f", nits);
    } else if (nits < 10.0) {
        if (std::abs(nits - std::round(nits)) < 1e-4) {
            std::snprintf(buf, sizeof(buf), "%.0f", std::round(nits));
        } else {
            std::snprintf(buf, sizeof(buf), "%.2f", nits);
        }
    } else if (nits < 100.0) {
        if (std::abs(nits - std::round(nits)) < 1e-3) {
            std::snprintf(buf, sizeof(buf), "%.0f", std::round(nits));
        } else {
            std::snprintf(buf, sizeof(buf), "%.1f", nits);
        }
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f", std::round(nits));
    }
    if (with_unit) {
        return std::string(buf) + " nits";
    }
    return std::string(buf);
}

const char* scale_mode_name(ScaleMode m) {
    switch (m) {
        case ScaleMode::PQ: return "PQ (ST.2084 perceptuel)";
        case ScaleMode::Log: return "Logarithmique (Log10)";
        case ScaleMode::Linear: return "Linéaire (nits)";
    }
    return "";
}

const char* view_mode_name(ViewMode v) {
    switch (v) {
        case ViewMode::Split: return "Mixte (Continu + Paliers)";
        case ViewMode::Smooth: return "Dégradé continu";
        case ViewMode::Bands: return "Bandes discrètes";
        case ViewMode::Rgbw: return "Canaux R/V/B/Blanc";
        case ViewMode::TestPattern: return "Mires & Images de test";
        case ViewMode::CieTriangle: return "Triangle CIE 1931 (Spectres)";
    }
    return "";
}

const char* test_pattern_name(TestPattern p) {
    switch (p) {
        case TestPattern::GamutBars: return "1/6 — Gamuts (Rec.709 / DCI-P3 / BT.2020) & ColorChecker";
        case TestPattern::Banding10v8: return "2/6 — Banding 10-bit/16-bit natif vs 8-bit quantifié";
        case TestPattern::ClippingBlackWhite: return "3/6 — Clipping Noirs profonds & Blancs max";
        case TestPattern::HdrScene: return "4/6 — Scène synthétique HDR / Wide Gamut";
        case TestPattern::SharpnessAnsi: return "5/6 — Netteté 1px, Chroma 4:4:4 & Contraste ANSI";
        case TestPattern::HueSpectrum: return "6/6 — Spectre complet Teinte × Saturation × Gamut";
    }
    return "";
}

const char* channel_mode_name(ChannelMode c) {
    switch (c) {
        case ChannelMode::White: return "Blanc (D65)";
        case ChannelMode::Red: return "Rouge";
        case ChannelMode::Green: return "Vert";
        case ChannelMode::Blue: return "Bleu";
    }
    return "";
}

const char* hdr_output_mode_name(HdrOutputMode m) {
    switch (m) {
        case HdrOutputMode::Hdr10Pq: return "HDR10 ST.2084 (10-bit PQ)";
        case HdrOutputMode::HdrExtendedLinear: return "HDR Extended Linear (16-bit scRGB)";
        case HdrOutputMode::Sdr10: return "SDR 10-bit (16-bit linéaire → 10-bit)";
        case HdrOutputMode::Sdr: return "SDR 8-bit (sRGB)";
    }
    return "";
}

void draw_rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r) {
    r = std::min(r, std::min(w, h) * 0.5);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

struct TextMetrics {
    int width = 0;
    int height = 0;
};

PangoLayout* make_layout(cairo_t* cr, const std::string& text, const char* font_desc_str, double scale) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* desc = pango_font_description_from_string(font_desc_str);
    int base_size = pango_font_description_get_size(desc);
    if (base_size > 0) {
        pango_font_description_set_size(desc, static_cast<int>(base_size * scale));
    }
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, text.c_str(), -1);
    return layout;
}

TextMetrics measure_layout(PangoLayout* layout) {
    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    return {w, h};
}

void draw_text(cairo_t* cr, double x, double y, const std::string& text,
               const char* font_desc_str, double scale,
               double r, double g, double b, double a) {
    PangoLayout* layout = make_layout(cr, text, font_desc_str, scale);
    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    g_object_unref(layout);
}

void draw_edid_marker(cairo_t* cr, double x, double top_y, double bottom_y,
                      double scale, const std::string& label,
                      double r, double g, double b) {
    if (x < 0.0 || x > cairo_image_surface_get_width(cairo_get_target(cr))) return;

    cairo_save(cr);
    double dashes[2] = {8.0 * scale, 6.0 * scale};
    cairo_set_dash(cr, dashes, 2, 0.0);
    cairo_set_line_width(cr, std::max(1.5, 2.0 * scale));
    cairo_set_source_rgba(cr, r, g, b, 0.90);
    cairo_move_to(cr, x, top_y);
    cairo_line_to(cr, x, bottom_y);
    cairo_stroke(cr);
    cairo_restore(cr);

    PangoLayout* layout = make_layout(cr, label, "Sans Bold 11", scale);
    TextMetrics tm = measure_layout(layout);
    double pad_x = 8.0 * scale;
    double pad_y = 4.0 * scale;
    double pill_w = tm.width + 2.0 * pad_x;
    double pill_h = tm.height + 2.0 * pad_y;
    double max_w = static_cast<double>(cairo_image_surface_get_width(cairo_get_target(cr)));
    double px = std::clamp(x - pill_w * 0.5, 6.0 * scale, max_w - pill_w - 6.0 * scale);
    double py = top_y + 10.0 * scale;

    cairo_save(cr);
    draw_rounded_rect(cr, px, py, pill_w, pill_h, 5.0 * scale);
    cairo_set_source_rgba(cr, 0.03, 0.03, 0.04, 0.92);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, std::max(1.0, 1.5 * scale));
    cairo_set_source_rgba(cr, r, g, b, 0.95);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, r, g, b, 1.0);
    cairo_move_to(cr, px + pad_x, py + pad_y);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    g_object_unref(layout);
}

struct TickCandidate {
    double nits;
    int priority;  // Higher = kept first during label collision avoidance
    bool is_edid;
};

void draw_nit_ruler(cairo_t* cr, const OverlayState& state, double ruler_top) {
    const double W = state.width_px;
    const double H = state.height_px;
    const double s = state.ui_scale;

    // Axis baseline
    cairo_save(cr);
    cairo_set_line_width(cr, std::max(1.0, 1.5 * s));
    cairo_set_source_rgba(cr, 0.65, 0.70, 0.78, 0.85);
    cairo_move_to(cr, 0.0, ruler_top + 1.0);
    cairo_line_to(cr, W, ruler_top + 1.0);
    cairo_stroke(cr);
    cairo_restore(cr);

    static constexpr double kStandardTicks[] = {
        0.0, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
        1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 80.0, 100.0,
        150.0, 200.0, 300.0, 400.0, 500.0, 600.0, 700.0, 800.0,
        1000.0, 1500.0, 2000.0, 3000.0, 4000.0, 6000.0, 8000.0, 10000.0
    };

    std::vector<TickCandidate> candidates;
    candidates.push_back({state.min_nits, 100, false});
    candidates.push_back({state.max_nits, 100, false});

    if (state.edid.hdr_capable) {
        if (state.edid.has_min) candidates.push_back({state.edid.min_nits, 95, true});
        if (state.edid.has_avg) candidates.push_back({state.edid.avg_nits, 95, true});
        if (state.edid.has_max) candidates.push_back({state.edid.max_nits, 95, true});
    }

    for (double v : kStandardTicks) {
        if (v < state.min_nits - 1e-6 || v > state.max_nits + 1e-6) continue;
        int prio = 40;
        if (v == 0.0 || v == 1.0 || v == 10.0 || v == 100.0 || v == 400.0 ||
            v == 1000.0 || v == 4000.0 || v == 10000.0) {
            prio = 80;
        } else if (v == 0.01 || v == 0.1 || v == 50.0 || v == 200.0 || v == 600.0 || v == 2000.0) {
            prio = 65;
        }
        candidates.push_back({v, prio, false});
    }

    // Sort by pixel x, deduplicating very close values
    struct TickPlaced {
        double x;
        double nits;
        int priority;
        bool is_edid;
        std::string label;
        int label_w;
        int label_h;
    };

    std::vector<TickPlaced> ticks;
    for (const auto& c : candidates) {
        if (c.nits < state.min_nits - 1e-6 || c.nits > state.max_nits + 1e-6) continue;
        double t = nits_to_t(c.nits, state.min_nits, state.max_nits, state.scale_mode);
        double x = std::clamp(t, 0.0, 1.0) * std::max(1.0, W - 1.0);
        bool dup = false;
        for (auto& existing : ticks) {
            if (std::abs(existing.x - x) < 6.0 * s) {
                if (c.priority > existing.priority) {
                    existing.x = x;
                    existing.nits = c.nits;
                    existing.priority = c.priority;
                    existing.is_edid = c.is_edid;
                }
                dup = true;
                break;
            }
        }
        if (!dup) {
            ticks.push_back({x, c.nits, c.priority, c.is_edid, "", 0, 0});
        }
    }

    // Measure labels
    for (auto& tk : ticks) {
        tk.label = format_nits(tk.nits, false);
        PangoLayout* layout = make_layout(cr, tk.label, tk.is_edid ? "Monospace Bold 11" : "Monospace 11", s);
        TextMetrics tm = measure_layout(layout);
        tk.label_w = tm.width;
        tk.label_h = tm.height;
        g_object_unref(layout);
    }

    // Select non-overlapping labels in priority order
    std::sort(ticks.begin(), ticks.end(), [](const TickPlaced& a, const TickPlaced& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.x < b.x;
    });

    std::vector<std::pair<double, double>> occupied;
    std::vector<bool> draw_label(ticks.size(), false);
    double min_gap = 12.0 * s;

    for (size_t i = 0; i < ticks.size(); ++i) {
        double half = ticks[i].label_w * 0.5;
        double lx = std::clamp(ticks[i].x - half, 4.0 * s, W - ticks[i].label_w - 4.0 * s);
        double rx = lx + ticks[i].label_w;
        bool overlaps = false;
        for (const auto& span : occupied) {
            if (!(rx + min_gap < span.first || lx - min_gap > span.second)) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) {
            draw_label[i] = true;
            occupied.emplace_back(lx, rx);
        }
    }

    // Draw ticks and selected labels
    for (size_t i = 0; i < ticks.size(); ++i) {
        const auto& tk = ticks[i];
        double tick_len = draw_label[i] ? (14.0 * s) : (7.0 * s);
        cairo_save(cr);
        cairo_set_line_width(cr, std::max(1.0, (draw_label[i] ? 2.0 : 1.2) * s));
        if (tk.is_edid) {
            cairo_set_source_rgba(cr, 0.35, 0.92, 0.65, 0.95);
        } else {
            cairo_set_source_rgba(cr, 0.85, 0.88, 0.92, draw_label[i] ? 0.95 : 0.50);
        }
        cairo_move_to(cr, tk.x, ruler_top + 1.0);
        cairo_line_to(cr, tk.x, ruler_top + 1.0 + tick_len);
        cairo_stroke(cr);
        cairo_restore(cr);

        if (draw_label[i]) {
            double lx = std::clamp(tk.x - tk.label_w * 0.5, 4.0 * s, W - tk.label_w - 4.0 * s);
            double ly = ruler_top + tick_len + 4.0 * s;
            if (tk.is_edid) {
                draw_text(cr, lx, ly, tk.label, "Monospace Bold 11", s, 0.35, 0.95, 0.68, 1.0);
            } else {
                draw_text(cr, lx, ly, tk.label, "Monospace 11", s, 0.92, 0.94, 0.97, 0.95);
            }
            // Small "nits" subtitle on rightmost or leftmost tick
            if (std::abs(tk.nits - state.max_nits) < 1e-4 && ly + tk.label_h + 18.0 * s < H - 24.0 * s) {
                draw_text(cr, std::max(4.0 * s, lx + tk.label_w - 28.0 * s), ly + tk.label_h + 1.0 * s,
                          "nits", "Sans 9", s, 0.65, 0.70, 0.78, 0.85);
            }
        }
    }

    // Bottom shortcut legend is drawn via draw_bottom_legend
}

void draw_bottom_legend(cairo_t* cr, const OverlayState& state) {
    const double W = state.width_px;
    const double H = state.height_px;
    const double s = state.ui_scale;
    const bool hdr_active = state.window_hdr_enabled &&
                            state.hdr_output_mode != HdrOutputMode::Sdr &&
                            state.hdr_output_mode != HdrOutputMode::Sdr10;
    std::string sdr_bright = (!hdr_active && state.mode_limits.backlight_available)
        ? "  [+/-] Lum. SDR"
        : "";
    std::string legend =
        "[↑/↓] Menu  [←/→/Entrée] Changer / Valider  [Shift+Flèches] Plage nits" + sdr_bright +
        "  [I] Mires  [T] Triangle CIE  [H] Masquer HUD  [Esc/Q] Quitter";
    PangoLayout* leg_layout = make_layout(cr, legend, "Sans 9.5", s);
    TextMetrics lm = measure_layout(leg_layout);
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.62, 0.67, 0.75, 0.88);
    cairo_move_to(cr, std::max(8.0 * s, (W - lm.width) * 0.5), H - lm.height - 8.0 * s);
    pango_cairo_show_layout(cr, leg_layout);
    cairo_restore(cr);
    g_object_unref(leg_layout);
}

void draw_band_labels(cairo_t* cr, const OverlayState& state, double band_top, double band_bottom) {
    const double W = state.width_px;
    const double s = state.ui_scale;
    int n = std::max(2, state.band_count);
    double band_w = W / static_cast<double>(n);
    if (band_w < 38.0 * s) {
        return;  // Too narrow to label cleanly without clutter
    }

    for (int i = 0; i < n; ++i) {
        double nits = band_index_to_nits(i, n, state.min_nits, state.max_nits, state.scale_mode);
        std::string txt = format_nits(nits, false);
        PangoLayout* layout = make_layout(cr, txt, "Monospace Bold 10", s);
        TextMetrics tm = measure_layout(layout);
        if (tm.width + 6.0 * s <= band_w) {
            double cx = (static_cast<double>(i) + 0.5) * band_w;
            double tx = cx - tm.width * 0.5;
            double ty = band_bottom - tm.height - 8.0 * s;
            if (ty > band_top + 6.0 * s) {
                double pad = 4.0 * s;
                cairo_save(cr);
                draw_rounded_rect(cr, tx - pad, ty - 2.0 * s, tm.width + 2.0 * pad, tm.height + 4.0 * s, 4.0 * s);
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.78);
                cairo_fill(cr);
                cairo_set_source_rgba(cr, 0.96, 0.97, 0.99, 0.96);
                cairo_move_to(cr, tx, ty);
                pango_cairo_show_layout(cr, layout);
                cairo_restore(cr);
            }
        }
        g_object_unref(layout);
    }
}

void draw_rgbw_labels(cairo_t* cr, const OverlayState& state, double area_bottom) {
    const double s = state.ui_scale;
    static const char* kRowNames[4] = {"BLANC (D65)", "ROUGE", "VERT", "BLEU"};
    static const double kRowColors[4][3] = {
        {0.95, 0.95, 0.98},
        {1.00, 0.35, 0.35},
        {0.35, 1.00, 0.45},
        {0.40, 0.65, 1.00},
    };
    double row_h = area_bottom / 4.0;
    for (int r = 0; r < 4; ++r) {
        double ry = r * row_h + 10.0 * s;
        PangoLayout* layout = make_layout(cr, kRowNames[r], "Sans Bold 11", s);
        TextMetrics tm = measure_layout(layout);
        double pad_x = 8.0 * s;
        double pad_y = 4.0 * s;
        double rx = 14.0 * s;
        cairo_save(cr);
        draw_rounded_rect(cr, rx, ry, tm.width + 2.0 * pad_x, tm.height + 2.0 * pad_y, 5.0 * s);
        cairo_set_source_rgba(cr, 0.03, 0.03, 0.04, 0.85);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, kRowColors[r][0], kRowColors[r][1], kRowColors[r][2], 0.98);
        cairo_move_to(cr, rx + pad_x, ry + pad_y);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
        g_object_unref(layout);
    }
}

struct HudRow {
    std::string text;
    const char* font;
    double r, g, b, a;
};

struct CardBox {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
};

// anchor_corner: 0 = top-left (ax, ay), 1 = bottom-left (ax, ay=bottom), 2 = bottom-right (ax=right, ay=bottom)
CardBox draw_info_card(cairo_t* cr,
                       double s,
                       const std::vector<HudRow>& rows,
                       double border_r,
                       double border_g,
                       double border_b,
                       double border_a,
                       int anchor_corner,
                       double ax,
                       double ay,
                       double min_top_y = 0.0) {
    if (rows.empty()) return {};
    double max_w = 0.0;
    double total_h = 0.0;
    double row_gap = 4.0 * s;
    std::vector<PangoLayout*> layouts;
    std::vector<TextMetrics> metrics;
    layouts.reserve(rows.size());
    metrics.reserve(rows.size());
    for (const auto& r : rows) {
        PangoLayout* l = make_layout(cr, r.text, r.font, s);
        TextMetrics tm = measure_layout(l);
        max_w = std::max(max_w, static_cast<double>(tm.width));
        total_h += tm.height + row_gap;
        layouts.push_back(l);
        metrics.push_back(tm);
    }

    double pad_x = 13.0 * s;
    double pad_y = 10.0 * s;
    double box_w = max_w + 2.0 * pad_x;
    double box_h = total_h - row_gap + 2.0 * pad_y;

    double x = ax;
    double y = ay;
    if (anchor_corner == 1) {
        y = std::max(min_top_y, ay - box_h);
    } else if (anchor_corner == 2) {
        x = ax - box_w;
        y = std::max(min_top_y, ay - box_h);
    }

    cairo_save(cr);
    draw_rounded_rect(cr, x, y, box_w, box_h, 8.0 * s);
    cairo_set_source_rgba(cr, 0.03, 0.04, 0.06, 0.91);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, std::max(1.0, 1.25 * s));
    cairo_set_source_rgba(cr, border_r, border_g, border_b, border_a);
    cairo_stroke(cr);

    double cy = y + pad_y;
    for (size_t i = 0; i < layouts.size(); ++i) {
        cairo_set_source_rgba(cr, rows[i].r, rows[i].g, rows[i].b, rows[i].a);
        cairo_move_to(cr, x + pad_x, cy);
        pango_cairo_show_layout(cr, layouts[i]);
        cy += metrics[i].height + row_gap;
        g_object_unref(layouts[i]);
    }
    cairo_restore(cr);
    return {x, y, box_w, box_h};
}

std::pair<std::string, std::string> corresponding_icc_info(const OverlayState& state) {
    if (!state.mode_limits.active_icc_path.empty()) {
        std::string fn = std::filesystem::path(state.mode_limits.active_icc_path).filename().string();
        return {fn, "Identique au profil Hyprland actif"};
    }
    std::string prefix = build_aggregated_screen_prefix(
        state.edid, "", state.mode_limits.hypr_description);
    if (state.hdr_output_mode == HdrOutputMode::Hdr10Pq) {
        return {prefix + "-BT2020-PQ.icc", "BT.2020 ST.2084 PQ (HDR10)"};
    }
    if (state.hdr_output_mode == HdrOutputMode::HdrExtendedLinear) {
        return {prefix + "-BT2020-Linear.icc", "BT.2020 Linéaire (scRGB HDR)"};
    }
    if (state.hdr_output_mode == HdrOutputMode::Sdr10) {
        if (state.mode_limits.hypr_cm_preset == "dcip3" ||
            state.mode_limits.hypr_cm_preset == "wide" ||
            state.mode_limits.hypr_cm_preset == "dp3" ||
            state.mode_limits.wl_primaries == 8 ||
            state.mode_limits.wl_primaries == 9) {
            return {prefix + "-DCI-P3-D65.icc", "DCI-P3 D65 (10-bit Wide SDR)"};
        }
        return {prefix + "-sRGB-Calibrated.icc", "sRGB Calibré (SDR 10-bit)"};
    }
    return {prefix + "-sRGB-Calibrated.icc", "sRGB Calibré (SDR 8-bit)"};
}

struct CornerCardsLayout {
    CardBox top_left;
    CardBox bottom_left;
    CardBox bottom_right;
};

CornerCardsLayout draw_corner_hud_cards(cairo_t* cr, const OverlayState& state, double bottom_y) {
    const double W = state.width_px;
    const double s = state.ui_scale;
    const double margin_x = 18.0 * s;
    const double top_y = 16.0 * s;

    bool hdr_active = state.window_hdr_enabled &&
                      state.hdr_output_mode != HdrOutputMode::Sdr &&
                      state.hdr_output_mode != HdrOutputMode::Sdr10;
    int fallback_bpc_pre = (state.window_hdr_enabled ? 10 : 8);
    int display_bpc_pre = parse_format_bpc(state.mode_limits.hypr_format, fallback_bpc_pre);

    std::string status_line;
    if (hdr_active) {
        status_line = "● HDR ACTIF";
    } else if (state.hdr_output_mode == HdrOutputMode::Sdr10) {
        status_line = (display_bpc_pre >= 10)
            ? "● MODE 10-BIT SDR ACTIF (Sortie linéaire haute précision)"
            : "● MODE 10-BIT SDR (Dalle actuellement 8-bit)";
    } else if (state.hdr_output_mode == HdrOutputMode::Sdr) {
        status_line = "● MODE 8-BIT SDR (sRGB standard)";
    } else {
        status_line = "● HDR INACTIF SUR LE COMPOSITEUR (Activez HDR dans Hyprland / Monique)";
    }

    char sortie_line[256];
    std::snprintf(sortie_line, sizeof(sortie_line), "Sortie GPU : %s", hdr_output_mode_name(state.hdr_output_mode));
    if (state.window_hdr_headroom > 0.01f) {
        char hr_buf[64];
        std::snprintf(hr_buf, sizeof(hr_buf), "  |  Headroom SDL : %.2fx", state.window_hdr_headroom);
        std::string tmp = std::string(sortie_line) + hr_buf;
        std::snprintf(sortie_line, sizeof(sortie_line), "%s", tmp.c_str());
    }

    double sdr_full_est = estimated_sdr_full_nits(
        state.edid.avg_nits, state.edid.has_avg, state.edid.max_nits, state.edid.has_max);
    double cur_peak = state.mode_limits.has_active_limit ? state.mode_limits.active_max_nits : sdr_full_est;
    double cur_min = state.mode_limits.has_active_limit ? state.mode_limits.active_min_nits : 0.2;
    LightQualityTier cur_tier = classify_light_quality(cur_peak, hdr_active);

    std::string quality_line = std::string("Lumière : ") + light_quality_name(cur_tier) +
                               " (" + format_nits(cur_peak, true) +
                               (hdr_active ? " pic HDR)" : " SDR actuel)");
    if (!hdr_active) {
        if (state.mode_limits.backlight_available) {
            quality_line += "  •  Dalle " + std::to_string(state.mode_limits.backlight_percent) +
                            "% (max " + format_nits(sdr_full_est, true) + ") [+/-]";
        }
        if (state.edid.hdr_capable && state.edid.has_max) {
            LightQualityTier edid_tier = classify_light_quality(state.edid.max_nits, true);
            quality_line += std::string("  •  Pic HDR : ") +
                            light_quality_name(edid_tier) + " (" + format_nits(state.edid.max_nits, true) + ")";
        }
    } else if (cur_min <= 0.01) {
        quality_line += "  •  Noirs profonds (" + format_nits(cur_min, true) + ")";
    }

    int display_bpc = parse_format_bpc(state.mode_limits.hypr_format, fallback_bpc_pre);
    std::string effective_cm_for_color = state.mode_limits.hypr_cm_preset;
    if (!state.mode_limits.active_icc_path.empty() &&
        (state.mode_limits.active_icc_path.find("EDID-Native") != std::string::npos ||
         state.mode_limits.active_icc_path.find("DCI-P3") != std::string::npos)) {
        effective_cm_for_color = "dcip3";
    }
    ColorQualityInfo cqi = evaluate_color_quality(
        display_bpc, state.hdr_output_mode, hdr_active,
        state.mode_limits.wl_primaries, effective_cm_for_color, cur_min, cur_peak);

    auto format_u64_spaces = [](uint64_t val) {
        std::string raw = std::to_string(val);
        std::string out;
        int cnt = 0;
        for (auto it = raw.rbegin(); it != raw.rend(); ++it) {
            if (cnt > 0 && cnt % 3 == 0) out.push_back(' ');
            out.push_back(*it);
            ++cnt;
        }
        std::reverse(out.begin(), out.end());
        return out;
    };

    char short_colors[32];
    if (cqi.total_colors >= 1000000000ULL) {
        std::snprintf(short_colors, sizeof(short_colors), "%.2f Md",
                      static_cast<double>(cqi.total_colors) / 1e9);
    } else {
        std::snprintf(short_colors, sizeof(short_colors), "%.2f M",
                      static_cast<double>(cqi.total_colors) / 1e6);
    }

    char color_line[256];
    std::snprintf(color_line, sizeof(color_line),
                  "Couleurs : %s — %d-bit/ch (%llu³ = %s / %s) | %s | %d pas %s",
                  light_quality_name(cqi.tier),
                  cqi.effective_bpc,
                  static_cast<unsigned long long>(cqi.levels_per_channel),
                  format_u64_spaces(cqi.total_colors).c_str(),
                  short_colors,
                  cqi.gamut.c_str(),
                  cqi.luminance_steps,
                  (cqi.effective_bpc >= 10 && hdr_active) ? "PQ" : "SDR");

    auto tier_color = [](LightQualityTier t, double& r, double& g, double& b) {
        if (t == LightQualityTier::Medium) {
            r = 0.98; g = 0.80; b = 0.25;
        } else if (t == LightQualityTier::High) {
            r = 0.32; g = 0.95; b = 0.58;
        } else if (t == LightQualityTier::UltraHigh) {
            r = 0.35; g = 0.92; b = 1.0;
        } else {
            r = 0.95; g = 0.42; b = 0.30;
        }
    };

    // 1. TOP-LEFT BLOCK: Live Mode, Quality & Status
    std::vector<HudRow> tl_rows;
    tl_rows.push_back({"TEST HDR — " + build_live_header_title(state), "Sans Bold 12", 0.98, 0.98, 1.0, 1.0});
    if (hdr_active) {
        tl_rows.push_back({status_line, "Sans Bold 10.5", 0.28, 0.94, 0.55, 1.0});
    } else if (state.hdr_output_mode == HdrOutputMode::Sdr10 && display_bpc_pre >= 10) {
        tl_rows.push_back({status_line, "Sans Bold 10.5", 0.35, 0.92, 1.0, 1.0});
    } else {
        tl_rows.push_back({status_line, "Sans Bold 10.5", 1.0, 0.52, 0.28, 1.0});
    }
    if (!state.mode_limits.monique_active_profile.empty()) {
        tl_rows.push_back({"Profil Monique : " + state.mode_limits.monique_active_profile,
                           "Sans Bold 10", 0.78, 0.88, 1.0, 0.98});
    }
    tl_rows.push_back({sortie_line, "Sans 10", 0.82, 0.86, 0.92, 0.95});
    double qr = 0.0, qg = 0.0, qb = 0.0;
    tier_color(cur_tier, qr, qg, qb);
    tl_rows.push_back({quality_line, "Sans Bold 10", qr, qg, qb, 1.0});
    double cr_col = 0.0, cg_col = 0.0, cb_col = 0.0;
    tier_color(cqi.tier, cr_col, cg_col, cb_col);
    tl_rows.push_back({color_line, "Sans Bold 10", cr_col, cg_col, cb_col, 1.0});
    if (state.mode_limits.hypr_available && !hdr_active &&
        (state.mode_limits.hypr_cm_preset == "wide" ||
         state.mode_limits.hypr_cm_preset == "dcip3" ||
         state.mode_limits.hypr_cm_preset == "auto")) {
        tl_rows.push_back({"Note Wide SDR : Gamut étendu sans courbe PQ ([M] / Menu pour HDR)",
                           "Sans 9.5", 1.0, 0.78, 0.32, 0.96});
    }
    if (!state.system_status_msg.empty()) {
        tl_rows.push_back({state.system_status_msg, "Sans Bold 10", 0.55, 0.92, 0.98, 0.98});
    }

    CornerCardsLayout layout{};
    layout.top_left = draw_info_card(cr, s, tl_rows, 0.35, 0.45, 0.60, 0.72, 0, margin_x, top_y);

    // 2. BOTTOM-LEFT BLOCK: EDID Hardware + Matching ICC Profile (Krita/Darktable) + CIE Limits
    std::vector<HudRow> bl_rows;
    if (state.edid.found) {
        std::string mon = state.edid.connector;
        if (!state.edid.monitor_name.empty()) {
            mon += " (" + state.edid.monitor_name + ")";
        }
        if (state.edid.hdr_capable) {
            char ebuf[256];
            std::snprintf(
                ebuf, sizeof(ebuf),
                "EDID %s : Min %s | Moy %s | Max %s nits (%s)",
                mon.c_str(),
                state.edid.has_min ? format_nits(state.edid.min_nits).c_str() : "?",
                state.edid.has_avg ? format_nits(state.edid.avg_nits).c_str() : "?",
                state.edid.has_max ? format_nits(state.edid.max_nits).c_str() : "?",
                state.edid.supports_pq ? "ST.2084 PQ" : "HDR");
            bl_rows.push_back({ebuf, "Sans Bold 10", 0.50, 0.90, 0.98, 0.98});
        } else {
            bl_rows.push_back({"EDID " + mon + " : SDR uniquement", "Sans 10", 0.82, 0.86, 0.92, 0.95});
        }
    } else {
        bl_rows.push_back({"EDID : Non détecté via /sys/class/drm", "Sans 10", 0.82, 0.86, 0.92, 0.95});
    }

    if (state.edid.has_chromaticity) {
        char cbuf[256];
        std::snprintf(
            cbuf, sizeof(cbuf),
            "Primaires EDID : R(%.3f,%.3f) G(%.3f,%.3f) B(%.3f,%.3f) γ=%.2f",
            state.edid.red_x, state.edid.red_y,
            state.edid.green_x, state.edid.green_y,
            state.edid.blue_x, state.edid.blue_y,
            state.edid.gamma);
        bl_rows.push_back({cbuf, "Sans 9.5", 0.78, 0.84, 0.92, 0.95});
    }

    std::string hypr_icc = state.mode_limits.active_icc_path.empty()
        ? "Aucun (preset CM Hyprland)"
        : std::filesystem::path(state.mode_limits.active_icc_path).filename().string();
    bl_rows.push_back({"ICC Hyprland : " + hypr_icc, "Sans 9.5",
                       state.mode_limits.active_icc_path.empty() ? 0.72 : 0.55,
                       state.mode_limits.active_icc_path.empty() ? 0.78 : 0.95,
                       state.mode_limits.active_icc_path.empty() ? 0.86 : 0.78, 0.95});

    auto [corr_icc, corr_desc] = corresponding_icc_info(state);
    bl_rows.push_back({"ICC correspondant (Krita / Darktable) : " + corr_icc,
                       "Sans Bold 10", 0.35, 0.96, 0.68, 1.0});
    bl_rows.push_back({"  → Espace : " + corr_desc + "  (~/.local/share/color/icc)",
                       "Sans 9", 0.62, 0.88, 0.96, 0.94});

    if (state.view_mode == ViewMode::CieTriangle) {
        int active_id = resolve_active_gamut_id(state);
        GamutTriangle edid_tri = state.edid.has_chromaticity
            ? GamutTriangle{{state.edid.red_x, state.edid.red_y},
                            {state.edid.green_x, state.edid.green_y},
                            {state.edid.blue_x, state.edid.blue_y},
                            {state.edid.white_x, state.edid.white_y}}
            : kGamutSrgb;
        const GamutTriangle active_tri = (active_id == 3)
            ? kGamutBt2020
            : (active_id == 1 ? kGamutDciP3 : (active_id == 2 ? edid_tri : kGamutSrgb));
        const char* active_name = (active_id == 3)
            ? "BT.2020"
            : (active_id == 1 ? "DCI-P3 D65" : (active_id == 2 ? "EDID Natif" : "sRGB"));
        char cl1[160], cl2[160];
        std::snprintf(cl1, sizeof(cl1), "Spectre actif (%s ★) : %.1f%% sRGB | %.1f%% DCI-P3 | %.1f%% BT.2020",
                      active_name,
                      gamut_coverage_percent(active_tri, kGamutSrgb),
                      gamut_coverage_percent(active_tri, kGamutDciP3),
                      gamut_coverage_percent(active_tri, kGamutBt2020));
        std::snprintf(cl2, sizeof(cl2), "Dalle EDID : %.1f%% sRGB | %.1f%% DCI-P3 | %.1f%% BT.2020 (Vol %.0f%% sRGB)",
                      gamut_coverage_percent(edid_tri, kGamutSrgb),
                      gamut_coverage_percent(edid_tri, kGamutDciP3),
                      gamut_coverage_percent(edid_tri, kGamutBt2020),
                      gamut_area_ratio_percent(edid_tri, kGamutSrgb));
        bl_rows.push_back({cl1, "Sans Bold 9.5", 0.98, 0.86, 0.38, 0.98});
        bl_rows.push_back({cl2, "Sans 9.5", 0.35, 0.95, 0.62, 0.98});
    }

    layout.bottom_left = draw_info_card(
        cr, s, bl_rows, 0.35, 0.72, 0.85, 0.72, 1,
        margin_x, bottom_y, layout.top_left.y + layout.top_left.h + 10.0 * s);

    // 3. BOTTOM-RIGHT BLOCK: Test Parameters & Active Limit
    std::vector<HudRow> br_rows;
    char br1[256], br2[256], br3[256];
    if (state.view_mode == ViewMode::TestPattern) {
        std::snprintf(br1, sizeof(br1), "Vue : %s  |  HUD : %.0f nits",
                      view_mode_name(state.view_mode), state.hud_nits);
        std::snprintf(br2, sizeof(br2), "Mire : %s", test_pattern_name(state.test_pattern));
    } else {
        std::snprintf(br1, sizeof(br1), "Plage : %s → %s nits  |  Échelle : %s",
                      format_nits(state.min_nits).c_str(),
                      format_nits(state.max_nits).c_str(),
                      scale_mode_name(state.scale_mode));
        std::snprintf(br2, sizeof(br2), "Vue : %s (%d pal.)  |  Canal : %s",
                      view_mode_name(state.view_mode),
                      state.band_count,
                      channel_mode_name(state.channel_mode));
    }
    if (state.mode_limits.has_active_limit) {
        std::snprintf(br3, sizeof(br3), "Limite : %s → %s nits [%s]",
                      format_nits(state.mode_limits.active_min_nits).c_str(),
                      format_nits(state.mode_limits.active_max_nits).c_str(),
                      state.mode_limits.active_source.c_str());
    } else {
        std::snprintf(br3, sizeof(br3), "Limite : Non disponible");
    }
    br_rows.push_back({br1, "Sans 10", 0.84, 0.88, 0.95, 0.96});
    br_rows.push_back({br2, "Sans 10", 0.84, 0.88, 0.95, 0.96});
    br_rows.push_back({br3, "Sans Bold 10", 1.0, 0.44, 0.66, 1.0});
    if (state.mode_limits.hypr_available && !state.mode_limits.hypr_cm_preset.empty()) {
        std::string hline = "Hyprland : " + state.mode_limits.hypr_cm_preset;
        if (!state.mode_limits.hypr_format.empty()) {
            hline += " (" + state.mode_limits.hypr_format + ")";
        }
        br_rows.push_back({hline, "Sans 9.5", 0.74, 0.82, 0.92, 0.94});
    }

    double menu_bottom = top_y;
    if (state.menu_visible) {
        double mx = 0.0, my = 0.0, mw = 0.0, mh = 0.0, mhh = 0.0, mrh = 0.0;
        compute_hud_menu_rect(state, mx, my, mw, mh, mhh, mrh);
        menu_bottom = my + mh + 10.0 * s;
    }
    layout.bottom_right = draw_info_card(
        cr, s, br_rows, 0.78, 0.36, 0.56, 0.72, 2,
        W - margin_x, bottom_y, menu_bottom);

    return layout;
}

void draw_current_mode_limit_bar(cairo_t* cr, const OverlayState& state, double ruler_top, double min_left = 0.0) {
    if (!state.show_limit_bar || !state.mode_limits.has_active_limit) return;

    const double W = state.width_px;
    const double s = state.ui_scale;
    const double lim_min = state.mode_limits.active_min_nits;
    const double lim_max = state.mode_limits.active_max_nits;

    double t_min = nits_to_t(lim_min, state.min_nits, state.max_nits, state.scale_mode);
    double t_max = nits_to_t(lim_max, state.min_nits, state.max_nits, state.scale_mode);
    double x_min_raw = t_min * (W - 1.0);
    double x_max_raw = t_max * (W - 1.0);

    // Horizontal limit gauge bar just above the nit ruler
    double bar_h = 6.0 * s;
    double bar_y = ruler_top - bar_h;
    double x_start = std::clamp(x_min_raw, 0.0, W);
    double x_end = std::clamp(x_max_raw, 0.0, W);

    cairo_save(cr);
    // Out-of-range / below-min region (dimmed dark)
    if (x_start > 0.0) {
        cairo_set_source_rgba(cr, 0.18, 0.20, 0.25, 0.88);
        cairo_rectangle(cr, 0.0, bar_y, x_start, bar_h);
        cairo_fill(cr);
    }
    // Active displayable range in current mode (bright emerald/cyan)
    if (x_end > x_start) {
        cairo_set_source_rgba(cr, 0.22, 0.92, 0.62, 0.95);
        cairo_rectangle(cr, x_start, bar_y, x_end - x_start, bar_h);
        cairo_fill(cr);
    }
    // Beyond current-mode limit (vivid rose-magenta clipping zone)
    if (x_end < W) {
        cairo_set_source_rgba(cr, 1.0, 0.24, 0.50, 0.92);
        cairo_rectangle(cr, x_end, bar_y, W - x_end, bar_h);
        cairo_fill(cr);
    }
    cairo_restore(cr);

    std::string mode_tag;
    if (state.mode_limits.hypr_available && !state.mode_limits.hypr_cm_preset.empty()) {
        mode_tag = " [" + state.mode_limits.hypr_cm_preset + "]";
    }

    // Vertical cutoff bar + prominent badge
    if (x_max_raw >= 0.0 && x_max_raw <= W) {
        cairo_save(cr);
        // Dark outer halo so the bar remains visible even over 1000+ nits white
        cairo_set_line_width(cr, std::max(4.0, 5.5 * s));
        cairo_set_source_rgba(cr, 0.02, 0.02, 0.03, 0.88);
        cairo_move_to(cr, x_max_raw, 0.0);
        cairo_line_to(cr, x_max_raw, ruler_top);
        cairo_stroke(cr);

        // Bright rose-magenta inner bar
        cairo_set_line_width(cr, std::max(2.0, 2.8 * s));
        cairo_set_source_rgba(cr, 1.0, 0.25, 0.52, 1.0);
        cairo_move_to(cr, x_max_raw, 0.0);
        cairo_line_to(cr, x_max_raw, ruler_top);
        cairo_stroke(cr);
        cairo_restore(cr);

        std::string label = "▼ LIMITE ÉCRAN" + mode_tag + " : " + format_nits(lim_max, true);
        PangoLayout* layout = make_layout(cr, label, "Sans Bold 11.5", s);
        TextMetrics tm = measure_layout(layout);
        double pad_x = 10.0 * s;
        double pad_y = 5.0 * s;
        double pill_w = tm.width + 2.0 * pad_x;
        double pill_h = tm.height + 2.0 * pad_y;
        double max_right = W - pill_w - 8.0 * s;
        if (state.menu_visible) {
            double mx = 0.0, my = 0.0, mw = 0.0, mh = 0.0, mhh = 0.0, mrh = 0.0;
            compute_hud_menu_rect(state, mx, my, mw, mh, mhh, mrh);
            if (mx - pill_w - 14.0 * s > min_left + 8.0 * s) {
                max_right = std::min(max_right, mx - pill_w - 14.0 * s);
            }
        }
        double left_bound = (min_left > 8.0 * s && min_left + 12.0 * s <= max_right) ? (min_left + 12.0 * s) : (8.0 * s);
        double px = std::clamp(x_max_raw - pill_w * 0.5, left_bound, std::max(left_bound, max_right));
        double py = 16.0 * s;

        cairo_save(cr);
        draw_rounded_rect(cr, px, py, pill_w, pill_h, 6.0 * s);
        cairo_set_source_rgba(cr, 0.04, 0.02, 0.04, 0.94);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, std::max(1.5, 2.0 * s));
        cairo_set_source_rgba(cr, 1.0, 0.28, 0.55, 1.0);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, 1.0, 0.55, 0.72, 1.0);
        cairo_move_to(cr, px + pad_x, py + pad_y);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
        g_object_unref(layout);
    } else {
        // Off-screen indicator badge so the user always sees the current limit
        std::string label = (x_max_raw > W)
            ? ("LIMITE ÉCRAN" + mode_tag + " : " + format_nits(lim_max, true) + " →")
            : ("← LIMITE ÉCRAN" + mode_tag + " : " + format_nits(lim_max, true));
        PangoLayout* layout = make_layout(cr, label, "Sans Bold 11", s);
        TextMetrics tm = measure_layout(layout);
        double pad_x = 10.0 * s;
        double pad_y = 5.0 * s;
        double pill_w = tm.width + 2.0 * pad_x;
        double pill_h = tm.height + 2.0 * pad_y;
        double px = (x_max_raw > W) ? (W - pill_w - 14.0 * s) : std::max(14.0 * s, min_left + 12.0 * s);
        if (x_max_raw > W && state.menu_visible) {
            double mx = 0.0, my = 0.0, mw = 0.0, mh = 0.0, mhh = 0.0, mrh = 0.0;
            compute_hud_menu_rect(state, mx, my, mw, mh, mhh, mrh);
            if (mx - pill_w - 14.0 * s > min_left + 8.0 * s) {
                px = mx - pill_w - 14.0 * s;
            }
        }
        double py = 16.0 * s;

        cairo_save(cr);
        draw_rounded_rect(cr, px, py, pill_w, pill_h, 6.0 * s);
        cairo_set_source_rgba(cr, 0.04, 0.02, 0.04, 0.94);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, std::max(1.5, 2.0 * s));
        cairo_set_source_rgba(cr, 1.0, 0.28, 0.55, 1.0);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, 1.0, 0.55, 0.72, 1.0);
        cairo_move_to(cr, px + pad_x, py + pad_y);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
        g_object_unref(layout);
    }
}

void draw_crosshair(cairo_t* cr, const OverlayState& state, double ruler_top) {
    if (!state.cursor_active || state.cursor_x_px < 0.0 || state.cursor_x_px >= state.width_px) {
        return;
    }
    if (state.show_hud && state.menu_visible) {
        double mx = 0.0, my = 0.0, mw = 0.0, mh = 0.0, mhh = 0.0, mrh = 0.0;
        compute_hud_menu_rect(state, mx, my, mw, mh, mhh, mrh);
        if (state.cursor_x_px >= mx && state.cursor_x_px <= mx + mw &&
            state.cursor_y_px >= my && state.cursor_y_px <= my + mh) {
            return;
        }
    }
    const double s = state.ui_scale;
    const double W = state.width_px;
    double cx = state.cursor_x_px;
    double cy = std::clamp(state.cursor_y_px, 20.0 * s, ruler_top - 20.0 * s);
    double t = std::clamp(cx / std::max(1.0, W - 1.0), 0.0, 1.0);

    bool in_bands = (state.view_mode == ViewMode::Bands && state.cursor_y_px < ruler_top) ||
                    (state.view_mode == ViewMode::Split &&
                     state.cursor_y_px >= state.height_px * 0.625 &&
                     state.cursor_y_px < ruler_top);

    double nits = in_bands
        ? t_to_band_nits(t, state.band_count, state.min_nits, state.max_nits, state.scale_mode)
        : t_to_nits(t, state.min_nits, state.max_nits, state.scale_mode);

    double pq = nits_to_pq(nits);
    int pq10 = nits_to_pq10(nits);
    double scrgb = nits / kScRgbRefWhiteNits;

    // Vertical hairline
    cairo_save(cr);
    cairo_set_line_width(cr, std::max(1.0, 1.2 * s));
    cairo_set_source_rgba(cr, 1.0, 0.88, 0.25, 0.85);
    cairo_move_to(cr, cx, 0.0);
    cairo_line_to(cr, cx, ruler_top);
    cairo_stroke(cr);
    cairo_restore(cr);

    char b1[96], b2[96], b3[96];
    std::snprintf(b1, sizeof(b1), "Luminance : %s", format_nits(nits, true).c_str());
    std::snprintf(b2, sizeof(b2), "PQ 10-bit : %d / 1023 (%.4f)", pq10, pq);
    std::snprintf(b3, sizeof(b3), "scRGB     : %.3f   |   x = %.0f px", scrgb, cx);

    PangoLayout* l1 = make_layout(cr, b1, "Monospace Bold 11", s);
    PangoLayout* l2 = make_layout(cr, b2, "Monospace 10", s);
    PangoLayout* l3 = make_layout(cr, b3, "Monospace 10", s);
    TextMetrics m1 = measure_layout(l1);
    TextMetrics m2 = measure_layout(l2);
    TextMetrics m3 = measure_layout(l3);

    double pad_x = 10.0 * s;
    double pad_y = 8.0 * s;
    double box_w = std::max({m1.width, m2.width, m3.width}) + 2.0 * pad_x;
    double box_h = m1.height + m2.height + m3.height + 6.0 * s + 2.0 * pad_y;

    double bx = (cx + 16.0 * s + box_w <= W - 8.0 * s) ? (cx + 16.0 * s) : (cx - 16.0 * s - box_w);
    double by = std::clamp(cy - box_h * 0.5, 16.0 * s, ruler_top - box_h - 12.0 * s);

    cairo_save(cr);
    draw_rounded_rect(cr, bx, by, box_w, box_h, 6.0 * s);
    cairo_set_source_rgba(cr, 0.03, 0.04, 0.06, 0.92);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, std::max(1.0, 1.3 * s));
    cairo_set_source_rgba(cr, 1.0, 0.85, 0.25, 0.90);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 1.0, 0.92, 0.45, 1.0);
    cairo_move_to(cr, bx + pad_x, by + pad_y);
    pango_cairo_show_layout(cr, l1);

    cairo_set_source_rgba(cr, 0.88, 0.91, 0.96, 0.96);
    cairo_move_to(cr, bx + pad_x, by + pad_y + m1.height + 3.0 * s);
    pango_cairo_show_layout(cr, l2);

    cairo_move_to(cr, bx + pad_x, by + pad_y + m1.height + m2.height + 6.0 * s);
    pango_cairo_show_layout(cr, l3);
    cairo_restore(cr);

    g_object_unref(l1);
    g_object_unref(l2);
    g_object_unref(l3);
}

void draw_pill_label(cairo_t* cr, double x, double y, double scale,
                     const std::string& text, const char* font_desc,
                     double tr, double tg, double tb,
                     double br, double bg, double bb,
                     bool right_align = false) {
    PangoLayout* layout = make_layout(cr, text, font_desc, scale);
    TextMetrics tm = measure_layout(layout);
    double pad_x = 9.0 * scale;
    double pad_y = 4.0 * scale;
    double pw = tm.width + 2.0 * pad_x;
    double ph = tm.height + 2.0 * pad_y;
    double px = right_align ? (x - pw) : x;

    cairo_save(cr);
    draw_rounded_rect(cr, px, y, pw, ph, 5.0 * scale);
    cairo_set_source_rgba(cr, 0.03, 0.04, 0.05, 0.88);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, std::max(1.0, 1.3 * scale));
    cairo_set_source_rgba(cr, br, bg, bb, 0.85);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
    cairo_move_to(cr, px + pad_x, y + pad_y);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    g_object_unref(layout);
}

void draw_test_pattern_overlay(cairo_t* cr, const OverlayState& state, double tl_right) {
    const double W = state.width_px;
    const double H = state.height_px;
    const double s = state.ui_scale;
    const bool hdr_active = state.window_hdr_enabled &&
                            state.hdr_output_mode != HdrOutputMode::Sdr &&
                            state.hdr_output_mode != HdrOutputMode::Sdr10;
    const double sdr_full = estimated_sdr_full_nits(
        state.edid.avg_nits, state.edid.has_avg, state.edid.max_nits, state.edid.has_max);
    const double peak = state.mode_limits.has_active_limit
        ? state.mode_limits.active_max_nits
        : (hdr_active && state.edid.has_max ? state.edid.max_nits : sdr_full);

    // Top badge sits cleanly between Top-Left card and Top-Right menu
    double badge_right = W - 18.0 * s;
    if (state.menu_visible) {
        double mx = 0.0, my = 0.0, mw = 0.0, mh = 0.0, mhh = 0.0, mrh = 0.0;
        compute_hud_menu_rect(state, mx, my, mw, mh, mhh, mrh);
        badge_right = mx - 14.0 * s;
    }
    if (badge_right > tl_right + 120.0 * s) {
        draw_pill_label(cr, badge_right, 16.0 * s, s,
                        std::string("MIRE ") + test_pattern_name(state.test_pattern),
                        "Sans Bold 10.5",
                        0.96, 0.98, 1.0, 0.35, 0.82, 1.0, true);
    }

    if (state.test_pattern == TestPattern::GamutBars) {
        draw_pill_label(cr, W * 0.965, H * 0.075, s, "Rec.709 / sRGB", "Sans Bold 10",
                        0.95, 0.96, 0.98, 0.70, 0.75, 0.85, true);
        draw_pill_label(cr, W * 0.965, H * 0.175, s, "DCI-P3 D65", "Sans Bold 10",
                        1.0, 0.86, 0.35, 0.98, 0.78, 0.25, true);
        draw_pill_label(cr, W * 0.965, H * 0.275, s, "BT.2020", "Sans Bold 10",
                        0.40, 0.94, 1.0, 0.30, 0.88, 0.96, true);
        draw_pill_label(cr, W * 0.965, H * 0.398, s, "ColorChecker 24 (sRGB référence)", "Sans Bold 10",
                        0.92, 0.94, 0.97, 0.50, 0.56, 0.65, true);
        draw_pill_label(cr, W * 0.965, H * 0.748, s, "Échelle gris 0 → ~" + format_nits(peak, true), "Sans Bold 10",
                        0.92, 0.94, 0.97, 0.50, 0.56, 0.65, true);
    } else if (state.test_pattern == TestPattern::Banding10v8) {
        draw_pill_label(cr, W * 0.965, H * 0.095, s, "Gris sombre : Haut = 10/16-bit natif", "Sans Bold 10",
                        0.35, 0.95, 0.65, 0.32, 0.92, 0.60, true);
        draw_pill_label(cr, W * 0.965, H * 0.230, s, "Gris sombre : Bas = 8-bit quantifié (paliers)", "Sans Bold 10",
                        1.0, 0.62, 0.35, 0.95, 0.50, 0.30, true);
        draw_pill_label(cr, W * 0.965, H * 0.375, s, "Ciel bleu : Haut = 10/16-bit natif", "Sans Bold 10",
                        0.35, 0.95, 0.65, 0.32, 0.92, 0.60, true);
        draw_pill_label(cr, W * 0.965, H * 0.510, s, "Ciel bleu : Bas = 8-bit quantifié", "Sans Bold 10",
                        1.0, 0.62, 0.35, 0.95, 0.50, 0.30, true);
        draw_pill_label(cr, W * 0.965, H * 0.655, s, "Or / Chair : Haut = 10/16-bit natif", "Sans Bold 10",
                        0.35, 0.95, 0.65, 0.32, 0.92, 0.60, true);
        draw_pill_label(cr, W * 0.965, H * 0.790, s, "Or / Chair : Bas = 8-bit quantifié", "Sans Bold 10",
                        1.0, 0.62, 0.35, 0.95, 0.50, 0.30, true);
    } else if (state.test_pattern == TestPattern::ClippingBlackWhite) {
        draw_pill_label(cr, W * 0.52, H * 0.065, s,
                        (!hdr_active)
                            ? ("HAUTES LUMIÈRES SDR (88% → 100% pleine dalle ~" + format_nits(peak, true) + ")")
                            : ("HAUTES LUMIÈRES HDR (0.50× → 1.65× pic " + format_nits(peak, true) + ")"),
                        "Sans Bold 10.5", 1.0, 0.88, 0.40, 0.95, 0.75, 0.25, false);
        draw_pill_label(cr, W * 0.03, H * 0.89, s,
                        (!hdr_active) ? "NOIRS SDR : 0.2% → 10% sur noir pur"
                                      : "NOIRS HDR : 0.005 → 8.0 nits sur noir pur",
                        "Sans Bold 10", 0.45, 0.92, 1.0, 0.35, 0.85, 0.98, false);

        static constexpr double kHdrBlackVals[12] = {0.005, 0.01, 0.02, 0.05, 0.10, 0.20, 0.40, 0.80, 1.50, 3.00, 5.00, 8.00};
        static constexpr double kSdrBlackPct[12] = {0.2, 0.4, 0.7, 1.0, 1.5, 2.0, 3.0, 4.0, 5.5, 7.0, 8.5, 10.0};
        static constexpr double kSdrHiPct[12] = {88.0, 90.0, 92.0, 94.0, 95.0, 96.0, 97.0, 98.0, 98.8, 99.2, 99.6, 100.0};
        static constexpr double kHdrHiMult[12] = {0.50, 0.65, 0.75, 0.85, 0.92, 0.96, 1.00, 1.05, 1.12, 1.25, 1.40, 1.65};

        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 4; ++col) {
                int idx = row * 4 + col;
                double cy = H * (0.10 + (row + 0.84) * (0.78 / 3.0));
                double lx = W * (0.03 + (col + 0.5) * (0.45 / 4.0));
                double rx = W * (0.52 + (col + 0.5) * (0.45 / 4.0));
                char lbuf[32], rbuf[32];
                if (!hdr_active) {
                    std::snprintf(lbuf, sizeof(lbuf), "%.1f%%", kSdrBlackPct[idx]);
                    std::snprintf(rbuf, sizeof(rbuf), "%.1f%%", kSdrHiPct[idx]);
                } else {
                    std::snprintf(lbuf, sizeof(lbuf), "%s n", format_nits(kHdrBlackVals[idx]).c_str());
                    std::snprintf(rbuf, sizeof(rbuf), "%s n", format_nits(peak * kHdrHiMult[idx]).c_str());
                }
                draw_text(cr, lx - 22.0 * s, cy - 16.0 * s, lbuf, "Monospace Bold 9.5", s, 0.75, 0.82, 0.92, 0.95);
                draw_text(cr, rx - 24.0 * s, cy - 16.0 * s, rbuf, "Monospace Bold 9.5", s, 0.12, 0.14, 0.18, 0.95);
            }
        }
    } else if (state.test_pattern == TestPattern::HdrScene) {
        draw_pill_label(cr, W * 0.97, H * 0.89, s,
                        "Soleil spéculaire (~" + format_nits(peak, true) + ") • Silhouettes sombres • Reflets BT.2020",
                        "Sans Bold 10", 0.95, 0.96, 0.98, 0.45, 0.65, 0.95, true);
    } else if (state.test_pattern == TestPattern::SharpnessAnsi) {
        draw_pill_label(cr, W * 0.965, H * 0.08, s, "Grille 1px / 2px & Lignes 4:4:4 (R/C, V/M)", "Sans Bold 10",
                        0.95, 0.96, 0.98, 0.45, 0.85, 0.98, true);
        draw_pill_label(cr, W * 0.965, H * 0.65, s, "Uniformité : 5% • 25% • 50% • Blanc • Pic • Gris", "Sans Bold 10",
                        0.95, 0.96, 0.98, 0.55, 0.62, 0.72, true);
    } else if (state.test_pattern == TestPattern::HueSpectrum) {
        draw_pill_label(cr, W * 0.965, H * 0.075, s, "Teinte × Saturation / Luminance (sRGB)", "Sans Bold 10",
                        0.95, 0.96, 0.98, 0.70, 0.75, 0.85, true);
        draw_pill_label(cr, W * 0.965, H * 0.515, s, "Expansion Gamut : Rec.709 (haut) → DCI-P3 (milieu) → BT.2020 (bas)",
                        "Sans Bold 10", 0.40, 0.94, 1.0, 0.30, 0.88, 0.96, true);
    }
}

GamutTriangle edid_gamut_triangle(const EdidHdrSpecs& edid) {
    if (edid.has_chromaticity) {
        return {
            {edid.red_x, edid.red_y},
            {edid.green_x, edid.green_y},
            {edid.blue_x, edid.blue_y},
            {edid.white_x, edid.white_y},
        };
    }
    return kGamutSrgb;
}

void draw_cie_triangle_overlay(cairo_t* cr, const OverlayState& state) {
    const double W = state.width_px;
    const double H = state.height_px;
    const double s = state.ui_scale;

    double plot_x = 0.0, plot_y = 0.0, plot_size = 0.0;
    compute_cie_plot_rect(state, plot_x, plot_y, plot_size);
    if (plot_size < 60.0) return;

    auto to_px = [&](double xc, double yc) -> std::pair<double, double> {
        double px = plot_x + ((xc - kCiePlotMin) / kCiePlotSpan) * plot_size;
        double py = plot_y + ((kCiePlotMax - yc) / kCiePlotSpan) * plot_size;
        return {px, py};
    };

    // Plot frame
    cairo_save(cr);
    cairo_rectangle(cr, plot_x, plot_y, plot_size, plot_size);
    cairo_set_line_width(cr, std::max(1.2, 1.5 * s));
    cairo_set_source_rgba(cr, 0.45, 0.52, 0.65, 0.85);
    cairo_stroke(cr);

    // Axis ticks (0.0 .. 0.8)
    for (int i = 0; i <= 8; ++i) {
        double v = i * 0.1;
        auto [tx, ty_b] = to_px(v, kCiePlotMin);
        auto [lx_l, ty] = to_px(kCiePlotMin, v);
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "%.1f", v);
        cairo_set_source_rgba(cr, 0.60, 0.66, 0.76, 0.85);
        cairo_move_to(cr, tx, plot_y + plot_size);
        cairo_line_to(cr, tx, plot_y + plot_size + 5.0 * s);
        cairo_move_to(cr, plot_x - 5.0 * s, ty);
        cairo_line_to(cr, plot_x, ty);
        cairo_stroke(cr);
        draw_text(cr, tx - 10.0 * s, plot_y + plot_size + 6.0 * s, lbl, "Monospace 9", s, 0.72, 0.78, 0.88, 0.9);
        draw_text(cr, plot_x - 28.0 * s, ty - 7.0 * s, lbl, "Monospace 9", s, 0.72, 0.78, 0.88, 0.9);
    }
    draw_text(cr, plot_x + plot_size * 0.5 - 18.0 * s, plot_y + plot_size + 22.0 * s,
              "CIE x", "Sans Bold 10", s, 0.82, 0.86, 0.94, 0.95);
    draw_text(cr, plot_x - 42.0 * s, plot_y + 4.0 * s,
              "CIE y", "Sans Bold 10", s, 0.82, 0.86, 0.94, 0.95);

    // Clip to plot box for locus & triangles
    cairo_rectangle(cr, plot_x, plot_y, plot_size, plot_size);
    cairo_clip(cr);

    // Closed CIE 1931 spectral locus outline + purple line
    cairo_new_path(cr);
    for (size_t i = 0; i < kCie1931Locus.size(); ++i) {
        auto [px, py] = to_px(kCie1931Locus[i].x, kCie1931Locus[i].y);
        if (i == 0) cairo_move_to(cr, px, py);
        else cairo_line_to(cr, px, py);
    }
    cairo_close_path(cr);
    cairo_set_line_width(cr, std::max(1.5, 2.0 * s));
    cairo_set_source_rgba(cr, 0.90, 0.93, 0.98, 0.92);
    cairo_stroke(cr);

    // Wavelength tick labels around locus
    static constexpr int kWaveLabels[] = {460, 480, 500, 520, 540, 560, 580, 600, 620, 650};
    for (int wl : kWaveLabels) {
        for (size_t i = 0; i < kCie1931Locus.size(); ++i) {
            if (kCie1931Locus[i].wavelength_nm != wl) continue;
            auto [px, py] = to_px(kCie1931Locus[i].x, kCie1931Locus[i].y);
            double dx = (kCie1931Locus[i].x - 0.3127);
            double dy = -(kCie1931Locus[i].y - 0.3290);  // screen y inverted
            double len = std::max(1e-6, std::hypot(dx, dy));
            double ox = (dx / len) * 14.0 * s;
            double oy = (dy / len) * 14.0 * s;
            cairo_set_line_width(cr, std::max(1.0, 1.3 * s));
            cairo_set_source_rgba(cr, 0.92, 0.95, 1.0, 0.85);
            cairo_arc(cr, px, py, 2.4 * s, 0.0, 2.0 * M_PI);
            cairo_fill(cr);
            char wbuf[16];
            std::snprintf(wbuf, sizeof(wbuf), "%d", wl);
            draw_text(cr, px + ox - 11.0 * s, py + oy - 6.0 * s, wbuf, "Monospace 8.5", s, 0.86, 0.90, 0.96, 0.9);
            break;
        }
    }

    int active_id = resolve_active_gamut_id(state);
    GamutTriangle edid_tri = edid_gamut_triangle(state.edid);

    auto draw_tri = [&](const GamutTriangle& tri, double r, double g, double b, double width_px, bool dashed, bool highlight) {
        auto [rx, ry] = to_px(tri.r.x, tri.r.y);
        auto [gx, gy] = to_px(tri.g.x, tri.g.y);
        auto [bx, by] = to_px(tri.b.x, tri.b.y);
        cairo_save(cr);
        if (highlight) {
            cairo_set_line_width(cr, width_px + 3.0 * s);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.72);
            cairo_move_to(cr, rx, ry);
            cairo_line_to(cr, gx, gy);
            cairo_line_to(cr, bx, by);
            cairo_close_path(cr);
            cairo_stroke(cr);
        }
        if (dashed) {
            double d[2] = {7.0 * s, 5.0 * s};
            cairo_set_dash(cr, d, 2, 0.0);
        }
        cairo_set_line_width(cr, width_px);
        cairo_set_source_rgba(cr, r, g, b, highlight ? 1.0 : 0.88);
        cairo_move_to(cr, rx, ry);
        cairo_line_to(cr, gx, gy);
        cairo_line_to(cr, bx, by);
        cairo_close_path(cr);
        cairo_stroke(cr);

        double rad = (highlight ? 4.5 : 3.2) * s;
        for (auto pt : {std::pair<double, double>{rx, ry}, {gx, gy}, {bx, by}}) {
            cairo_arc(cr, pt.first, pt.second, rad, 0.0, 2.0 * M_PI);
            cairo_fill(cr);
        }
        cairo_restore(cr);
    };

    // Draw reference triangles & highlight active spectrum limit
    draw_tri(kGamutBt2020, 0.30, 0.90, 1.00, active_id == 3 ? 3.2 * s : 2.0 * s, active_id != 3, active_id == 3);
    draw_tri(kGamutDciP3,  1.00, 0.80, 0.25, active_id == 1 ? 3.2 * s : 2.0 * s, active_id != 1, active_id == 1);
    if (state.edid.has_chromaticity) {
        draw_tri(edid_tri, 0.32, 0.96, 0.58, active_id == 2 ? 3.2 * s : 2.2 * s, false, active_id == 2);
    }
    draw_tri(kGamutSrgb,   0.95, 0.96, 1.00, active_id == 0 ? 3.2 * s : 1.8 * s, active_id != 0, active_id == 0);

    // D65 White point cross
    {
        auto [wx, wy] = to_px(0.3127, 0.3290);
        cairo_set_line_width(cr, std::max(1.5, 1.8 * s));
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96);
        cairo_move_to(cr, wx - 6.0 * s, wy);
        cairo_line_to(cr, wx + 6.0 * s, wy);
        cairo_move_to(cr, wx, wy - 6.0 * s);
        cairo_line_to(cr, wx, wy + 6.0 * s);
        cairo_stroke(cr);
        draw_text(cr, wx + 7.0 * s, wy - 8.0 * s, "D65", "Sans Bold 9", s, 1.0, 1.0, 1.0, 0.95);
    }
    if (state.edid.has_chromaticity &&
        (std::abs(state.edid.white_x - 0.3127) > 0.002 || std::abs(state.edid.white_y - 0.3290) > 0.002)) {
        auto [ewx, ewy] = to_px(state.edid.white_x, state.edid.white_y);
        cairo_set_source_rgba(cr, 0.32, 0.96, 0.58, 0.95);
        cairo_arc(cr, ewx, ewy, 3.5 * s, 0.0, 2.0 * M_PI);
        cairo_stroke(cr);
    }
    cairo_restore(cr);

    // Legend box in top-right of plot
    {
        struct LegItem { std::string label; double r, g, b; bool active; };
        std::vector<LegItem> items = {
            {"BT.2020 (Conteneur HDR)", 0.30, 0.90, 1.00, active_id == 3},
            {"DCI-P3 D65 (Wide SDR)", 1.00, 0.80, 0.25, active_id == 1},
            {"sRGB / Rec.709", 0.95, 0.96, 1.00, active_id == 0},
        };
        if (state.edid.has_chromaticity) {
            items.insert(items.begin() + 2, {"Dalle EDID (Limite physique)", 0.32, 0.96, 0.58, active_id == 2});
        }
        double lx = plot_x + plot_size - 235.0 * s;
        double ly = plot_y + 10.0 * s;
        double lw = 225.0 * s;
        double lh = (items.size() * 20.0 + 16.0) * s;
        cairo_save(cr);
        draw_rounded_rect(cr, lx, ly, lw, lh, 6.0 * s);
        cairo_set_source_rgba(cr, 0.03, 0.04, 0.06, 0.90);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, std::max(1.0, 1.2 * s));
        cairo_set_source_rgba(cr, 0.40, 0.48, 0.60, 0.75);
        cairo_stroke(cr);
        for (size_t i = 0; i < items.size(); ++i) {
            double ry = ly + (10.0 + i * 20.0) * s;
            cairo_set_line_width(cr, items[i].active ? 3.0 * s : 2.0 * s);
            cairo_set_source_rgba(cr, items[i].r, items[i].g, items[i].b, 1.0);
            cairo_move_to(cr, lx + 10.0 * s, ry + 7.0 * s);
            cairo_line_to(cr, lx + 28.0 * s, ry + 7.0 * s);
            cairo_stroke(cr);
            std::string txt = items[i].label + (items[i].active ? " ★" : "");
            draw_text(cr, lx + 34.0 * s, ry, txt, items[i].active ? "Sans Bold 9" : "Sans 9", s,
                      items[i].r, items[i].g, items[i].b, 0.98);
        }
        cairo_restore(cr);
    }

    // Interactive CIE mouse inspector inside the plot (skipped when hovering menu)
    bool over_menu = false;
    if (state.show_hud && state.menu_visible) {
        double mx = 0.0, my = 0.0, mw = 0.0, mh = 0.0, mhh = 0.0, mrh = 0.0;
        compute_hud_menu_rect(state, mx, my, mw, mh, mhh, mrh);
        over_menu = (state.cursor_x_px >= mx && state.cursor_x_px <= mx + mw &&
                     state.cursor_y_px >= my && state.cursor_y_px <= my + mh);
    }
    if (state.cursor_active && !over_menu &&
        state.cursor_x_px >= plot_x && state.cursor_x_px <= plot_x + plot_size &&
        state.cursor_y_px >= plot_y && state.cursor_y_px <= plot_y + plot_size) {
        double rx = (state.cursor_x_px - plot_x) / plot_size;
        double ry = (state.cursor_y_px - plot_y) / plot_size;
        CiePoint cp{kCiePlotMin + rx * kCiePlotSpan, kCiePlotMax - ry * kCiePlotSpan};
        CiePoint uvp = cie_xy_to_uv_prime(cp);
        bool in_locus = point_in_cie_locus(cp);
        bool in_srgb = point_in_triangle(cp, kGamutSrgb);
        bool in_p3   = point_in_triangle(cp, kGamutDciP3);
        bool in_2020 = point_in_triangle(cp, kGamutBt2020);
        bool in_edid = point_in_triangle(cp, edid_tri);

        cairo_save(cr);
        cairo_set_line_width(cr, std::max(1.0, 1.1 * s));
        cairo_set_source_rgba(cr, 1.0, 0.88, 0.25, 0.78);
        cairo_move_to(cr, state.cursor_x_px, plot_y);
        cairo_line_to(cr, state.cursor_x_px, plot_y + plot_size);
        cairo_move_to(cr, plot_x, state.cursor_y_px);
        cairo_line_to(cr, plot_x + plot_size, state.cursor_y_px);
        cairo_stroke(cr);
        cairo_restore(cr);

        char t1[96], t2[96], t3[128];
        std::snprintf(t1, sizeof(t1), "CIE 1931 : (%.4f, %.4f)", cp.x, cp.y);
        std::snprintf(t2, sizeof(t2), "CIE 1976 : (u'=%.4f, v'=%.4f)", uvp.x, uvp.y);
        std::string mem = in_locus
            ? (std::string(in_srgb ? "sRGB ✓" : "sRGB ✗") + " | " +
               (in_p3 ? "DCI-P3 ✓" : "DCI-P3 ✗") + " | " +
               (in_edid ? "EDID ✓" : "EDID ✗") + " | " +
               (in_2020 ? "BT.2020 ✓" : "BT.2020 ✗"))
            : "Hors spectre visible humain";
        std::snprintf(t3, sizeof(t3), "%s", mem.c_str());

        double bx = (state.cursor_x_px + 250.0 * s < W - 12.0 * s) ? (state.cursor_x_px + 14.0 * s) : (state.cursor_x_px - 260.0 * s);
        double by = std::clamp(state.cursor_y_px - 36.0 * s, 16.0 * s, H - 96.0 * s);
        draw_pill_label(cr, bx, by, s, std::string(t1) + "\n" + t2 + "\n" + t3,
                        "Monospace 9.5", 1.0, 0.94, 0.55, 1.0, 0.85, 0.25, false);
    }
}

const char* short_view_name(ViewMode v) {
    switch (v) {
        case ViewMode::Split: return "Mixte";
        case ViewMode::Smooth: return "Continu";
        case ViewMode::Bands: return "Bandes";
        case ViewMode::Rgbw: return "R/V/B/Blanc";
        case ViewMode::TestPattern: return "Mires test";
        case ViewMode::CieTriangle: return "Triangle CIE";
    }
    return "";
}

const char* short_pattern_name(TestPattern p) {
    switch (p) {
        case TestPattern::GamutBars: return "1/6 Gamuts";
        case TestPattern::Banding10v8: return "2/6 Banding 10v8";
        case TestPattern::ClippingBlackWhite: return "3/6 Clipping";
        case TestPattern::HdrScene: return "4/6 Scène HDR";
        case TestPattern::SharpnessAnsi: return "5/6 Netteté";
        case TestPattern::HueSpectrum: return "6/6 Spectre";
    }
    return "";
}

const char* short_output_name(HdrOutputMode m) {
    switch (m) {
        case HdrOutputMode::Hdr10Pq: return "HDR10 PQ";
        case HdrOutputMode::HdrExtendedLinear: return "HDR scRGB 16F";
        case HdrOutputMode::Sdr10: return "SDR 10-bit";
        case HdrOutputMode::Sdr: return "SDR 8-bit";
    }
    return "";
}

const char* short_scale_name(ScaleMode s) {
    switch (s) {
        case ScaleMode::PQ: return "PQ ST.2084";
        case ScaleMode::Log: return "Log10";
        case ScaleMode::Linear: return "Linéaire";
    }
    return "";
}

const char* short_zoom_name(int idx) {
    switch (((idx % 6) + 6) % 6) {
        case 0: return "Auto";
        case 1: return "Pic EDID";
        case 2: return "Noirs 0–5";
        case 3: return "EDID exact";
        case 4: return "0–4000 nits";
        case 5: return "0–10000 nits";
    }
    return "Auto";
}

std::string active_preset_label(const OverlayState& state) {
    if (!state.mode_limits.active_icc_path.empty()) {
        if (state.mode_limits.active_icc_path.find("EDID-Native") != std::string::npos) return "ICC EDID 10b";
        if (state.mode_limits.active_icc_path.find("DCI-P3") != std::string::npos) return "ICC P3 10b";
        return "ICC sRGB";
    }
    int disp_bpc = parse_format_bpc(state.mode_limits.hypr_format, state.window_hdr_enabled ? 10 : 8);
    if (state.mode_limits.hypr_cm_preset == "hdr") return "hdr (EDID)";
    if (state.mode_limits.hypr_cm_preset == "hdredid") return "hdredid";
    if (state.mode_limits.hypr_cm_preset == "dcip3" || state.mode_limits.hypr_cm_preset == "wide") return "dcip3 (10b)";
    if (state.mode_limits.hypr_cm_preset == "srgb") return (disp_bpc >= 10) ? "srgb10 (10b)" : "srgb (8b)";
    return state.mode_limits.hypr_cm_preset.empty() ? "Auto" : state.mode_limits.hypr_cm_preset;
}

void draw_hud_menu(cairo_t* cr, const OverlayState& state) {
    if (!state.menu_visible) return;
    const double s = state.ui_scale;
    double mx = 0.0, my = 0.0, mw = 0.0, mh = 0.0, header_h = 0.0, row_h = 0.0;
    compute_hud_menu_rect(state, mx, my, mw, mh, header_h, row_h);

    const bool hdr_active = state.window_hdr_enabled &&
                            state.hdr_output_mode != HdrOutputMode::Sdr &&
                            state.hdr_output_mode != HdrOutputMode::Sdr10;

    struct MenuRow {
        std::string label;
        std::string value;
        bool is_action = false;
        bool is_disabled = false;
        double vr = 0.45, vg = 0.90, vb = 1.0;
    };

    std::string lum_val;
    if (hdr_active) {
        lum_val = "(SDR seul)";
    } else if (state.mode_limits.backlight_available) {
        lum_val = std::to_string(state.mode_limits.backlight_percent) + "%";
    } else {
        lum_val = "N/D";
    }

    std::vector<MenuRow> rows = {
        {"Vue", short_view_name(state.view_mode), false, false, 0.45, 0.90, 1.0},
        {"Mire test", short_pattern_name(state.test_pattern), false, false, 0.45, 0.90, 1.0},
        {"Sortie GPU", short_output_name(state.hdr_output_mode), false, false, 0.35, 0.95, 0.65},
        {"Preset Hyprland", active_preset_label(state), false, false, 0.98, 0.82, 0.35},
        {"Luminosité SDR", lum_val, false, hdr_active || !state.mode_limits.backlight_available,
         hdr_active ? 0.55 : 0.98, hdr_active ? 0.58 : 0.86, hdr_active ? 0.65 : 0.42},
        {"Échelle", short_scale_name(state.scale_mode), false, false, 0.85, 0.90, 0.98},
        {"Canal", channel_mode_name(state.channel_mode), false, false, 0.85, 0.90, 0.98},
        {"Paliers (Bandes)", std::to_string(state.band_count), false, false, 0.85, 0.90, 0.98},
        {"Zoom plage", short_zoom_name(state.zoom_preset), false, false, 0.85, 0.90, 0.98},
        {"Plage Max", format_nits(state.max_nits, true), false, false, 0.85, 0.90, 0.98},
        {"Plage Min", format_nits(state.min_nits, true), false, false, 0.85, 0.90, 0.98},
        {"Repères EDID", state.show_edid_markers ? "Affichés" : "Masqués", false, false, 0.35, 0.95, 0.65},
        {"Plein écran", state.fullscreen ? "Oui" : "Non", false, false, 0.85, 0.90, 0.98},
        {"Exporter → Monique & Ouvrir", "Entrée ⏎", true, false, 0.35, 0.95, 0.65},
        {"Masquer HUD [H]", "Entrée ⏎", true, false, 0.78, 0.84, 0.94},
        {"Quitter", "Entrée ⏎", true, false, 1.0, 0.48, 0.48},
    };

    cairo_save(cr);
    draw_rounded_rect(cr, mx, my, mw, mh, 8.0 * s);
    cairo_set_source_rgba(cr, 0.03, 0.04, 0.06, 0.92);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, std::max(1.0, 1.3 * s));
    cairo_set_source_rgba(cr, 0.38, 0.56, 0.78, 0.80);
    cairo_stroke(cr);

    // Header
    draw_text(cr, mx + 12.0 * s, my + 7.0 * s, "MENU CONTRÔLE  (↑/↓/←/→)", "Sans Bold 10.5", s,
              0.92, 0.96, 1.0, 1.0);
    cairo_set_line_width(cr, std::max(1.0, 1.0 * s));
    cairo_set_source_rgba(cr, 0.35, 0.45, 0.60, 0.55);
    cairo_move_to(cr, mx + 8.0 * s, my + header_h);
    cairo_line_to(cr, mx + mw - 8.0 * s, my + header_h);
    cairo_stroke(cr);

    int sel = std::clamp(state.menu_selected, 0, static_cast<int>(rows.size()) - 1);
    double rows_top = my + header_h + 4.0 * s;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        double ry = rows_top + i * row_h;
        bool is_sel = (i == sel);

        if (i == static_cast<int>(HudMenuItem::ExportMonique)) {
            cairo_set_source_rgba(cr, 0.35, 0.45, 0.60, 0.45);
            cairo_move_to(cr, mx + 10.0 * s, ry - 1.0 * s);
            cairo_line_to(cr, mx + mw - 10.0 * s, ry - 1.0 * s);
            cairo_stroke(cr);
        }

        if (is_sel) {
            draw_rounded_rect(cr, mx + 6.0 * s, ry + 1.0 * s, mw - 12.0 * s, row_h - 2.0 * s, 5.0 * s);
            cairo_set_source_rgba(cr, rows[i].is_action ? 0.16 : 0.14, rows[i].is_action ? 0.34 : 0.28, 0.44, 0.90);
            cairo_fill_preserve(cr);
            cairo_set_line_width(cr, std::max(1.0, 1.2 * s));
            cairo_set_source_rgba(cr, 0.40, 0.85, 1.0, 0.95);
            cairo_stroke(cr);
        }

        double lr = rows[i].is_disabled ? 0.52 : (is_sel ? 1.0 : 0.84);
        double lg = rows[i].is_disabled ? 0.56 : (is_sel ? 1.0 : 0.88);
        double lb = rows[i].is_disabled ? 0.62 : (is_sel ? 1.0 : 0.94);
        draw_text(cr, mx + 14.0 * s, ry + 4.0 * s, rows[i].label,
                  (is_sel || rows[i].is_action) ? "Sans Bold 9.5" : "Sans 9.5", s, lr, lg, lb, 1.0);

        std::string vtxt = rows[i].is_action ? rows[i].value : ("‹ " + rows[i].value + " ›");
        PangoLayout* vl = make_layout(cr, vtxt, is_sel ? "Sans Bold 9.5" : "Sans 9.5", s);
        TextMetrics vm = measure_layout(vl);
        cairo_set_source_rgba(cr, rows[i].vr, rows[i].vg, rows[i].vb, rows[i].is_disabled ? 0.65 : 1.0);
        cairo_move_to(cr, mx + mw - 14.0 * s - vm.width, ry + 4.0 * s);
        pango_cairo_show_layout(cr, vl);
        g_object_unref(vl);
    }

    double fy = rows_top + static_cast<double>(rows.size()) * row_h + 4.0 * s;
    draw_text(cr, mx + 12.0 * s, fy, "↑↓ Choisir  •  ←→ Modifier  •  Entrée/Clic Valider",
              "Sans 8.5", s, 0.62, 0.70, 0.82, 0.92);
    cairo_restore(cr);
}

}  // namespace

void compute_hud_menu_rect(const OverlayState& state,
                           double& out_x,
                           double& out_y,
                           double& out_w,
                           double& out_h,
                           double& out_header_h,
                           double& out_row_h) {
    const double W = std::max(1, state.width_px);
    const double s = state.ui_scale;
    out_w = std::clamp(340.0 * s, 250.0, W * 0.34);
    out_header_h = 30.0 * s;
    out_row_h = 24.0 * s;
    double footer_h = 22.0 * s;
    out_h = out_header_h + kHudMenuItemCount * out_row_h + footer_h + 10.0 * s;
    out_x = W - out_w - 18.0 * s;
    out_y = 16.0 * s;
}

void compute_cie_plot_rect(const OverlayState& state, double& out_x, double& out_y, double& out_size) {
    const double W = std::max(1, state.width_px);
    const double H = std::max(1, state.height_px);
    const double s = state.ui_scale;
    double left_pad = state.show_hud ? std::min(W * 0.42, 660.0 * s) : (48.0 * s);
    double right_pad = 24.0 * s;
    if (state.show_hud && state.menu_visible) {
        double mx = 0.0, my = 0.0, mw = 0.0, mh = 0.0, mhh = 0.0, mrh = 0.0;
        compute_hud_menu_rect(state, mx, my, mw, mh, mhh, mrh);
        right_pad = mw + 34.0 * s;
    }
    double top_pad = 20.0 * s;
    double bottom_pad = state.show_hud ? (58.0 * s) : (32.0 * s);
    double avail_w = std::max(80.0, W - left_pad - right_pad);
    double avail_h = std::max(80.0, H - top_pad - bottom_pad);
    out_size = std::min(avail_w, avail_h);
    out_x = left_pad + (avail_w - out_size) * 0.5;
    out_y = top_pad + (avail_h - out_size) * 0.5;
}

int resolve_active_gamut_id(const OverlayState& state) {
    bool hdr_active = state.window_hdr_enabled &&
                      state.hdr_output_mode != HdrOutputMode::Sdr &&
                      state.hdr_output_mode != HdrOutputMode::Sdr10;
    if (hdr_active && state.hdr_output_mode == HdrOutputMode::Hdr10Pq) {
        return 3;  // BT.2020 container
    }
    if (state.mode_limits.wl_primaries == 6) {
        return 3;  // BT.2020
    }
    if (!state.mode_limits.active_icc_path.empty() &&
        state.mode_limits.active_icc_path.find("EDID-Native") != std::string::npos) {
        return 2;  // EDID-Native ICC
    }
    if (state.mode_limits.wl_primaries == 8 || state.mode_limits.wl_primaries == 9 ||
        state.mode_limits.hypr_cm_preset == "dcip3" ||
        state.mode_limits.hypr_cm_preset == "dp3" ||
        state.mode_limits.hypr_cm_preset == "wide" ||
        state.mode_limits.hypr_cm_preset == "adobe" ||
        (hdr_active && state.hdr_output_mode == HdrOutputMode::HdrExtendedLinear)) {
        return 1;  // DCI-P3
    }
    return 0;  // sRGB
}

std::string build_live_header_title(const OverlayState& state) {
    bool hdr_active = state.window_hdr_enabled &&
                      state.hdr_output_mode != HdrOutputMode::Sdr &&
                      state.hdr_output_mode != HdrOutputMode::Sdr10;
    double sdr_full_est = estimated_sdr_full_nits(
        state.edid.avg_nits, state.edid.has_avg, state.edid.max_nits, state.edid.has_max);
    double cur_peak = state.mode_limits.has_active_limit ? state.mode_limits.active_max_nits : sdr_full_est;
    double cur_min = state.mode_limits.has_active_limit ? state.mode_limits.active_min_nits : 0.2;
    LightQualityTier light_tier = classify_light_quality(cur_peak, hdr_active);

    int fallback_bpc = (state.window_hdr_enabled ? 10 : 8);
    int display_bpc = parse_format_bpc(state.mode_limits.hypr_format, fallback_bpc);
    std::string effective_cm = state.mode_limits.hypr_cm_preset;
    if (!state.mode_limits.active_icc_path.empty() &&
        (state.mode_limits.active_icc_path.find("EDID-Native") != std::string::npos ||
         state.mode_limits.active_icc_path.find("DCI-P3") != std::string::npos)) {
        effective_cm = "dcip3";
    }
    ColorQualityInfo cqi = evaluate_color_quality(
        display_bpc, state.hdr_output_mode, hdr_active,
        state.mode_limits.wl_primaries, effective_cm, cur_min, cur_peak);

    std::string mode_label;
    if (!state.mode_limits.monique_active_profile.empty()) {
        std::string prof = state.mode_limits.monique_active_profile;
        auto close_br = prof.find(']');
        if (prof.rfind("[", 0) == 0 && close_br != std::string::npos) {
            mode_label = prof.substr(close_br + 1);
            while (!mode_label.empty() && mode_label.front() == ' ') {
                mode_label.erase(mode_label.begin());
            }
        } else {
            mode_label = prof;
        }
    } else if (!state.mode_limits.active_icc_path.empty()) {
        mode_label = std::filesystem::path(state.mode_limits.active_icc_path).filename().string();
    } else if (hdr_active) {
        mode_label = (state.mode_limits.hypr_cm_preset == "hdredid")
            ? "HDR EDID Natif"
            : "HDR Reference (EDID)";
    } else if (effective_cm == "dcip3" || effective_cm == "wide") {
        mode_label = "DCI-P3 Wide";
    } else if (cqi.effective_bpc >= 10) {
        mode_label = "sRGB 10-bit";
    } else {
        mode_label = "sRGB Standard";
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "[%d BIT %s • Couleur: %s • Lumière: %s] %s",
                  cqi.effective_bpc,
                  hdr_active ? "HDR" : "SDR",
                  light_quality_name(cqi.tier),
                  light_quality_name(light_tier),
                  mode_label.c_str());
    return std::string(buf);
}

void render_overlay(const OverlayState& state, std::vector<uint8_t>& out_bgra) {
    int w = std::max(1, state.width_px);
    int h = std::max(1, state.height_px);
    size_t stride = static_cast<size_t>(w) * 4;
    out_bgra.assign(stride * static_cast<size_t>(h), 0);

    cairo_surface_t* surface = cairo_image_surface_create_for_data(
        out_bgra.data(), CAIRO_FORMAT_ARGB32, w, h, static_cast<int>(stride));
    cairo_t* cr = cairo_create(surface);

    const bool is_gradient_view =
        (state.view_mode != ViewMode::TestPattern && state.view_mode != ViewMode::CieTriangle);

    double ruler_top = (state.view_mode == ViewMode::Split) ? (h * 0.80) : (h * 0.84);

    if (is_gradient_view) {
        if (state.show_hud) {
            draw_nit_ruler(cr, state, ruler_top);
            if (state.view_mode == ViewMode::Split) {
                draw_band_labels(cr, state, h * 0.625, ruler_top);
            } else if (state.view_mode == ViewMode::Bands) {
                draw_band_labels(cr, state, 0.0, ruler_top);
            } else if (state.view_mode == ViewMode::Rgbw) {
                draw_rgbw_labels(cr, state, ruler_top);
            }
        }

        if (state.show_edid_markers && state.edid.hdr_capable) {
            double marker_bottom = ruler_top;
            double marker_top = state.show_hud ? (h * 0.24) : (20.0 * state.ui_scale);
            if (state.edid.has_min && state.edid.min_nits >= state.min_nits && state.edid.min_nits <= state.max_nits) {
                double x = nits_to_t(state.edid.min_nits, state.min_nits, state.max_nits, state.scale_mode) * (w - 1);
                draw_edid_marker(cr, x, marker_top, marker_bottom, state.ui_scale,
                                 "EDID Min " + format_nits(state.edid.min_nits, true), 0.30, 0.88, 0.96);
            }
            if (state.edid.has_avg && state.edid.avg_nits >= state.min_nits && state.edid.avg_nits <= state.max_nits) {
                double x = nits_to_t(state.edid.avg_nits, state.min_nits, state.max_nits, state.scale_mode) * (w - 1);
                draw_edid_marker(cr, x, marker_top + 36.0 * state.ui_scale, marker_bottom, state.ui_scale,
                                 "EDID Moy " + format_nits(state.edid.avg_nits, true), 0.98, 0.78, 0.25);
            }
            if (state.edid.has_max && state.edid.max_nits >= state.min_nits && state.edid.max_nits <= state.max_nits) {
                double x = nits_to_t(state.edid.max_nits, state.min_nits, state.max_nits, state.scale_mode) * (w - 1);
                draw_edid_marker(cr, x, marker_top, marker_bottom, state.ui_scale,
                                 "EDID Max " + format_nits(state.edid.max_nits, true), 0.32, 0.95, 0.58);
            }
        }

        if (state.show_hud) {
            double cards_bottom = (state.view_mode == ViewMode::Split)
                ? (h * 0.62 - 10.0 * state.ui_scale)
                : (ruler_top - 14.0 * state.ui_scale);
            CornerCardsLayout cards = draw_corner_hud_cards(cr, state, cards_bottom);
            draw_current_mode_limit_bar(cr, state, ruler_top, cards.top_left.x + cards.top_left.w);
            draw_crosshair(cr, state, ruler_top);
            draw_hud_menu(cr, state);
            draw_bottom_legend(cr, state);
        }
    } else if (state.view_mode == ViewMode::TestPattern) {
        if (state.show_hud) {
            double cards_bottom = h - 38.0 * state.ui_scale;
            CornerCardsLayout cards = draw_corner_hud_cards(cr, state, cards_bottom);
            draw_test_pattern_overlay(cr, state, cards.top_left.x + cards.top_left.w);
            draw_hud_menu(cr, state);
            draw_bottom_legend(cr, state);
        }
    } else {
        if (state.show_hud) {
            double cards_bottom = h - 38.0 * state.ui_scale;
            draw_corner_hud_cards(cr, state, cards_bottom);
            draw_hud_menu(cr, state);
            draw_bottom_legend(cr, state);
        }
        draw_cie_triangle_overlay(cr, state);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

}  // namespace testhdr
