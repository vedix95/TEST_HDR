#pragma once

#include "display_limits.h"
#include "edid.h"
#include "nit_math.h"

#include <cstdint>
#include <vector>

namespace testhdr {

enum class HudMenuItem : int32_t {
    View = 0,
    TestPattern = 1,
    OutputMode = 2,
    HyprPreset = 3,
    SdrBrightness = 4,
    Scale = 5,
    Channel = 6,
    BandCount = 7,
    ZoomPreset = 8,
    MaxNits = 9,
    MinNits = 10,
    EdidMarkers = 11,
    Fullscreen = 12,
    ExportMonique = 13,
    HideHud = 14,
    Quit = 15,
};

inline constexpr int kHudMenuItemCount = 16;

struct OverlayState {
    int width_px = 1920;
    int height_px = 1080;
    float ui_scale = 1.0f;

    double min_nits = 0.0;
    double max_nits = 1000.0;
    ScaleMode scale_mode = ScaleMode::PQ;
    ViewMode view_mode = ViewMode::Split;
    TestPattern test_pattern = TestPattern::GamutBars;
    ChannelMode channel_mode = ChannelMode::White;
    HdrOutputMode hdr_output_mode = HdrOutputMode::Hdr10Pq;
    int band_count = 20;
    int zoom_preset = 0;
    bool fullscreen = true;

    bool show_hud = true;
    bool menu_visible = true;
    int menu_selected = 0;
    bool show_edid_markers = true;
    bool show_limit_bar = true;
    float hud_nits = 80.0f;

    bool window_hdr_enabled = false;
    float window_hdr_headroom = 0.0f;
    float window_sdr_white_level = 1.0f;
    bool supports_hdr10 = false;
    bool supports_scrgb = false;
    bool supports_sdr10 = false;

    bool cursor_active = false;
    double cursor_x_px = -1.0;
    double cursor_y_px = -1.0;

    EdidHdrSpecs edid;
    ModeLimitInfo mode_limits;
    std::string system_status_msg;
};

// Builds the live header string (e.g. "[10 BIT HDR • Couleur: ULTRA HIGH • Lumière: HIGH] HDR Reference (EDID)").
std::string build_live_header_title(const OverlayState& state);

// Computes the square pixel bounding box of the CIE 1931 diagram in CieTriangle view.
void compute_cie_plot_rect(const OverlayState& state, double& out_x, double& out_y, double& out_size);

// Computes the pixel geometry of the top-right interactive HUD menu.
void compute_hud_menu_rect(const OverlayState& state,
                           double& out_x,
                           double& out_y,
                           double& out_w,
                           double& out_h,
                           double& out_header_h,
                           double& out_row_h);

// Resolves active gamut ID: 0=sRGB, 1=DCI-P3, 2=EDID-Native, 3=BT.2020.
int resolve_active_gamut_id(const OverlayState& state);

// Renders the crisp ruler, nit ticks, current-mode limit bar, EDID markers,
// crosshair, test pattern annotations, CIE 1931 diagram vectors, and HUD into
// a premultiplied BGRA8 buffer (CAIRO_FORMAT_ARGB32).
void render_overlay(const OverlayState& state, std::vector<uint8_t>& out_bgra);

}  // namespace testhdr
