#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace testhdr {

struct EdidHdrSpecs {
    bool found = false;
    std::string connector;
    std::string monitor_name;
    std::string manufacturer_pnp;
    std::string product_code_hex;
    bool hdr_capable = false;
    bool supports_pq = false;
    bool supports_hlg = false;
    bool supports_p3 = false;
    bool supports_bt2020 = false;
    double max_nits = 0.0;
    double avg_nits = 0.0;
    double min_nits = 0.0;
    bool has_max = false;
    bool has_avg = false;
    bool has_min = false;

    bool has_chromaticity = false;
    double red_x = 0.640;
    double red_y = 0.330;
    double green_x = 0.300;
    double green_y = 0.600;
    double blue_x = 0.150;
    double blue_y = 0.060;
    double white_x = 0.3127;
    double white_y = 0.3290;
    double gamma = 2.2;
};

EdidHdrSpecs parse_edid_bytes(const std::vector<uint8_t>& data, const std::string& connector_name = "");
EdidHdrSpecs probe_drm_edid(const std::string& preferred_connector = "");

}  // namespace testhdr
