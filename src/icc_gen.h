#pragma once

#include "edid.h"

#include <string>

namespace testhdr {

struct GeneratedIccSet {
    bool ok = false;
    std::string screen_prefix;
    std::string native_icc_path;
    std::string p3_icc_path;
    std::string srgb_icc_path;
    std::string bt2020_linear_icc_path;
    std::string bt2020_pq_icc_path;
    std::string error_msg;
};

// Returns ~/.local/share/icc (using XDG_DATA_HOME if set).
std::string default_icc_directory();

// Builds a sanitized aggregated screen prefix like "Samsung_Display_Corp_0x4180-eDP-1".
std::string build_aggregated_screen_prefix(const EdidHdrSpecs& edid,
                                           const std::string& connector_override = "",
                                           const std::string& description_override = "");

// Generates 5 calibrated ICC v4 display profiles (EDID Native, DCI-P3 D65, sRGB,
// BT.2020 Linear, BT.2020 PQ) into target_dir (or default_icc_directory() if empty)
// using LittleCMS 2, prefixed with the aggregated screen name.
GeneratedIccSet generate_calibrated_icc_profiles(const EdidHdrSpecs& edid,
                                                 const std::string& connector_override = "",
                                                 const std::string& target_dir = "",
                                                 const std::string& description_override = "");

// Reads the currently configured `icc` path for `connector` from
// ~/.config/hypr/monitors.conf or ~/.config/hypr/monitors.lua (empty if none).
std::string detect_active_hypr_icc(const std::string& connector);

// Ensures calibrated ICC profiles exist in ~/.local/share/icc/, creates symlinks
// in ~/.color/icc/ -> ~/.local/share/icc/*.icc (cleaning stale unprefixed files),
// and creates ~/.local/bin/testhdr symlink pointing to the running testhdr executable.
bool ensure_startup_symlinks(const EdidHdrSpecs& edid,
                             const std::string& connector_override = "",
                             const std::string& description_override = "",
                             const std::string& icc_dir_override = "",
                             const std::string& legacy_icc_dir_override = "",
                             const std::string& bin_dir_override = "",
                             const std::string& exe_path_override = "");

}  // namespace testhdr
