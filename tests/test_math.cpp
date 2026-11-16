#include "cie_math.h"
#include "edid.h"
#include "icc_gen.h"
#include "nit_math.h"
#include "overlay.h"

#include <lcms2.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

using namespace testhdr;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "Test failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                        \
        }                                                                        \
    } while (0)

static bool near(double a, double b, double tol) {
    return std::abs(a - b) <= tol;
}

static void test_pq_roundtrip() {
    const double test_nits[] = {0.0, 0.005, 0.1, 1.0, 10.0, 80.0, 100.0, 400.0, 616.884, 1000.0, 4000.0, 10000.0};
    for (double n : test_nits) {
        double pq = nits_to_pq(n);
        double back = pq_to_nits(pq);
        CHECK(pq >= 0.0 && pq <= 1.0);
        CHECK(near(back, n, std::max(1e-5, n * 1e-6)));
    }
    CHECK(near(nits_to_pq(100.0), 0.508078, 1e-4));
    CHECK(near(nits_to_pq(1000.0), 0.751827, 1e-4));
    CHECK(near(nits_to_pq(10000.0), 1.0, 1e-9));
}

static void test_scale_inverses() {
    const ScaleMode modes[] = {ScaleMode::PQ, ScaleMode::Log, ScaleMode::Linear};
    const double ts[] = {0.0, 0.1, 0.25, 0.5, 0.75, 1.0};
    for (ScaleMode m : modes) {
        for (double t : ts) {
            double nits = t_to_nits(t, 0.0, 1000.0, m);
            double t_back = nits_to_t(nits, 0.0, 1000.0, m);
            CHECK(near(t_back, t, 1e-6));
        }
        CHECK(near(t_to_nits(0.0, 5.0, 617.0, m), 5.0, 1e-5));
        CHECK(near(t_to_nits(1.0, 5.0, 617.0, m), 617.0, 1e-4));
    }
}

static void test_bands() {
    double first = band_index_to_nits(0, 20, 0.0, 1000.0, ScaleMode::PQ);
    double last = band_index_to_nits(19, 20, 0.0, 1000.0, ScaleMode::PQ);
    CHECK(near(first, 0.0, 1e-9));
    CHECK(near(last, 1000.0, 1e-4));
}

static void test_edid_hdr_parser() {
    std::vector<uint8_t> edid(256, 0);
    const uint8_t hdr[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    for (int i = 0; i < 8; ++i) edid[i] = hdr[i];
    edid[54 + 3] = 0xFC;
    const char* name = "TEST-HDR\n";
    for (int i = 0; i < 9; ++i) edid[54 + 5 + i] = static_cast<uint8_t>(name[i]);
    edid[126] = 1;

    edid[128] = 0x02;
    edid[129] = 0x03;
    edid[130] = 12;
    edid[132] = 0xE6;
    edid[133] = 0x06;
    edid[134] = 0x05;
    edid[135] = 0x01;
    edid[136] = 116;
    edid[137] = 96;
    edid[138] = 7;

    EdidHdrSpecs s = parse_edid_bytes(edid, "DP-1");
    CHECK(s.found);
    CHECK(s.monitor_name == "TEST-HDR");
    CHECK(s.hdr_capable);
    CHECK(s.supports_pq);
    CHECK(!s.supports_hlg);
    CHECK(s.has_max && near(s.max_nits, 616.884, 0.01));
    CHECK(s.has_avg && near(s.avg_nits, 400.0, 0.01));
    CHECK(s.has_min && near(s.min_nits, 0.00465, 0.001));

    // Verify reading real sysfs EDID (which reports st_size == 0)
    if (std::filesystem::exists("/sys/class/drm/card1-eDP-1/edid")) {
        EdidHdrSpecs live = probe_drm_edid("eDP-1");
        CHECK(live.found);
        CHECK(live.hdr_capable);
        CHECK(live.supports_p3);
        CHECK(live.has_max && live.max_nits > 600.0);
        CHECK(live.has_avg && near(live.avg_nits, 400.0, 0.5));
        CHECK(live.has_min && live.min_nits < 0.01);
        CHECK(live.has_chromaticity);
        CHECK(live.red_x > 0.65 && live.green_y > 0.70);
        CHECK(near(live.gamma, 2.20, 0.05));
    }
}

static void test_icc_generation() {
    EdidHdrSpecs specs;
    specs.found = true;
    specs.connector = "eDP-1";
    specs.monitor_name = "TestPanel";
    specs.hdr_capable = true;
    specs.has_max = true;
    specs.max_nits = 617.0;
    specs.has_chromaticity = true;
    specs.red_x = 0.6796;
    specs.red_y = 0.3203;
    specs.green_x = 0.2373;
    specs.green_y = 0.7226;
    specs.blue_x = 0.1396;
    specs.blue_y = 0.0498;
    specs.white_x = 0.3125;
    specs.white_y = 0.3291;
    specs.gamma = 2.20;

    std::filesystem::path tmp_dir = std::filesystem::temp_directory_path() / "testhdr_icc_test";
    std::filesystem::remove_all(tmp_dir);
    GeneratedIccSet set = generate_calibrated_icc_profiles(
        specs, "eDP-1", tmp_dir.string(), "Samsung Display Corp. 0x4180");
    CHECK(set.ok);
    CHECK(set.screen_prefix == "Samsung_Display_Corp_0x4180-eDP-1");
    CHECK(std::filesystem::exists(set.native_icc_path));
    CHECK(std::filesystem::exists(set.p3_icc_path));
    CHECK(std::filesystem::exists(set.srgb_icc_path));
    CHECK(std::filesystem::exists(set.bt2020_linear_icc_path));
    CHECK(std::filesystem::exists(set.bt2020_pq_icc_path));

    cmsHPROFILE h = cmsOpenProfileFromFile(set.native_icc_path.c_str(), "r");
    CHECK(h != nullptr);
    CHECK(cmsGetDeviceClass(h) == cmsSigDisplayClass);
    CHECK(cmsGetColorSpace(h) == cmsSigRgbData);
    const cmsCIEXYZ* lumi = static_cast<const cmsCIEXYZ*>(cmsReadTag(h, cmsSigLuminanceTag));
    CHECK(lumi != nullptr);
    CHECK(near(lumi->Y, 617.0, 0.5));
    cmsCloseProfile(h);

    cmsHPROFILE hpq = cmsOpenProfileFromFile(set.bt2020_pq_icc_path.c_str(), "r");
    CHECK(hpq != nullptr);
    CHECK(cmsGetDeviceClass(hpq) == cmsSigDisplayClass);
    cmsCloseProfile(hpq);

    // Verify startup symlinks creation in temporary directories
    std::filesystem::path legacy_dir = tmp_dir / "legacy_icc";
    std::filesystem::path bin_dir = tmp_dir / "bin";
    std::filesystem::path fake_exe = tmp_dir / "testhdr";
    bool sym_ok = ensure_startup_symlinks(
        specs, "eDP-1", "Samsung Display Corp. 0x4180",
        tmp_dir.string(), legacy_dir.string(), bin_dir.string(), fake_exe.string());
    CHECK(sym_ok);
    CHECK(std::filesystem::is_symlink(legacy_dir / (set.screen_prefix + "-EDID-Native.icc")));
    CHECK(std::filesystem::is_symlink(legacy_dir / (set.screen_prefix + "-DCI-P3-D65.icc")));
    CHECK(std::filesystem::is_symlink(legacy_dir / (set.screen_prefix + "-sRGB-Calibrated.icc")));
    CHECK(std::filesystem::is_symlink(legacy_dir / (set.screen_prefix + "-BT2020-Linear.icc")));
    CHECK(std::filesystem::is_symlink(legacy_dir / (set.screen_prefix + "-BT2020-PQ.icc")));
    CHECK(std::filesystem::read_symlink(legacy_dir / (set.screen_prefix + "-EDID-Native.icc")) == set.native_icc_path);
    CHECK(std::filesystem::is_symlink(bin_dir / "testhdr"));
    CHECK(std::filesystem::read_symlink(bin_dir / "testhdr") == fake_exe);

    std::filesystem::remove_all(tmp_dir);
}

static void test_overlay_render() {
    OverlayState st;
    st.width_px = 1920;
    st.height_px = 1080;
    st.ui_scale = 1.5f;
    st.cursor_active = true;
    st.cursor_x_px = 960.0;
    st.cursor_y_px = 400.0;
    st.edid = probe_drm_edid();
    DisplayLimitsMonitor mon;
    st.mode_limits = mon.resolve(true, 7.71f, 1.0f, st.edid);
    CHECK(st.mode_limits.has_active_limit);
    CHECK(st.mode_limits.active_max_nits > 100.0);
    st.window_hdr_enabled = true;
    st.hdr_output_mode = HdrOutputMode::Hdr10Pq;
    st.mode_limits.hypr_format = "XRGB2101010";
    std::string header = build_live_header_title(st);
    CHECK(header.find("[10 BIT HDR") == 0);
    CHECK(header.find("Couleur: ULTRA HIGH") != std::string::npos);
    CHECK(header.find("Lumière: HIGH") != std::string::npos);

    std::vector<uint8_t> buf;
    render_overlay(st, buf);
    CHECK(buf.size() == static_cast<size_t>(st.width_px) * st.height_px * 4);
    bool has_nonzero = false;
    for (uint8_t b : buf) {
        if (b != 0) {
            has_nonzero = true;
            break;
        }
    }
    CHECK(has_nonzero);

    // Non-HDR resolves to estimated full-brightness SDR panel max scaled by backlight
    ModeLimitInfo sdr_lim = mon.resolve(false, 1.0f, 1.0f, st.edid);
    CHECK(sdr_lim.has_active_limit);
    double expected_sdr_full = estimated_sdr_full_nits(st.edid.avg_nits, st.edid.has_avg, st.edid.max_nits, st.edid.has_max);
    double bl_ratio = sdr_lim.backlight_available ? (std::clamp(sdr_lim.backlight_percent, 1, 100) / 100.0) : 1.0;
    CHECK(near(sdr_lim.active_max_nits, expected_sdr_full * bl_ratio, 1e-4));

    // Light quality tiers (based on panel luminance capability, SDR full or HDR peak)
    CHECK(classify_light_quality(200.0, false) == LightQualityTier::Low);
    CHECK(classify_light_quality(400.0, false) == LightQualityTier::Medium);
    CHECK(classify_light_quality(617.0, false) == LightQualityTier::High);
    CHECK(classify_light_quality(200.0, true) == LightQualityTier::Low);
    CHECK(classify_light_quality(350.0, true) == LightQualityTier::Medium);
    CHECK(classify_light_quality(617.0, true) == LightQualityTier::High);
    CHECK(classify_light_quality(1000.0, true) == LightQualityTier::UltraHigh);

    // Color quality & total color count calculation
    ColorQualityInfo sdr8 = evaluate_color_quality(8, HdrOutputMode::Sdr, false, 1, "srgb", 0.2, 80.0);
    CHECK(sdr8.effective_bpc == 8);
    CHECK(sdr8.levels_per_channel == 256);
    CHECK(sdr8.total_colors == 16777216ULL);
    CHECK(sdr8.gamut == "sRGB");
    CHECK(sdr8.tier == LightQualityTier::Low);

    ColorQualityInfo p3_10 = evaluate_color_quality(10, HdrOutputMode::Hdr10Pq, false, 9, "wide", 0.2, 80.0);
    CHECK(p3_10.effective_bpc == 10);
    CHECK(p3_10.levels_per_channel == 1024);
    CHECK(p3_10.total_colors == 1073741824ULL);
    CHECK(p3_10.gamut == "DCI-P3");
    CHECK(p3_10.tier == LightQualityTier::High);

    ColorQualityInfo hdr10 = evaluate_color_quality(10, HdrOutputMode::Hdr10Pq, true, 6, "hdr", 0.005, 617.0);
    CHECK(hdr10.effective_bpc == 10);
    CHECK(hdr10.total_colors == 1073741824ULL);
    CHECK(hdr10.gamut == "BT.2020");
    CHECK(hdr10.tier == LightQualityTier::UltraHigh);
    CHECK(hdr10.luminance_steps > 600 && hdr10.luminance_steps < 750);

    // 10-bit SDR modes
    ColorQualityInfo sdr10_srgb = evaluate_color_quality(10, HdrOutputMode::Sdr10, false, 1, "srgb", 0.2, 80.0);
    CHECK(sdr10_srgb.effective_bpc == 10);
    CHECK(sdr10_srgb.levels_per_channel == 1024);
    CHECK(sdr10_srgb.total_colors == 1073741824ULL);
    CHECK(sdr10_srgb.gamut == "sRGB");
    CHECK(sdr10_srgb.luminance_steps == 1024);
    CHECK(sdr10_srgb.tier == LightQualityTier::Medium);

    ColorQualityInfo sdr10_p3 = evaluate_color_quality(10, HdrOutputMode::Sdr10, false, 9, "dcip3", 0.2, 80.0);
    CHECK(sdr10_p3.effective_bpc == 10);
    CHECK(sdr10_p3.gamut == "DCI-P3");
    CHECK(sdr10_p3.tier == LightQualityTier::High);

    // 10-bit SDR header string
    OverlayState st_sdr10 = st;
    st_sdr10.window_hdr_enabled = false;
    st_sdr10.hdr_output_mode = HdrOutputMode::Sdr10;
    st_sdr10.mode_limits.hypr_cm_preset = "srgb";
    st_sdr10.mode_limits.hypr_format = "XRGB2101010";
    st_sdr10.mode_limits.wl_primaries = 1;
    st_sdr10.mode_limits.monique_active_profile.clear();
    st_sdr10.mode_limits.active_icc_path.clear();
    std::string h_sdr10 = build_live_header_title(st_sdr10);
    CHECK(h_sdr10.find("[10 BIT SDR") == 0);
    CHECK(h_sdr10.find("sRGB 10-bit") != std::string::npos);

    // Render TestPattern (all 6) & CieTriangle views
    st_sdr10.view_mode = ViewMode::TestPattern;
    for (int p = 0; p < kTestPatternCount; ++p) {
        st_sdr10.test_pattern = static_cast<TestPattern>(p);
        render_overlay(st_sdr10, buf);
        CHECK(buf.size() == static_cast<size_t>(st_sdr10.width_px) * st_sdr10.height_px * 4);
    }

    st_sdr10.view_mode = ViewMode::CieTriangle;
    double px = 0.0, py = 0.0, psize = 0.0;
    compute_cie_plot_rect(st_sdr10, px, py, psize);
    CHECK(psize > 100.0);

    double mx = 0.0, my = 0.0, mw = 0.0, mh = 0.0, mhh = 0.0, mrh = 0.0;
    compute_hud_menu_rect(st_sdr10, mx, my, mw, mh, mhh, mrh);
    CHECK(mw > 200.0 && mh > 200.0);
    CHECK(px + psize <= mx + 1.0);

    st_sdr10.menu_selected = static_cast<int>(HudMenuItem::ExportMonique);
    st_sdr10.cursor_active = true;
    st_sdr10.cursor_x_px = px + psize * 0.4;
    st_sdr10.cursor_y_px = py + psize * 0.5;
    render_overlay(st_sdr10, buf);
    CHECK(buf.size() == static_cast<size_t>(st_sdr10.width_px) * st_sdr10.height_px * 4);
}

static void test_cie_math() {
    CHECK(point_in_cie_locus({0.3127, 0.3290}));
    CHECK(!point_in_cie_locus({0.0, 0.85}));
    CHECK(!point_in_cie_locus({0.85, 0.25}));

    CHECK(point_in_triangle({0.3127, 0.3290}, kGamutSrgb));
    CHECK(point_in_triangle({0.6800, 0.3200}, kGamutDciP3));
    CHECK(!point_in_triangle({0.6800, 0.3200}, kGamutSrgb));

    double p3_in_srgb = gamut_coverage_percent(kGamutSrgb, kGamutDciP3);
    CHECK(p3_in_srgb > 70.0 && p3_in_srgb < 77.0);
    CHECK(near(gamut_coverage_percent(kGamutDciP3, kGamutSrgb), 100.0, 0.5));
    CHECK(near(gamut_coverage_percent(kGamutBt2020, kGamutDciP3), 100.0, 0.1));
    CHECK(near(gamut_coverage_percent(kGamutBt2020, kGamutSrgb), 100.0, 0.1));
}

static void test_omarchy_detection() {
    CHECK(parse_os_release_is_omarchy("NAME=\"Omarchy\"\nID=omarchy\nID_LIKE=arch\n"));
    CHECK(parse_os_release_is_omarchy("NAME=Omarchy\nID=arch\n"));
    CHECK(!parse_os_release_is_omarchy("NAME=\"Arch Linux\"\nID=arch\n"));
    CHECK(!parse_os_release_is_omarchy("NAME=\"Ubuntu\"\nID=ubuntu\n"));
    if (std::filesystem::exists("/usr/share/omarchy/bin/omarchy-pkg-aur-add")) {
        CHECK(is_omarchy_system());
    }
}

int main() {
    test_pq_roundtrip();
    test_scale_inverses();
    test_bands();
    test_edid_hdr_parser();
    test_icc_generation();
    test_overlay_render();
    test_cie_math();
    test_omarchy_detection();
    std::printf("All testhdr unit tests passed.\n");
    return 0;
}
