#include "edid.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

namespace testhdr {
namespace {

std::string trim_edid_text(const uint8_t* ptr, size_t len) {
    std::string s;
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = ptr[i];
        if (c == '\n' || c == '\r' || c == '\0') break;
        if (c >= 0x20 && c <= 0x7E) {
            s.push_back(static_cast<char>(c));
        }
    }
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    return s;
}

bool read_file_bytes(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return false;
    out.clear();
    // Sysfs files (e.g. /sys/class/drm/*/edid) report st_size == 0, so read sequentially until EOF.
    char buf[4096];
    while (ifs) {
        ifs.read(buf, sizeof(buf));
        std::streamsize got = ifs.gcount();
        if (got > 0) {
            if (out.size() + static_cast<size_t>(got) > 65536) return false;
            out.insert(out.end(),
                       reinterpret_cast<const uint8_t*>(buf),
                       reinterpret_cast<const uint8_t*>(buf) + got);
        }
    }
    return !out.empty();
}

bool is_connector_connected(const std::filesystem::path& drm_dir) {
    std::ifstream ifs(drm_dir / "status");
    if (!ifs) return true;  // Assume usable if status not readable
    std::string status;
    std::getline(ifs, status);
    return status == "connected";
}

}  // namespace

EdidHdrSpecs parse_edid_bytes(const std::vector<uint8_t>& data, const std::string& connector_name) {
    EdidHdrSpecs specs;
    specs.connector = connector_name;

    if (data.size() < 128) {
        return specs;
    }
    static constexpr uint8_t kEdidHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (!std::equal(std::begin(kEdidHeader), std::end(kEdidHeader), data.begin())) {
        return specs;
    }

    specs.found = true;

    // 3-letter PNP vendor ID (bytes 8..9) and little-endian product code (bytes 10..11)
    {
        uint8_t b8 = data[8];
        uint8_t b9 = data[9];
        int c1 = (b8 >> 2) & 0x1F;
        int c2 = ((b8 & 0x03) << 3) | ((b9 >> 5) & 0x07);
        int c3 = b9 & 0x1F;
        if (c1 >= 1 && c1 <= 26 && c2 >= 1 && c2 <= 26 && c3 >= 1 && c3 <= 26) {
            specs.manufacturer_pnp.push_back(static_cast<char>('A' + c1 - 1));
            specs.manufacturer_pnp.push_back(static_cast<char>('A' + c2 - 1));
            specs.manufacturer_pnp.push_back(static_cast<char>('A' + c3 - 1));
        }
        uint16_t prod = static_cast<uint16_t>(data[10]) | (static_cast<uint16_t>(data[11]) << 8);
        if (prod != 0) {
            char pbuf[16];
            std::snprintf(pbuf, sizeof(pbuf), "0x%04x", prod);
            specs.product_code_hex = pbuf;
        }
    }

    // Extract monitor name from the 4 base descriptor blocks (offsets 54, 72, 90, 108)
    for (size_t off = 54; off <= 108; off += 18) {
        if (off + 18 > data.size()) break;
        if (data[off] == 0x00 && data[off + 1] == 0x00 && data[off + 2] == 0x00 &&
            data[off + 3] == 0xFC && data[off + 4] == 0x00) {
            specs.monitor_name = trim_edid_text(&data[off + 5], 13);
            break;
        }
    }

    // Gamma (byte 23) and CIE 1931 10-bit Chromaticity Coordinates (bytes 25..34)
    if (data[23] != 0xFF && data[23] > 0) {
        specs.gamma = (static_cast<double>(data[23]) + 100.0) / 100.0;
    }
    auto decode_xy = [](uint8_t hi, uint8_t lo2) -> double {
        uint16_t v = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 2) | (lo2 & 0x03u));
        return static_cast<double>(v) / 1024.0;
    };
    double rx = decode_xy(data[27], static_cast<uint8_t>(data[25] >> 6));
    double ry = decode_xy(data[28], static_cast<uint8_t>(data[25] >> 4));
    double gx = decode_xy(data[29], static_cast<uint8_t>(data[25] >> 2));
    double gy = decode_xy(data[30], data[25]);
    double bx = decode_xy(data[31], static_cast<uint8_t>(data[26] >> 6));
    double by = decode_xy(data[32], static_cast<uint8_t>(data[26] >> 4));
    double wx = decode_xy(data[33], static_cast<uint8_t>(data[26] >> 2));
    double wy = decode_xy(data[34], data[26]);
    if (rx > 0.0 && ry > 0.0 && gx > 0.0 && gy > 0.0 &&
        bx > 0.0 && by > 0.0 && wx > 0.0 && wy > 0.0) {
        specs.has_chromaticity = true;
        specs.red_x = rx;
        specs.red_y = ry;
        specs.green_x = gx;
        specs.green_y = gy;
        specs.blue_x = bx;
        specs.blue_y = by;
        specs.white_x = wx;
        specs.white_y = wy;
    }

    uint8_t ext_count = data[126];
    for (uint8_t ext = 0; ext < ext_count; ++ext) {
        size_t base = static_cast<size_t>(ext + 1) * 128;
        if (base + 128 > data.size()) break;
        const uint8_t* blk = &data[base];

        // CTA-861 Extension Block (Tag 0x02)
        if (blk[0] != 0x02) continue;
        uint8_t dtd_offset = blk[2];
        if (dtd_offset <= 4 || dtd_offset > 127) continue;

        size_t pos = 4;
        while (pos < dtd_offset) {
            uint8_t hdr = blk[pos];
            uint8_t tag = (hdr >> 5) & 0x07;
            uint8_t len = hdr & 0x1F;
            if (pos + 1 + len > dtd_offset) break;

            // Extended Tag (0x07), Colorimetry Data Block (0x05)
            if (tag == 0x07 && len >= 3 && blk[pos + 1] == 0x05) {
                uint8_t c1 = blk[pos + 2];
                uint8_t c2 = blk[pos + 3];
                if ((c1 & 0xC0u) != 0 || (c2 & 0x01u) != 0) {
                    specs.supports_bt2020 = true;
                    specs.supports_p3 = true;
                }
                if ((c2 & 0x80u) != 0) {
                    specs.supports_p3 = true;
                }
            }

            // Extended Tag (0x07), HDR Static Metadata Data Block (0x06)
            if (tag == 0x07 && len >= 3 && blk[pos + 1] == 0x06) {
                uint8_t eotf = blk[pos + 2];
                specs.supports_pq = (eotf & (1u << 2)) != 0;
                specs.supports_hlg = (eotf & (1u << 3)) != 0;
                specs.hdr_capable = specs.supports_pq || specs.supports_hlg || ((eotf & (1u << 1)) != 0);
                if (specs.hdr_capable) {
                    specs.supports_p3 = true;
                }

                if (len >= 4 && blk[pos + 4] > 0) {
                    specs.max_nits = 50.0 * std::pow(2.0, static_cast<double>(blk[pos + 4]) / 32.0);
                    specs.has_max = true;
                }
                if (len >= 5 && blk[pos + 5] > 0) {
                    specs.avg_nits = 50.0 * std::pow(2.0, static_cast<double>(blk[pos + 5]) / 32.0);
                    specs.has_avg = true;
                }
                if (len >= 6) {
                    uint8_t cv_min = blk[pos + 6];
                    if (cv_min == 0) {
                        specs.min_nits = 0.0;
                        specs.has_min = true;
                    } else if (specs.has_max && specs.max_nits > 0.0) {
                        double ratio = static_cast<double>(cv_min) / 255.0;
                        specs.min_nits = specs.max_nits * (ratio * ratio) / 100.0;
                        specs.has_min = true;
                    }
                }
            }
            pos += 1 + len;
        }
    }

    return specs;
}

EdidHdrSpecs probe_drm_edid(const std::string& preferred_connector) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path drm_root("/sys/class/drm");
    if (!fs::exists(drm_root, ec)) {
        return {};
    }

    EdidHdrSpecs fallback;
    for (const auto& entry : fs::directory_iterator(drm_root, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string dir_name = entry.path().filename().string();
        auto dash = dir_name.find('-');
        if (dash == std::string::npos || dir_name.rfind("card", 0) != 0) continue;

        std::string connector = dir_name.substr(dash + 1);
        if (!preferred_connector.empty() && connector != preferred_connector) {
            continue;
        }
        if (!is_connector_connected(entry.path())) continue;

        fs::path edid_path = entry.path() / "edid";
        std::vector<uint8_t> bytes;
        if (!read_file_bytes(edid_path, bytes)) continue;

        EdidHdrSpecs parsed = parse_edid_bytes(bytes, connector);
        if (!parsed.found) continue;
        if (parsed.hdr_capable) {
            return parsed;
        }
        if (!fallback.found) {
            fallback = parsed;
        }
    }
    return fallback;
}

}  // namespace testhdr
