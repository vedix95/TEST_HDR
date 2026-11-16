#include "display_limits.h"
#include "icc_gen.h"
#include "nit_math.h"

#include "color-management-v1-client-protocol.h"

#include <wayland-client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace testhdr {
namespace {

std::string extract_json_string(const std::string& obj, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t pos = obj.find(pat);
    if (pos == std::string::npos) return "";
    pos = obj.find(':', pos + pat.size());
    if (pos == std::string::npos) return "";
    size_t q1 = obj.find('"', pos + 1);
    if (q1 == std::string::npos) return "";
    size_t q2 = obj.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return obj.substr(q1 + 1, q2 - q1 - 1);
}

bool extract_json_number(const std::string& obj, const std::string& key, double& out) {
    std::string pat = "\"" + key + "\"";
    size_t pos = obj.find(pat);
    if (pos == std::string::npos) return false;
    pos = obj.find(':', pos + pat.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < obj.size() && std::isspace(static_cast<unsigned char>(obj[pos]))) ++pos;
    if (pos >= obj.size()) return false;
    char* endptr = nullptr;
    double val = std::strtod(obj.c_str() + pos, &endptr);
    if (endptr == obj.c_str() + pos) return false;
    out = val;
    return true;
}

bool extract_json_bool(const std::string& obj, const std::string& key, bool& out) {
    std::string pat = "\"" + key + "\"";
    size_t pos = obj.find(pat);
    if (pos == std::string::npos) return false;
    pos = obj.find(':', pos + pat.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < obj.size() && std::isspace(static_cast<unsigned char>(obj[pos]))) ++pos;
    if (obj.compare(pos, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (obj.compare(pos, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

std::vector<std::string> split_json_array_objects(const std::string& json) {
    std::vector<std::string> objs;
    int depth = 0;
    size_t start = std::string::npos;
    bool in_str = false;
    bool esc = false;
    for (size_t i = 0; i < json.size(); ++i) {
        char c = json[i];
        if (esc) {
            esc = false;
            continue;
        }
        if (c == '\\' && in_str) {
            esc = true;
            continue;
        }
        if (c == '"') {
            in_str = !in_str;
            continue;
        }
        if (in_str) continue;
        if (c == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (c == '}') {
            if (depth > 0) {
                --depth;
                if (depth == 0 && start != std::string::npos) {
                    objs.push_back(json.substr(start, i - start + 1));
                    start = std::string::npos;
                }
            }
        }
    }
    return objs;
}

}  // namespace

struct DisplayLimitsMonitor::Impl {
    wl_display* display = nullptr;
    wl_surface* surface = nullptr;
    wl_registry* registry = nullptr;
    wp_color_manager_v1* color_manager = nullptr;
    wp_color_management_surface_feedback_v1* feedback = nullptr;
    wp_image_description_v1* pending_desc = nullptr;
    wp_image_description_info_v1* pending_info = nullptr;

    ModeLimitInfo data;
    ModeLimitInfo staged;
    bool dirty = false;

    void request_preferred();
    void clear_pending_desc();
    void clear_pending_info();
};

static void on_info_done(void* user_data, wp_image_description_info_v1* info) {
    auto* impl = static_cast<DisplayLimitsMonitor::Impl*>(user_data);
    impl->data.wayland_cm_available = true;
    impl->data.wayland_info_ready = true;
    impl->data.wl_min_nits = impl->staged.wl_min_nits;
    impl->data.wl_max_nits = impl->staged.wl_max_nits;
    impl->data.wl_ref_nits = impl->staged.wl_ref_nits;
    impl->data.wl_target_min_nits = impl->staged.wl_target_min_nits;
    impl->data.wl_target_max_nits = impl->staged.wl_target_max_nits;
    impl->data.wl_max_cll = impl->staged.wl_max_cll;
    impl->data.wl_max_fall = impl->staged.wl_max_fall;
    impl->data.wl_tf = impl->staged.wl_tf;
    impl->data.wl_primaries = impl->staged.wl_primaries;
    impl->dirty = true;
    if (impl->pending_info == info) {
        impl->pending_info = nullptr;
    }
    wp_image_description_info_v1_destroy(info);
}

static void on_info_icc_file(void*, wp_image_description_info_v1*, int32_t fd, uint32_t) {
    if (fd >= 0) {
        FILE* f = fdopen(fd, "r");
        if (f) fclose(f);
    }
}

static void on_info_primaries(void*, wp_image_description_info_v1*,
                              int32_t, int32_t, int32_t, int32_t,
                              int32_t, int32_t, int32_t, int32_t) {}

static void on_info_primaries_named(void* user_data, wp_image_description_info_v1*, uint32_t primaries) {
    auto* impl = static_cast<DisplayLimitsMonitor::Impl*>(user_data);
    impl->staged.wl_primaries = primaries;
}

static void on_info_tf_power(void*, wp_image_description_info_v1*, uint32_t) {}

static void on_info_tf_named(void* user_data, wp_image_description_info_v1*, uint32_t tf) {
    auto* impl = static_cast<DisplayLimitsMonitor::Impl*>(user_data);
    impl->staged.wl_tf = tf;
}

static void on_info_luminances(void* user_data, wp_image_description_info_v1*,
                               uint32_t min_lum, uint32_t max_lum, uint32_t reference_lum) {
    auto* impl = static_cast<DisplayLimitsMonitor::Impl*>(user_data);
    impl->staged.wl_min_nits = static_cast<double>(min_lum) / 10000.0;
    impl->staged.wl_max_nits = static_cast<double>(max_lum);
    impl->staged.wl_ref_nits = static_cast<double>(reference_lum);
}

static void on_info_target_primaries(void*, wp_image_description_info_v1*,
                                     int32_t, int32_t, int32_t, int32_t,
                                     int32_t, int32_t, int32_t, int32_t) {}

static void on_info_target_luminance(void* user_data, wp_image_description_info_v1*,
                                     uint32_t min_lum, uint32_t max_lum) {
    auto* impl = static_cast<DisplayLimitsMonitor::Impl*>(user_data);
    impl->staged.wl_target_min_nits = static_cast<double>(min_lum) / 10000.0;
    impl->staged.wl_target_max_nits = static_cast<double>(max_lum);
}

static void on_info_target_max_cll(void* user_data, wp_image_description_info_v1*, uint32_t max_cll) {
    auto* impl = static_cast<DisplayLimitsMonitor::Impl*>(user_data);
    impl->staged.wl_max_cll = max_cll;
}

static void on_info_target_max_fall(void* user_data, wp_image_description_info_v1*, uint32_t max_fall) {
    auto* impl = static_cast<DisplayLimitsMonitor::Impl*>(user_data);
    impl->staged.wl_max_fall = max_fall;
}

static const wp_image_description_info_v1_listener kInfoListener = {
    .done = on_info_done,
    .icc_file = on_info_icc_file,
    .primaries = on_info_primaries,
    .primaries_named = on_info_primaries_named,
    .tf_power = on_info_tf_power,
    .tf_named = on_info_tf_named,
    .luminances = on_info_luminances,
    .target_primaries = on_info_target_primaries,
    .target_luminance = on_info_target_luminance,
    .target_max_cll = on_info_target_max_cll,
    .target_max_fall = on_info_target_max_fall,
};

static void on_desc_failed(void* user_data, wp_image_description_v1* desc, uint32_t, const char*) {
    auto* impl = static_cast<DisplayLimitsMonitor::Impl*>(user_data);
    if (impl->pending_desc == desc) {
        impl->pending_desc = nullptr;
    }
    wp_image_description_v1_destroy(desc);
}

static void query_desc_info(DisplayLimitsMonitor::Impl* impl, wp_image_description_v1* desc) {
    impl->clear_pending_info();
    impl->staged = ModeLimitInfo{};
    impl->pending_info = wp_image_description_v1_get_information(desc);
    if (impl->pending_info) {
        wp_image_description_info_v1_add_listener(impl->pending_info, &kInfoListener, impl);
    }
    if (impl->pending_desc == desc) {
        impl->pending_desc = nullptr;
    }
    wp_image_description_v1_destroy(desc);
}

static void on_desc_ready(void* user_data, wp_image_description_v1* desc, uint32_t) {
    query_desc_info(static_cast<DisplayLimitsMonitor::Impl*>(user_data), desc);
}

static void on_desc_ready2(void* user_data, wp_image_description_v1* desc, uint32_t, uint32_t) {
    query_desc_info(static_cast<DisplayLimitsMonitor::Impl*>(user_data), desc);
}

static const wp_image_description_v1_listener kDescListener = {
    .failed = on_desc_failed,
    .ready = on_desc_ready,
    .ready2 = on_desc_ready2,
};

void DisplayLimitsMonitor::Impl::clear_pending_info() {
    if (pending_info) {
        wp_image_description_info_v1_destroy(pending_info);
        pending_info = nullptr;
    }
}

void DisplayLimitsMonitor::Impl::clear_pending_desc() {
    if (pending_desc) {
        wp_image_description_v1_destroy(pending_desc);
        pending_desc = nullptr;
    }
}

void DisplayLimitsMonitor::Impl::request_preferred() {
    if (!feedback) return;
    clear_pending_desc();
    pending_desc = wp_color_management_surface_feedback_v1_get_preferred(feedback);
    if (pending_desc) {
        wp_image_description_v1_add_listener(pending_desc, &kDescListener, this);
    }
}

static void on_feedback_preferred_changed(void* user_data, wp_color_management_surface_feedback_v1*, uint32_t) {
    static_cast<DisplayLimitsMonitor::Impl*>(user_data)->request_preferred();
}

static void on_feedback_preferred_changed2(void* user_data, wp_color_management_surface_feedback_v1*, uint32_t, uint32_t) {
    static_cast<DisplayLimitsMonitor::Impl*>(user_data)->request_preferred();
}

static const wp_color_management_surface_feedback_v1_listener kFeedbackListener = {
    .preferred_changed = on_feedback_preferred_changed,
    .preferred_changed2 = on_feedback_preferred_changed2,
};

static void on_cm_intent(void*, wp_color_manager_v1*, uint32_t) {}
static void on_cm_feature(void*, wp_color_manager_v1*, uint32_t) {}
static void on_cm_tf(void*, wp_color_manager_v1*, uint32_t) {}
static void on_cm_primaries(void*, wp_color_manager_v1*, uint32_t) {}
static void on_cm_done(void*, wp_color_manager_v1*) {}

static const wp_color_manager_v1_listener kCmListener = {
    .supported_intent = on_cm_intent,
    .supported_feature = on_cm_feature,
    .supported_tf_named = on_cm_tf,
    .supported_primaries_named = on_cm_primaries,
    .done = on_cm_done,
};

static void on_registry_global(void* user_data, wl_registry* reg, uint32_t name,
                               const char* interface, uint32_t version) {
    auto* impl = static_cast<DisplayLimitsMonitor::Impl*>(user_data);
    if (std::strcmp(interface, wp_color_manager_v1_interface.name) == 0 && !impl->color_manager) {
        uint32_t bind_ver = std::min<uint32_t>(version, 3u);
        impl->color_manager = static_cast<wp_color_manager_v1*>(
            wl_registry_bind(reg, name, &wp_color_manager_v1_interface, bind_ver));
        if (impl->color_manager) {
            impl->data.wayland_cm_available = true;
            wp_color_manager_v1_add_listener(impl->color_manager, &kCmListener, impl);
            if (impl->surface) {
                impl->feedback = wp_color_manager_v1_get_surface_feedback(impl->color_manager, impl->surface);
                if (impl->feedback) {
                    wp_color_management_surface_feedback_v1_add_listener(
                        impl->feedback, &kFeedbackListener, impl);
                    impl->request_preferred();
                }
            }
        }
    }
}

static void on_registry_global_remove(void*, wl_registry*, uint32_t) {}

static const wl_registry_listener kRegistryListener = {
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

DisplayLimitsMonitor::~DisplayLimitsMonitor() {
    shutdown();
}

void DisplayLimitsMonitor::init_wayland(wl_display* display, wl_surface* surface) {
    shutdown();
    impl_ = new Impl();
    impl_->display = display;
    impl_->surface = surface;
    if (!display || !surface) return;

    impl_->registry = wl_display_get_registry(display);
    if (!impl_->registry) return;
    wl_registry_add_listener(impl_->registry, &kRegistryListener, impl_);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
}

void DisplayLimitsMonitor::poll_hyprland(const std::string& preferred_connector) {
    if (!impl_) impl_ = new Impl();

    FILE* pipe = popen("hyprctl monitors -j 2>/dev/null", "r");
    if (!pipe) return;

    std::string json;
    char buf[4096];
    while (size_t n = std::fread(buf, 1, sizeof(buf), pipe)) {
        json.append(buf, n);
    }
    int rc = pclose(pipe);
    if (rc != 0 || json.empty()) return;

    std::vector<std::string> monitors = split_json_array_objects(json);
    if (monitors.empty()) return;

    std::string chosen;
    for (const auto& m : monitors) {
        std::string name = extract_json_string(m, "name");
        if (!preferred_connector.empty() && name == preferred_connector) {
            chosen = m;
            break;
        }
        bool focused = false;
        if (chosen.empty() && extract_json_bool(m, "focused", focused) && focused) {
            chosen = m;
        }
    }
    if (chosen.empty()) chosen = monitors.front();

    std::string mon = extract_json_string(chosen, "name");
    std::string desc = extract_json_string(chosen, "description");
    std::string cm = extract_json_string(chosen, "colorManagementPreset");
    std::string fmt = extract_json_string(chosen, "currentFormat");
    double sdr_min = 0.0, sdr_max = 0.0, sdr_b = 1.0;
    extract_json_number(chosen, "sdrMinLuminance", sdr_min);
    extract_json_number(chosen, "sdrMaxLuminance", sdr_max);
    extract_json_number(chosen, "sdrBrightness", sdr_b);
    std::string active_icc = detect_active_hypr_icc(mon);

    std::string monique_active;
    {
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        const char* home = std::getenv("HOME");
        std::string cfg_path;
        if (xdg && xdg[0] != '\0') cfg_path = std::string(xdg) + "/monique/settings.json";
        else if (home && home[0] != '\0') cfg_path = std::string(home) + "/.config/monique/settings.json";
        if (!cfg_path.empty()) {
            std::ifstream sfs(cfg_path);
            if (sfs) {
                std::string content((std::istreambuf_iterator<char>(sfs)), std::istreambuf_iterator<char>());
                monique_active = extract_json_string(content, "active_profile");
            }
        }
    }

    int bl_pct = 100;
    bool bl_ok = read_sysfs_backlight(bl_pct);

    if (mon != impl_->data.hypr_monitor || desc != impl_->data.hypr_description ||
        cm != impl_->data.hypr_cm_preset ||
        fmt != impl_->data.hypr_format || sdr_min != impl_->data.hypr_sdr_min_nits ||
        sdr_max != impl_->data.hypr_sdr_max_nits || sdr_b != impl_->data.hypr_sdr_brightness ||
        active_icc != impl_->data.active_icc_path ||
        monique_active != impl_->data.monique_active_profile ||
        bl_ok != impl_->data.backlight_available ||
        bl_pct != impl_->data.backlight_percent) {
        impl_->data.hypr_available = true;
        impl_->data.hypr_monitor = mon;
        impl_->data.hypr_description = desc;
        impl_->data.hypr_cm_preset = cm;
        impl_->data.hypr_format = fmt;
        impl_->data.hypr_sdr_min_nits = sdr_min;
        impl_->data.hypr_sdr_max_nits = sdr_max;
        impl_->data.hypr_sdr_brightness = sdr_b;
        impl_->data.active_icc_path = active_icc;
        impl_->data.monique_active_profile = monique_active;
        impl_->data.backlight_available = bl_ok;
        impl_->data.backlight_percent = bl_pct;
        impl_->dirty = true;
    }
}

bool DisplayLimitsMonitor::consume_dirty() {
    if (!impl_) return false;
    bool d = impl_->dirty;
    impl_->dirty = false;
    return d;
}

ModeLimitInfo DisplayLimitsMonitor::resolve(bool window_hdr_enabled,
                                            float window_hdr_headroom,
                                            float window_sdr_white_level,
                                            const EdidHdrSpecs& edid) const {
    ModeLimitInfo out = impl_ ? impl_->data : ModeLimitInfo{};
    if (!out.backlight_available) {
        int bl_pct = 100;
        if (read_sysfs_backlight(bl_pct)) {
            out.backlight_available = true;
            out.backlight_percent = bl_pct;
        }
    }
    bool hypr_hdr = out.hypr_available &&
                    (out.hypr_cm_preset == "hdr" || out.hypr_cm_preset == "hdredid");

    // In SDR, Wayland/Hyprland report 80 nits as nominal signal white, not actual panel brightness.
    // Use the panel's estimated full-brightness SDR maximum scaled by hardware backlight level.
    if (!window_hdr_enabled && !hypr_hdr) {
        double full_nits = estimated_sdr_full_nits(
            edid.avg_nits, edid.has_avg, edid.max_nits, edid.has_max);
        double ratio = out.backlight_available
            ? (static_cast<double>(std::clamp(out.backlight_percent, 1, 100)) / 100.0)
            : 1.0;
        out.has_active_limit = true;
        out.active_max_nits = full_nits * ratio;
        out.active_min_nits = edid.has_min ? edid.min_nits : 0.2;
        if (out.backlight_available) {
            out.active_source = "Dalle SDR " + std::to_string(out.backlight_percent) +
                                "% / max " + std::to_string(static_cast<int>(std::llround(full_nits))) + " nits";
        } else {
            out.active_source = edid.has_avg
                ? "Dalle SDR pleine lumière (EDID Moy)"
                : (edid.has_max ? "Dalle SDR pleine lumière (EDID)" : "Dalle SDR pleine lumière (est.)");
        }
        return out;
    }

    // 1. Wayland wp_color_management_v1 compositor target/primary luminance
    if (out.wayland_info_ready && out.wl_target_max_nits > 0.0) {
        out.has_active_limit = true;
        out.active_max_nits = out.wl_target_max_nits;
        out.active_min_nits = out.wl_target_min_nits;
        out.active_source = "Wayland CM (target)";
    } else if (out.wayland_info_ready && out.wl_max_nits > 0.0) {
        out.has_active_limit = true;
        out.active_max_nits = out.wl_max_nits;
        out.active_min_nits = out.wl_min_nits;
        out.active_source = "Wayland CM";
    } else if (window_hdr_enabled && window_hdr_headroom > 1.01f) {
        // 2. SDL3 HDR headroom
        double sdr_ref = (out.hypr_sdr_max_nits > 0.0)
            ? out.hypr_sdr_max_nits
            : (80.0 * std::max(0.1f, window_sdr_white_level));
        out.has_active_limit = true;
        out.active_max_nits = sdr_ref * static_cast<double>(window_hdr_headroom);
        out.active_min_nits = edid.has_min ? edid.min_nits : 0.0;
        out.active_source = "SDL HDR Headroom";
    } else if (hypr_hdr && edid.has_max) {
        // 3. Hyprland in HDR preset
        out.has_active_limit = true;
        out.active_max_nits = edid.max_nits;
        out.active_min_nits = edid.has_min ? edid.min_nits : 0.0;
        out.active_source = "Hyprland (" + out.hypr_cm_preset + ")";
    } else {
        // 4. Fallback HDR
        out.has_active_limit = true;
        out.active_max_nits = edid.has_max ? edid.max_nits : 400.0;
        out.active_min_nits = edid.has_min ? edid.min_nits : 0.0;
        out.active_source = "EDID HDR";
    }

    return out;
}

void DisplayLimitsMonitor::shutdown() {
    if (!impl_) return;
    impl_->clear_pending_info();
    impl_->clear_pending_desc();
    if (impl_->feedback) {
        wp_color_management_surface_feedback_v1_destroy(impl_->feedback);
        impl_->feedback = nullptr;
    }
    if (impl_->color_manager) {
        wp_color_manager_v1_destroy(impl_->color_manager);
        impl_->color_manager = nullptr;
    }
    if (impl_->registry) {
        wl_registry_destroy(impl_->registry);
        impl_->registry = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
}

namespace {

std::string find_executable_in_path(const char* name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return "";
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string dirs(path_env);
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t sep = dirs.find(':', start);
        std::string dir = (sep == std::string::npos) ? dirs.substr(start) : dirs.substr(start, sep - start);
        if (!dir.empty()) {
            fs::path cand = fs::path(dir) / name;
            if (fs::exists(cand, ec) && !fs::is_directory(cand, ec) && access(cand.c_str(), X_OK) == 0) {
                return cand.string();
            }
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return "";
}

std::string find_monique_helper() {
    namespace fs = std::filesystem;
    char exe_buf[4096];
    ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (len > 0) {
        exe_buf[len] = '\0';
        fs::path exe_dir = fs::path(exe_buf).parent_path();
        std::error_code ec;
        for (const fs::path& cand : {
                 exe_dir / "monique-set-cm",
                 exe_dir / ".." / "bin" / "monique-set-cm",
                 exe_dir / ".." / ".." / "bin" / "monique-set-cm",
             }) {
            if (fs::exists(cand, ec) && access(cand.c_str(), X_OK) == 0) {
                return cand.string();
            }
        }
    }
    std::string in_path = find_executable_in_path("monique-set-cm");
    if (!in_path.empty()) return in_path;
    for (const char* sys_cand : {"/usr/bin/monique-set-cm", "/usr/local/bin/monique-set-cm"}) {
        if (access(sys_cand, X_OK) == 0) return sys_cand;
    }
    return "";
}

void append_edid_args(std::vector<std::string>& args, const std::string& monitor_name, const EdidHdrSpecs& edid) {
    if (!monitor_name.empty()) {
        args.push_back("--monitor");
        args.push_back(monitor_name);
    }
    if (edid.has_min) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4f", edid.min_nits);
        args.push_back("--min-nits");
        args.push_back(buf);
    }
    if (edid.has_avg && edid.avg_nits > 0.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(edid.avg_nits)));
        args.push_back("--avg-nits");
        args.push_back(buf);
    }
    if (edid.has_max && edid.max_nits > 0.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(edid.max_nits)));
        args.push_back("--max-nits");
        args.push_back(buf);
    }
}

bool run_monique_helper(const std::vector<std::string>& extra_args,
                        std::string& out_stdout,
                        int& out_exit_code) {
    out_stdout.clear();
    out_exit_code = -1;
    std::string helper = find_monique_helper();
    if (helper.empty()) {
        return false;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return false;
    }

    std::vector<std::string> args;
    args.push_back(helper);
    args.insert(args.end(), extra_args.begin(), extra_args.end());

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execv(helper.c_str(), argv.data());
        _exit(127);
    }

    close(pipefd[1]);
    char buf[512];
    while (ssize_t n = read(pipefd[0], buf, sizeof(buf))) {
        if (n > 0) out_stdout.append(buf, static_cast<size_t>(n));
        else break;
    }
    close(pipefd[0]);
    while (!out_stdout.empty() && (out_stdout.back() == '\n' || out_stdout.back() == '\r')) {
        out_stdout.pop_back();
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) {
        return false;
    }
    out_exit_code = WEXITSTATUS(status);
    return out_exit_code == 0;
}

}  // namespace

bool apply_system_cm_monique(const std::string& cm_preset,
                             const std::string& monitor_name,
                             const EdidHdrSpecs& edid,
                             std::string& out_status,
                             const std::string& icc_path) {
    std::vector<std::string> args = {cm_preset};
    if (!icc_path.empty()) {
        args.push_back("--icc");
        args.push_back(icc_path);
    }
    append_edid_args(args, monitor_name, edid);

    std::string stdout_msg;
    int code = -1;
    if (!run_monique_helper(args, stdout_msg, code)) {
        out_status = (code < 0)
            ? "Helper monique-set-cm introuvable"
            : ("Échec Monique/Hyprland (cm=" + cm_preset + ", code=" + std::to_string(code) + ")");
        return false;
    }
    out_status = icc_path.empty()
        ? ("Système modifié via Monique : cm=" + cm_preset)
        : ("Système modifié via Monique : ICC " + std::filesystem::path(icc_path).filename().string());
    return true;
}

bool sync_monique_profiles(const std::string& monitor_name,
                           const std::string& monitor_description,
                           const std::string& active_cm,
                           const std::string& active_icc,
                           const EdidHdrSpecs& edid,
                           std::string& out_status) {
    if (!ensure_startup_symlinks(edid, monitor_name, monitor_description)) {
        out_status = "Échec lors de la génération des profils ICC / liens symboliques";
        return false;
    }
    GeneratedIccSet icc = generate_calibrated_icc_profiles(edid, monitor_name, "", monitor_description);
    if (!icc.ok) {
        out_status = icc.error_msg;
        return false;
    }

    std::vector<std::string> args = {"--sync-profiles"};
    if (edid.hdr_capable) args.push_back("--hdr-capable");
    if (edid.supports_p3 || edid.hdr_capable) args.push_back("--supports-p3");
    if (!active_cm.empty()) {
        args.push_back("--active-cm");
        args.push_back(active_cm);
    }
    if (!active_icc.empty()) {
        args.push_back("--active-icc");
        args.push_back(active_icc);
    }
    args.push_back("--icc-native");
    args.push_back(icc.native_icc_path);
    args.push_back("--icc-p3");
    args.push_back(icc.p3_icc_path);
    args.push_back("--icc-srgb");
    args.push_back(icc.srgb_icc_path);
    args.push_back("--icc-bt2020-linear");
    args.push_back(icc.bt2020_linear_icc_path);
    args.push_back("--icc-bt2020-pq");
    args.push_back(icc.bt2020_pq_icc_path);
    append_edid_args(args, monitor_name, edid);

    std::string stdout_msg;
    int code = -1;
    if (!run_monique_helper(args, stdout_msg, code)) {
        out_status = (code < 0)
            ? "Helper monique-set-cm introuvable"
            : ("Échec synchro profils Monique (code=" + std::to_string(code) + ")");
        return false;
    }
    out_status = "ICC + Profils Monique remplacés : " + (stdout_msg.empty() ? "OK" : stdout_msg);
    return true;
}

bool read_sysfs_backlight(int& out_percent) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base("/sys/class/backlight");
    if (!fs::exists(base, ec)) return false;
    for (const auto& entry : fs::directory_iterator(base, ec)) {
        std::ifstream f_cur(entry.path() / "brightness");
        std::ifstream f_max(entry.path() / "max_brightness");
        long long cur = -1, mx = 0;
        if (f_cur >> cur && f_max >> mx && mx > 0 && cur >= 0) {
            out_percent = static_cast<int>(std::clamp(
                std::llround((static_cast<double>(cur) / static_cast<double>(mx)) * 100.0),
                0LL, 100LL));
            return true;
        }
    }
    return false;
}

bool set_backlight_percent(int target_percent, int& out_actual_percent, std::string& out_status) {
    int clamped = std::clamp(target_percent, 5, 100);
    std::string val = std::to_string(clamped) + "%";

    pid_t pid = fork();
    if (pid < 0) {
        out_status = "Impossible de lancer brightnessctl";
        return false;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        const char* argv[] = {"brightnessctl", "-q", "set", val.c_str(), nullptr};
        execvp("brightnessctl", const_cast<char* const*>(argv));
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        out_status = "Échec du réglage luminosité SDR (brightnessctl requis)";
        return false;
    }
    if (!read_sysfs_backlight(out_actual_percent)) {
        out_actual_percent = clamped;
    }
    out_status = "Luminosité dalle SDR : " + std::to_string(out_actual_percent) + "%";
    return true;
}

bool parse_os_release_is_omarchy(const std::string& os_release_text) {
    size_t start = 0;
    while (start < os_release_text.size()) {
        size_t end = os_release_text.find('\n', start);
        std::string line = (end == std::string::npos)
            ? os_release_text.substr(start)
            : os_release_text.substr(start, end - start);
        auto unquote = [](std::string val) {
            if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                                    (val.front() == '\'' && val.back() == '\''))) {
                return val.substr(1, val.size() - 2);
            }
            return val;
        };
        if (line.rfind("ID=", 0) == 0) {
            std::string val = unquote(line.substr(3));
            for (char& c : val) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (val == "omarchy") return true;
        } else if (line.rfind("NAME=", 0) == 0) {
            std::string val = unquote(line.substr(5));
            for (char& c : val) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (val == "omarchy") return true;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

bool is_omarchy_system() {
    std::ifstream f("/etc/os-release");
    if (f) {
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (parse_os_release_is_omarchy(content)) return true;
    }
    std::error_code ec;
    if (std::filesystem::exists("/usr/share/omarchy/bin/omarchy-pkg-aur-add", ec)) {
        return true;
    }
    return false;
}

bool is_monique_integrated() {
    if (find_executable_in_path("monique").empty() && access("/usr/bin/monique", X_OK) != 0) {
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        const char* argv[] = {
            "python3", "-c",
            "import importlib.util, sys; sys.exit(0 if importlib.util.find_spec('monique') else 1)",
            nullptr,
        };
        execvp("python3", const_cast<char* const*>(argv));
        _exit(127);
    }
    int status = 0;
    return waitpid(pid, &status, 0) >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool install_monique_on_omarchy(bool run_in_terminal, std::string& out_status) {
    if (!is_omarchy_system()) {
        out_status = "Installation automatique de Monique réservée à Omarchy.";
        return false;
    }

    auto run_argv = [](const std::vector<std::string>& args) -> int {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid == 0) {
            execvp(argv[0], argv.data());
            _exit(127);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    };

    if (!run_in_terminal) {
        int rc = -1;
        if (access("/usr/share/omarchy/bin/omarchy-pkg-aur-add", X_OK) == 0) {
            rc = run_argv({"/usr/share/omarchy/bin/omarchy-pkg-aur-add", "monique"});
        } else if (!find_executable_in_path("yay").empty()) {
            rc = run_argv({"yay", "-S", "--noconfirm", "--needed", "monique"});
        } else if (!find_executable_in_path("paru").empty()) {
            rc = run_argv({"paru", "-S", "--noconfirm", "--needed", "monique"});
        } else {
            out_status = "Aucun assistant AUR (omarchy-pkg-aur-add/yay/paru) trouvé.";
            return false;
        }
        if (rc == 0 && is_monique_integrated()) {
            out_status = "Monique installé avec succès sur Omarchy.";
            return true;
        }
        out_status = "Échec de l'installation de Monique (code=" + std::to_string(rc) + ").";
        return false;
    }

    const char* install_script =
        "echo '[TEST_HDR] Installation de Monique sur Omarchy...'; "
        "if [ -x /usr/share/omarchy/bin/omarchy-pkg-aur-add ]; then "
        "/usr/share/omarchy/bin/omarchy-pkg-aur-add monique; "
        "elif command -v yay >/dev/null 2>&1; then yay -S --needed monique; "
        "elif command -v paru >/dev/null 2>&1; then paru -S --needed monique; "
        "else echo 'yay/paru/omarchy-pkg-aur-add introuvable'; exit 1; fi; "
        "rc=$?; echo ''; read -r -p 'Appuyez sur Entree pour fermer...' _; exit $rc";

    std::vector<std::string> term_args;
    std::string term = find_executable_in_path("xdg-terminal-exec");
    if (!term.empty()) {
        term_args = {term, "bash", "-lc", install_script};
    } else {
        for (const char* cand : {"ghostty", "alacritty", "kitty", "foot"}) {
            term = find_executable_in_path(cand);
            if (!term.empty()) {
                term_args = {term, "-e", "bash", "-lc", install_script};
                break;
            }
        }
    }
    if (term_args.empty()) {
        out_status = "Aucun terminal trouvé pour installer Monique (exécutez : omarchy-pkg-aur-add monique).";
        return false;
    }

    int rc = run_argv(term_args);
    if (is_monique_integrated()) {
        out_status = "Monique installé et détecté sur Omarchy.";
        return true;
    }
    out_status = "Installation de Monique annulée ou incomplète (code=" + std::to_string(rc) + ").";
    return false;
}

}  // namespace testhdr
