#pragma once

#include "edid.h"

#include <cstdint>
#include <string>

struct wl_display;
struct wl_surface;

namespace testhdr {

struct ModeLimitInfo {
    // Wayland wp_color_management_v1 (compositor current mode)
    bool wayland_cm_available = false;
    bool wayland_info_ready = false;
    double wl_min_nits = 0.0;
    double wl_max_nits = 0.0;
    double wl_ref_nits = 0.0;
    double wl_target_min_nits = 0.0;
    double wl_target_max_nits = 0.0;
    uint32_t wl_max_cll = 0;
    uint32_t wl_max_fall = 0;
    uint32_t wl_tf = 0;
    uint32_t wl_primaries = 0;

    // Hyprland IPC (hyprctl monitors -j)
    bool hypr_available = false;
    std::string hypr_monitor;
    std::string hypr_description;
    std::string hypr_cm_preset;
    std::string hypr_format;
    double hypr_sdr_min_nits = 0.0;
    double hypr_sdr_max_nits = 0.0;
    double hypr_sdr_brightness = 1.0;
    std::string active_icc_path;
    std::string monique_active_profile;

    // Hardware backlight (sysfs /sys/class/backlight)
    bool backlight_available = false;
    int backlight_percent = 100;

    // Resolved active limit for the current mode
    bool has_active_limit = false;
    double active_min_nits = 0.0;
    double active_max_nits = 0.0;
    std::string active_source;
};

// Reads current hardware backlight percentage [0..100] from /sys/class/backlight.
bool read_sysfs_backlight(int& out_percent);

// Sets hardware backlight percentage [5..100] via brightnessctl (SDR brightness control).
bool set_backlight_percent(int target_percent, int& out_actual_percent, std::string& out_status);

class DisplayLimitsMonitor {
public:
    DisplayLimitsMonitor() = default;
    ~DisplayLimitsMonitor();

    DisplayLimitsMonitor(const DisplayLimitsMonitor&) = delete;
    DisplayLimitsMonitor& operator=(const DisplayLimitsMonitor&) = delete;

    void init_wayland(struct wl_display* display, struct wl_surface* surface);
    void poll_hyprland(const std::string& preferred_connector);

    // Returns true if info changed since last call
    bool consume_dirty();

    ModeLimitInfo resolve(bool window_hdr_enabled,
                          float window_hdr_headroom,
                          float window_sdr_white_level,
                          const EdidHdrSpecs& edid) const;

    void shutdown();

    struct Impl;

private:
    Impl* impl_ = nullptr;
};

// Applies a Hyprland color_management preset ("hdr", "hdredid", "dcip3", "srgb", "auto")
// using Monique's profile & config generators, updating the live compositor.
bool apply_system_cm_monique(const std::string& cm_preset,
                             const std::string& monitor_name,
                             const EdidHdrSpecs& edid,
                             std::string& out_status,
                             const std::string& icc_path = "");

// Generates calibrated ICC profiles into ~/.local/share/icc/ and replaces all
// Monique profiles with quality-prefixed profiles matching detected modes.
bool sync_monique_profiles(const std::string& monitor_name,
                           const std::string& monitor_description,
                           const std::string& active_cm,
                           const std::string& active_icc,
                           const EdidHdrSpecs& edid,
                           std::string& out_status);

// Detects Omarchy OS from /etc/os-release content or system markers.
bool parse_os_release_is_omarchy(const std::string& os_release_text);
bool is_omarchy_system();

// Verifies that Monique (executable + Python package) is integrated on the system.
bool is_monique_integrated();

// Attempts to install Monique on Omarchy (via omarchy-pkg-aur-add / yay / paru).
// If run_in_terminal is true, launches an interactive terminal window for GUI startups.
bool install_monique_on_omarchy(bool run_in_terminal, std::string& out_status);

}  // namespace testhdr
