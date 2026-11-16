#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>

#include "display_limits.h"
#include "edid.h"
#include "fullscreen_vert_spv.h"
#include "gradient_frag_spv.h"
#include "icc_gen.h"
#include "nit_math.h"
#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace testhdr {
namespace {

struct alignas(16) ShaderParams {
    float width;
    float height;
    float min_nits;
    float max_nits;
    int32_t scale_mode;
    int32_t view_mode;
    int32_t channel_mode;
    int32_t hdr_output_mode;
    int32_t band_count;
    float hud_nits;
    float sdr_preview_max_nits;
    int32_t test_pattern;
    float cie_x0;
    float cie_y0;
    float cie_w;
    float cie_h;
    int32_t active_gamut_id;
    float peak_nits;
    float edid_rx;
    float edid_ry;
    float edid_gx;
    float edid_gy;
    float edid_bx;
    float edid_by;
};

static_assert(sizeof(ShaderParams) == 96, "ShaderParams must match std140 layout");

const char* swapchain_comp_name(SDL_GPUSwapchainComposition c) {
    switch (c) {
        case SDL_GPU_SWAPCHAINCOMPOSITION_SDR: return "SDR";
        case SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR: return "SDR_LINEAR";
        case SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR: return "HDR_EXTENDED_LINEAR";
        case SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084: return "HDR10_ST2084";
    }
    return "UNKNOWN";
}

SDL_GPUSwapchainComposition to_sdl_composition(HdrOutputMode mode) {
    switch (mode) {
        case HdrOutputMode::Hdr10Pq:
            return SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084;
        case HdrOutputMode::HdrExtendedLinear:
        case HdrOutputMode::Sdr10:
            return SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR;
        case HdrOutputMode::Sdr:
        default:
            return SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    }
}

bool mode_supported(const OverlayState& st, HdrOutputMode mode) {
    switch (mode) {
        case HdrOutputMode::Hdr10Pq: return st.supports_hdr10;
        case HdrOutputMode::HdrExtendedLinear: return st.supports_scrgb;
        case HdrOutputMode::Sdr10: return st.supports_sdr10;
        case HdrOutputMode::Sdr: return true;
    }
    return false;
}

double step_value_up(double current, const std::vector<double>& stops) {
    for (double v : stops) {
        if (v > current + 1e-6) return v;
    }
    return stops.back();
}

double step_value_down(double current, const std::vector<double>& stops) {
    for (auto it = stops.rbegin(); it != stops.rend(); ++it) {
        if (*it < current - 1e-6) return *it;
    }
    return stops.front();
}

struct App {
    SDL_Window* window = nullptr;
    SDL_GPUDevice* device = nullptr;
    SDL_GPUShader* vert_shader = nullptr;
    SDL_GPUShader* frag_shader = nullptr;
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUTextureFormat pipeline_format = SDL_GPU_TEXTUREFORMAT_INVALID;

    SDL_GPUTexture* overlay_tex = nullptr;
    SDL_GPUSampler* overlay_sampler = nullptr;
    SDL_GPUTransferBuffer* transfer_buf = nullptr;
    int tex_w = 0;
    int tex_h = 0;

    OverlayState state;
    DisplayLimitsMonitor limits;
    std::string preferred_connector;
    std::vector<uint8_t> overlay_bgra;
    bool overlay_dirty = true;
    bool fullscreen = true;
    bool cursor_locked = false;
    int zoom_preset = 0;
    ViewMode prev_non_cie_view = ViewMode::Split;

    void refresh_hdr_capabilities() {
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        state.window_hdr_enabled = SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
        state.window_hdr_headroom = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 0.0f);
        state.window_sdr_white_level = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
        state.supports_hdr10 = SDL_WindowSupportsGPUSwapchainComposition(
            device, window, SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084);
        state.supports_scrgb = SDL_WindowSupportsGPUSwapchainComposition(
            device, window, SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR);
        state.supports_sdr10 = state.supports_scrgb;
        limits.poll_hyprland(preferred_connector.empty() ? state.edid.connector : preferred_connector);
        state.mode_limits = limits.resolve(
            state.window_hdr_enabled, state.window_hdr_headroom, state.window_sdr_white_level, state.edid);
    }

    bool is_system_hdr_preset() const {
        return state.mode_limits.hypr_cm_preset == "hdr" ||
               state.mode_limits.hypr_cm_preset == "hdredid";
    }

    void switch_system_cm(const std::string& cm_preset,
                          HdrOutputMode target_swapchain,
                          const std::string& icc_path = "") {
        std::string mon = !preferred_connector.empty()
            ? preferred_connector
            : (!state.mode_limits.hypr_monitor.empty() ? state.mode_limits.hypr_monitor : state.edid.connector);
        apply_system_cm_monique(cm_preset, mon, state.edid, state.system_status_msg, icc_path);
        SDL_Delay(150);
        SDL_PumpEvents();
        refresh_hdr_capabilities();
        apply_hdr_output_mode(target_swapchain);
        overlay_dirty = true;
    }

    bool apply_hdr_output_mode(HdrOutputMode requested) {
        refresh_hdr_capabilities();
        HdrOutputMode chosen = requested;
        if (!mode_supported(state, chosen)) {
            if (state.supports_hdr10) {
                chosen = HdrOutputMode::Hdr10Pq;
            } else if (state.supports_scrgb) {
                chosen = HdrOutputMode::HdrExtendedLinear;
            } else {
                chosen = HdrOutputMode::Sdr;
            }
        }

        SDL_GPUSwapchainComposition comp = to_sdl_composition(chosen);
        SDL_GPUPresentMode pmode = SDL_GPU_PRESENTMODE_VSYNC;
        if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX)) {
            pmode = SDL_GPU_PRESENTMODE_MAILBOX;
        }
        if (!SDL_SetGPUSwapchainParameters(device, window, comp, pmode)) {
            SDL_Log("SDL_SetGPUSwapchainParameters(%s) failed: %s",
                    swapchain_comp_name(comp), SDL_GetError());
            chosen = HdrOutputMode::Sdr;
            SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
        }

        state.hdr_output_mode = chosen;
        overlay_dirty = true;
        return ensure_pipeline();
    }

    bool ensure_pipeline() {
        SDL_GPUTextureFormat fmt = SDL_GetGPUSwapchainTextureFormat(device, window);
        if (fmt == SDL_GPU_TEXTUREFORMAT_INVALID) {
            SDL_Log("Invalid swapchain texture format");
            return false;
        }
        if (pipeline && pipeline_format == fmt) {
            return true;
        }
        if (pipeline) {
            SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
            pipeline = nullptr;
        }

        SDL_GPUColorTargetDescription target_desc{};
        target_desc.format = fmt;

        SDL_GPUGraphicsPipelineCreateInfo pipe_info{};
        pipe_info.vertex_shader = vert_shader;
        pipe_info.fragment_shader = frag_shader;
        pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipe_info.target_info.num_color_targets = 1;
        pipe_info.target_info.color_target_descriptions = &target_desc;

        pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipe_info);
        if (!pipeline) {
            SDL_Log("SDL_CreateGPUGraphicsPipeline failed: %s", SDL_GetError());
            return false;
        }
        pipeline_format = fmt;
        return true;
    }

    bool ensure_overlay_resources(int w, int h) {
        if (w <= 0 || h <= 0) return false;
        if (overlay_tex && tex_w == w && tex_h == h) return true;

        if (overlay_tex) {
            SDL_ReleaseGPUTexture(device, overlay_tex);
            overlay_tex = nullptr;
        }
        if (transfer_buf) {
            SDL_ReleaseGPUTransferBuffer(device, transfer_buf);
            transfer_buf = nullptr;
        }

        SDL_GPUTextureCreateInfo tex_info{};
        tex_info.type = SDL_GPU_TEXTURETYPE_2D;
        tex_info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        tex_info.width = static_cast<Uint32>(w);
        tex_info.height = static_cast<Uint32>(h);
        tex_info.layer_count_or_depth = 1;
        tex_info.num_levels = 1;
        overlay_tex = SDL_CreateGPUTexture(device, &tex_info);
        if (!overlay_tex) {
            SDL_Log("SDL_CreateGPUTexture failed: %s", SDL_GetError());
            return false;
        }

        SDL_GPUTransferBufferCreateInfo tb_info{};
        tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb_info.size = static_cast<Uint32>(w) * static_cast<Uint32>(h) * 4u;
        transfer_buf = SDL_CreateGPUTransferBuffer(device, &tb_info);
        if (!transfer_buf) {
            SDL_Log("SDL_CreateGPUTransferBuffer failed: %s", SDL_GetError());
            return false;
        }

        tex_w = w;
        tex_h = h;
        overlay_dirty = true;
        return true;
    }

    void update_window_metrics() {
        int w_px = 0, h_px = 0;
        SDL_GetWindowSizeInPixels(window, &w_px, &h_px);
        int w_log = 0, h_log = 0;
        SDL_GetWindowSize(window, &w_log, &h_log);
        float scale = 1.0f;
        if (w_log > 0 && w_px > 0) {
            scale = static_cast<float>(w_px) / static_cast<float>(w_log);
        }
        float disp_scale = SDL_GetWindowDisplayScale(window);
        if (disp_scale > scale) {
            scale = disp_scale;
        }
        scale = std::clamp(scale, 1.0f, 3.0f);

        if (w_px != state.width_px || h_px != state.height_px || std::abs(scale - state.ui_scale) > 0.01f) {
            state.width_px = std::max(1, w_px);
            state.height_px = std::max(1, h_px);
            state.ui_scale = scale;
            overlay_dirty = true;
        }
    }

    void apply_zoom_preset(int preset_idx) {
        zoom_preset = ((preset_idx % 6) + 6) % 6;
        switch (zoom_preset) {
            case 0: {
                state.min_nits = 0.0;
                double target = 1000.0;
                if (state.edid.has_max && state.edid.max_nits > 750.0) {
                    target = std::ceil(state.edid.max_nits * 1.4 / 100.0) * 100.0;
                }
                state.max_nits = std::clamp(target, 400.0, 10000.0);
                break;
            }
            case 1:
                state.min_nits = 200.0;
                state.max_nits = std::max(1000.0, state.edid.has_max ? std::ceil(state.edid.max_nits * 1.4 / 100.0) * 100.0 : 1000.0);
                break;
            case 2:
                state.min_nits = 0.0;
                state.max_nits = 5.0;
                break;
            case 3:
                if (state.edid.hdr_capable && state.edid.has_max) {
                    state.min_nits = state.edid.has_min ? state.edid.min_nits : 0.0;
                    state.max_nits = state.edid.max_nits;
                } else {
                    state.min_nits = 0.0;
                    state.max_nits = 600.0;
                }
                break;
            case 4:
                state.min_nits = 0.0;
                state.max_nits = 4000.0;
                break;
            case 5:
                state.min_nits = 0.0;
                state.max_nits = 10000.0;
                break;
        }
        state.zoom_preset = zoom_preset;
        overlay_dirty = true;
    }

    void reset_defaults(double initial_max_nits) {
        state.scale_mode = ScaleMode::PQ;
        state.view_mode = ViewMode::Split;
        state.test_pattern = TestPattern::GamutBars;
        state.channel_mode = ChannelMode::White;
        state.band_count = 20;
        state.fullscreen = fullscreen;
        state.show_hud = true;
        state.menu_visible = true;
        state.menu_selected = 0;
        state.show_edid_markers = true;
        state.hud_nits = 80.0f;
        prev_non_cie_view = ViewMode::Split;
        cursor_locked = false;
        if (initial_max_nits > 0.0) {
            state.min_nits = 0.0;
            state.max_nits = std::clamp(initial_max_nits, 1.0, 10000.0);
        } else {
            apply_zoom_preset(0);
        }
        overlay_dirty = true;
    }

    void cycle_output_mode(int dir) {
        int cur = static_cast<int>(state.hdr_output_mode);
        int disp_bpc = parse_format_bpc(state.mode_limits.hypr_format, 8);
        int step_sign = (dir < 0) ? -1 : 1;
        for (int step = 1; step <= 4; ++step) {
            int raw = (cur + step_sign * step) % 4;
            if (raw < 0) raw += 4;
            auto cand = static_cast<HdrOutputMode>(raw);
            if (cand == HdrOutputMode::Sdr) {
                switch_system_cm("srgb", HdrOutputMode::Sdr);
                break;
            }
            if (cand == HdrOutputMode::Sdr10) {
                if (!is_system_hdr_preset() && disp_bpc >= 10) {
                    if (mode_supported(state, cand)) {
                        apply_hdr_output_mode(cand);
                        break;
                    }
                } else {
                    std::string sdr10_cm = (state.edid.supports_p3 || state.edid.hdr_capable)
                        ? "dcip3"
                        : "srgb10";
                    switch_system_cm(sdr10_cm, HdrOutputMode::Sdr10);
                    break;
                }
            }
            if ((cand == HdrOutputMode::Hdr10Pq || cand == HdrOutputMode::HdrExtendedLinear) &&
                !is_system_hdr_preset()) {
                switch_system_cm("hdr", cand);
                break;
            }
            if (mode_supported(state, cand)) {
                apply_hdr_output_mode(cand);
                break;
            }
        }
    }

    void cycle_preset(int dir) {
        static const char* kPresets[] = {"hdr", "hdredid", "icc", "dcip3", "srgb10", "srgb"};
        int cur_idx = 0;
        int disp_bpc = parse_format_bpc(state.mode_limits.hypr_format, 8);
        if (!state.mode_limits.active_icc_path.empty()) {
            cur_idx = 2;
        } else if (state.mode_limits.hypr_cm_preset == "hdr") {
            cur_idx = 0;
        } else if (state.mode_limits.hypr_cm_preset == "hdredid") {
            cur_idx = 1;
        } else if (state.mode_limits.hypr_cm_preset == "dcip3" ||
                   state.mode_limits.hypr_cm_preset == "wide") {
            cur_idx = 3;
        } else if (state.mode_limits.hypr_cm_preset == "srgb") {
            cur_idx = (disp_bpc >= 10) ? 4 : 5;
        }
        int next_idx = ((cur_idx + ((dir < 0) ? -1 : 1)) % 6 + 6) % 6;
        std::string next_preset = kPresets[next_idx];
        if (next_preset == "icc") {
            std::string mon = !preferred_connector.empty()
                ? preferred_connector
                : (!state.mode_limits.hypr_monitor.empty()
                       ? state.mode_limits.hypr_monitor
                       : state.edid.connector);
            GeneratedIccSet icc = generate_calibrated_icc_profiles(
                state.edid, mon, "", state.mode_limits.hypr_description);
            if (icc.ok) {
                switch_system_cm("icc", HdrOutputMode::Sdr10, icc.native_icc_path);
            } else {
                state.system_status_msg = icc.error_msg;
                overlay_dirty = true;
            }
        } else {
            HdrOutputMode target_mode = (next_preset == "hdr" || next_preset == "hdredid")
                ? HdrOutputMode::Hdr10Pq
                : (next_preset == "srgb" ? HdrOutputMode::Sdr : HdrOutputMode::Sdr10);
            switch_system_cm(next_preset, target_mode);
        }
    }

    void adjust_sdr_brightness(int delta_pct) {
        bool hdr_active = state.window_hdr_enabled &&
                          state.hdr_output_mode != HdrOutputMode::Sdr &&
                          state.hdr_output_mode != HdrOutputMode::Sdr10;
        if (hdr_active) {
            state.system_status_msg = "Luminosité (+/-) modifiable uniquement en mode SDR";
            overlay_dirty = true;
            return;
        }
        if (!state.mode_limits.backlight_available) {
            state.system_status_msg = "Rétroéclairage matériel non détecté (/sys/class/backlight)";
            overlay_dirty = true;
            return;
        }
        int target = state.mode_limits.backlight_percent + delta_pct;
        int actual = state.mode_limits.backlight_percent;
        set_backlight_percent(target, actual, state.system_status_msg);
        refresh_hdr_capabilities();
        overlay_dirty = true;
    }

    static void launch_monique_detached() {
        pid_t pid = fork();
        if (pid < 0) return;
        if (pid == 0) {
            pid_t pid2 = fork();
            if (pid2 == 0) {
                setsid();
                int devnull = open("/dev/null", O_RDWR);
                if (devnull >= 0) {
                    dup2(devnull, STDIN_FILENO);
                    dup2(devnull, STDOUT_FILENO);
                    dup2(devnull, STDERR_FILENO);
                    close(devnull);
                }
                const char* argv[] = {"monique", nullptr};
                execvp("monique", const_cast<char* const*>(argv));
                _exit(127);
            }
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
    }

    void export_to_monique_and_open(bool& running) {
        std::string mon = !preferred_connector.empty()
            ? preferred_connector
            : (!state.mode_limits.hypr_monitor.empty()
                   ? state.mode_limits.hypr_monitor
                   : state.edid.connector);

        std::string active_cm;
        std::string active_icc;
        if (state.hdr_output_mode == HdrOutputMode::Sdr) {
            if (!state.mode_limits.active_icc_path.empty() &&
                state.mode_limits.active_icc_path.find("sRGB") != std::string::npos) {
                active_icc = state.mode_limits.active_icc_path;
            } else {
                active_cm = "srgb";
            }
        } else if (state.hdr_output_mode == HdrOutputMode::Sdr10) {
            if (!state.mode_limits.active_icc_path.empty()) {
                active_icc = state.mode_limits.active_icc_path;
            } else if (state.mode_limits.hypr_cm_preset == "dcip3" ||
                       state.mode_limits.hypr_cm_preset == "wide") {
                active_cm = "dcip3";
            } else {
                active_cm = "srgb10";
            }
        } else {
            if (!state.mode_limits.active_icc_path.empty()) {
                active_icc = state.mode_limits.active_icc_path;
            }
            active_cm = (state.mode_limits.hypr_cm_preset == "hdredid") ? "hdredid" : "hdr";
        }

        if (sync_monique_profiles(mon, state.mode_limits.hypr_description,
                                  active_cm, active_icc, state.edid, state.system_status_msg)) {
            launch_monique_detached();
            running = false;
        } else {
            overlay_dirty = true;
        }
    }

    void render_frame() {
        update_window_metrics();
        if (!ensure_overlay_resources(state.width_px, state.height_px)) return;
        if (!ensure_pipeline()) return;

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
        if (!cmd) return;

        SDL_GPUTexture* swapchain_tex = nullptr;
        Uint32 sw_w = 0, sw_h = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain_tex, &sw_w, &sw_h) || !swapchain_tex) {
            SDL_SubmitGPUCommandBuffer(cmd);
            return;
        }

        if (static_cast<int>(sw_w) != state.width_px || static_cast<int>(sw_h) != state.height_px) {
            state.width_px = static_cast<int>(sw_w);
            state.height_px = static_cast<int>(sw_h);
            ensure_overlay_resources(state.width_px, state.height_px);
            overlay_dirty = true;
        }

        if (overlay_dirty) {
            std::string title = "Test HDR — " + build_live_header_title(state);
            SDL_SetWindowTitle(window, title.c_str());
            render_overlay(state, overlay_bgra);
            void* mapped = SDL_MapGPUTransferBuffer(device, transfer_buf, true);
            if (mapped) {
                std::memcpy(mapped, overlay_bgra.data(), overlay_bgra.size());
                SDL_UnmapGPUTransferBuffer(device, transfer_buf);

                SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(cmd);
                SDL_GPUTextureTransferInfo src{};
                src.transfer_buffer = transfer_buf;
                src.offset = 0;
                src.pixels_per_row = static_cast<Uint32>(state.width_px);
                src.rows_per_layer = static_cast<Uint32>(state.height_px);

                SDL_GPUTextureRegion dst{};
                dst.texture = overlay_tex;
                dst.w = static_cast<Uint32>(state.width_px);
                dst.h = static_cast<Uint32>(state.height_px);
                dst.d = 1;
                SDL_UploadToGPUTexture(copy_pass, &src, &dst, true);
                SDL_EndGPUCopyPass(copy_pass);
            }
            overlay_dirty = false;
        }

        SDL_GPUColorTargetInfo color_target{};
        color_target.texture = swapchain_tex;
        color_target.load_op = SDL_GPU_LOADOP_DONT_CARE;
        color_target.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(render_pass, pipeline);

        SDL_GPUTextureSamplerBinding sampler_binding{};
        sampler_binding.texture = overlay_tex;
        sampler_binding.sampler = overlay_sampler;
        SDL_BindGPUFragmentSamplers(render_pass, 0, &sampler_binding, 1);

        ShaderParams params{};
        params.width = static_cast<float>(state.width_px);
        params.height = static_cast<float>(state.height_px);
        params.min_nits = static_cast<float>(state.min_nits);
        params.max_nits = static_cast<float>(state.max_nits);
        params.scale_mode = static_cast<int32_t>(state.scale_mode);
        params.view_mode = static_cast<int32_t>(state.view_mode);
        params.channel_mode = static_cast<int32_t>(state.channel_mode);
        params.hdr_output_mode = static_cast<int32_t>(state.hdr_output_mode);
        params.band_count = state.band_count;
        params.hud_nits = state.hud_nits;
        params.sdr_preview_max_nits = static_cast<float>(std::max(80.0, state.max_nits));
        params.test_pattern = static_cast<int32_t>(state.test_pattern);

        double cx = 0.0, cy = 0.0, csize = 0.0;
        compute_cie_plot_rect(state, cx, cy, csize);
        float w_f = static_cast<float>(std::max(1, state.width_px));
        float h_f = static_cast<float>(std::max(1, state.height_px));
        params.cie_x0 = static_cast<float>(cx) / w_f;
        params.cie_y0 = static_cast<float>(cy) / h_f;
        params.cie_w = static_cast<float>(csize) / w_f;
        params.cie_h = static_cast<float>(csize) / h_f;
        params.active_gamut_id = resolve_active_gamut_id(state);
        bool hdr_active = state.window_hdr_enabled &&
                          state.hdr_output_mode != HdrOutputMode::Sdr &&
                          state.hdr_output_mode != HdrOutputMode::Sdr10;
        params.peak_nits = hdr_active
            ? static_cast<float>(state.mode_limits.has_active_limit ? std::max(80.0, state.mode_limits.active_max_nits) : 400.0)
            : 80.0f;
        params.edid_rx = static_cast<float>(state.edid.red_x);
        params.edid_ry = static_cast<float>(state.edid.red_y);
        params.edid_gx = static_cast<float>(state.edid.green_x);
        params.edid_gy = static_cast<float>(state.edid.green_y);
        params.edid_bx = static_cast<float>(state.edid.blue_x);
        params.edid_by = static_cast<float>(state.edid.blue_y);
        SDL_PushGPUFragmentUniformData(cmd, 0, &params, sizeof(params));

        SDL_DrawGPUPrimitives(render_pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(render_pass);

        SDL_SubmitGPUCommandBuffer(cmd);
    }

    void cleanup() {
        limits.shutdown();
        if (device) {
            SDL_WaitForGPUIdle(device);
            if (pipeline) SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
            if (vert_shader) SDL_ReleaseGPUShader(device, vert_shader);
            if (frag_shader) SDL_ReleaseGPUShader(device, frag_shader);
            if (overlay_tex) SDL_ReleaseGPUTexture(device, overlay_tex);
            if (overlay_sampler) SDL_ReleaseGPUSampler(device, overlay_sampler);
            if (transfer_buf) SDL_ReleaseGPUTransferBuffer(device, transfer_buf);
            if (window) SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
        }
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

#ifndef TESTHDR_VERSION
#define TESTHDR_VERSION "0.5.0"
#endif

void print_usage(const char* argv0) {
    std::printf(
        "TEST_HDR %s — Testeur HDR / Wide Gamut / Triangle CIE 1931\n"
        "Usage: %s [options]\n"
        "  --windowed           Démarre en mode fenêtré (sinon plein écran)\n"
        "  --hdr                Active cm=hdr dans Hyprland via Monique au démarrage\n"
        "  --cie                Ouvre directement sur le Triangle CIE 1931\n"
        "  --view <vue>         Vue initiale : mixte, continu, bandes, rgbw, mires, cie\n"
        "  --sync-profiles      Génère les ICC (~/.local/share/icc/) et remplace les profils Monique puis quitte\n"
        "  --max-nits <valeur>  Définit la borne haute initiale en nits (ex: 617, 1000, 4000)\n"
        "  --connector <nom>    Force un connecteur DRM pour lire l'EDID (ex: eDP-1, DP-1)\n"
        "  --version / -v       Affiche la version\n"
        "  --help / -h          Affiche cette aide\n",
        TESTHDR_VERSION, argv0);
}

bool parse_view_arg(const std::string& v, ViewMode& out) {
    if (v == "mixte" || v == "split") { out = ViewMode::Split; return true; }
    if (v == "continu" || v == "smooth") { out = ViewMode::Smooth; return true; }
    if (v == "bandes" || v == "bands") { out = ViewMode::Bands; return true; }
    if (v == "rgbw") { out = ViewMode::Rgbw; return true; }
    if (v == "mires" || v == "patterns") { out = ViewMode::TestPattern; return true; }
    if (v == "cie") { out = ViewMode::CieTriangle; return true; }
    return false;
}

}  // namespace
}  // namespace testhdr

int main(int argc, char* argv[]) {
    using namespace testhdr;

    bool start_fullscreen = true;
    bool apply_hdr_on_start = false;
    bool sync_profiles_only = false;
    bool has_start_view = false;
    ViewMode start_view = ViewMode::Split;
    double initial_max_nits = -1.0;
    std::string preferred_connector;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--version" || arg == "-v") {
            std::printf("TEST_HDR %s\n", TESTHDR_VERSION);
            return 0;
        } else if (arg == "--windowed" || arg == "-w") {
            start_fullscreen = false;
        } else if (arg == "--hdr") {
            apply_hdr_on_start = true;
        } else if (arg == "--cie") {
            has_start_view = true;
            start_view = ViewMode::CieTriangle;
        } else if (arg == "--view" && i + 1 < argc) {
            if (parse_view_arg(argv[++i], start_view)) {
                has_start_view = true;
            }
        } else if (arg == "--sync-profiles") {
            sync_profiles_only = true;
        } else if (arg == "--max-nits" && i + 1 < argc) {
            initial_max_nits = std::atof(argv[++i]);
        } else if (arg == "--connector" && i + 1 < argc) {
            preferred_connector = argv[++i];
        }
    }

    bool monique_ready = is_monique_integrated();
    const bool on_omarchy = is_omarchy_system();
    std::string monique_startup_msg;
    bool prompt_monique_gui = false;

    if (!monique_ready) {
        if (!on_omarchy) {
            monique_startup_msg = "Monique non détecté (installation auto proposée uniquement sous Omarchy).";
            std::fprintf(stderr, "[TEST_HDR] %s\n", monique_startup_msg.c_str());
        } else if (isatty(STDIN_FILENO)) {
            std::printf("[TEST_HDR] Monique n'est pas installé sur ce système Omarchy.\n");
            std::printf("Voulez-vous installer Monique maintenant (omarchy-pkg-aur-add / yay) ? [O/n] ");
            std::fflush(stdout);
            std::string answer;
            std::getline(std::cin, answer);
            bool yes = answer.empty() || answer[0] == 'o' || answer[0] == 'O' || answer[0] == 'y' || answer[0] == 'Y';
            if (yes) {
                monique_ready = install_monique_on_omarchy(false, monique_startup_msg);
                std::printf("[TEST_HDR] %s\n", monique_startup_msg.c_str());
            } else {
                monique_startup_msg = "Monique ignoré au démarrage (export/changement de profil désactivé).";
            }
        } else if (!sync_profiles_only) {
            prompt_monique_gui = true;
        }
    }

    if (sync_profiles_only) {
        if (!monique_ready) {
            std::fprintf(stderr, "[TEST_HDR] Impossible de synchroniser les profils : Monique non installé.\n");
            return 1;
        }
        EdidHdrSpecs edid = probe_drm_edid(preferred_connector);
        DisplayLimitsMonitor tmp_lim;
        tmp_lim.poll_hyprland(preferred_connector.empty() ? edid.connector : preferred_connector);
        ModeLimitInfo info = tmp_lim.resolve(edid.hdr_capable, 1.0f, 1.0f, edid);
        std::string mon = !preferred_connector.empty()
            ? preferred_connector
            : (!info.hypr_monitor.empty() ? info.hypr_monitor : edid.connector);
        std::string active_icc = detect_active_hypr_icc(mon);
        std::string active_cm = !info.hypr_cm_preset.empty()
            ? info.hypr_cm_preset
            : (edid.hdr_capable ? "hdr" : "srgb");
        std::string status;
        bool ok = sync_monique_profiles(mon, info.hypr_description, active_cm, active_icc, edid, status);
        std::printf("%s\n", status.c_str());
        return ok ? 0 : 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    if (prompt_monique_gui && !monique_ready && on_omarchy) {
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Quitter"},
            {0, 0, "Continuer sans Monique"},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Installer Monique"},
        };
        SDL_MessageBoxData mb{};
        mb.flags = SDL_MESSAGEBOX_INFORMATION;
        mb.title = "TEST_HDR — Intégration Monique (Omarchy)";
        mb.message =
            "Monique n'est pas installé sur ce système Omarchy.\n"
            "Monique est requis pour appliquer les profils HDR/ICC et exporter depuis TEST_HDR.\n\n"
            "Souhaitez-vous installer Monique maintenant ?";
        mb.numbuttons = 3;
        mb.buttons = buttons;
        int btn = 0;
        if (SDL_ShowMessageBox(&mb, &btn)) {
            if (btn == 2) {
                SDL_Quit();
                return 0;
            }
            if (btn == 1) {
                monique_ready = install_monique_on_omarchy(true, monique_startup_msg);
            } else {
                monique_startup_msg = "Monique non installé — export/changement de profil désactivé.";
            }
        }
    }

    App app;
    app.fullscreen = start_fullscreen;
    app.preferred_connector = preferred_connector;
    app.state.edid = probe_drm_edid(preferred_connector);
    app.limits.poll_hyprland(preferred_connector.empty() ? app.state.edid.connector : preferred_connector);
    {
        ModeLimitInfo pre = app.limits.resolve(false, 1.0f, 1.0f, app.state.edid);
        ensure_startup_symlinks(app.state.edid, preferred_connector, pre.hypr_description);
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (start_fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }

    app.window = SDL_CreateWindow("Test HDR — Dégradé & Nuances en Nits", 1600, 900, flags);
    if (!app.window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        app.cleanup();
        return 1;
    }

    app.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    if (!app.device) {
        SDL_Log("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        app.cleanup();
        return 1;
    }

    if (!SDL_ClaimWindowForGPUDevice(app.device, app.window)) {
        SDL_Log("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        app.cleanup();
        return 1;
    }

    SDL_GPUShaderCreateInfo vs_info{};
    vs_info.code = fullscreen_vert_spv;
    vs_info.code_size = sizeof(fullscreen_vert_spv);
    vs_info.entrypoint = "main";
    vs_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    app.vert_shader = SDL_CreateGPUShader(app.device, &vs_info);

    SDL_GPUShaderCreateInfo fs_info{};
    fs_info.code = gradient_frag_spv;
    fs_info.code_size = sizeof(gradient_frag_spv);
    fs_info.entrypoint = "main";
    fs_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fs_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fs_info.num_samplers = 1;
    fs_info.num_uniform_buffers = 1;
    app.frag_shader = SDL_CreateGPUShader(app.device, &fs_info);

    if (!app.vert_shader || !app.frag_shader) {
        SDL_Log("Failed to create GPU shaders: %s", SDL_GetError());
        app.cleanup();
        return 1;
    }

    SDL_GPUSamplerCreateInfo samp_info{};
    samp_info.min_filter = SDL_GPU_FILTER_LINEAR;
    samp_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    samp_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samp_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    app.overlay_sampler = SDL_CreateGPUSampler(app.device, &samp_info);
    if (!app.overlay_sampler) {
        SDL_Log("SDL_CreateGPUSampler failed: %s", SDL_GetError());
        app.cleanup();
        return 1;
    }

    {
        SDL_PropertiesID wprops = SDL_GetWindowProperties(app.window);
        auto* wl_dpy = static_cast<struct wl_display*>(
            SDL_GetPointerProperty(wprops, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
        auto* wl_surf = static_cast<struct wl_surface*>(
            SDL_GetPointerProperty(wprops, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
        if (wl_dpy && wl_surf) {
            app.limits.init_wayland(wl_dpy, wl_surf);
        }
    }

    app.reset_defaults(initial_max_nits);
    if (has_start_view) {
        app.state.view_mode = start_view;
    }
    if (!monique_startup_msg.empty()) {
        app.state.system_status_msg = monique_startup_msg;
    }
    app.refresh_hdr_capabilities();
    if (apply_hdr_on_start) {
        app.switch_system_cm("hdr", HdrOutputMode::Hdr10Pq);
    } else {
        int disp_bpc = parse_format_bpc(app.state.mode_limits.hypr_format, 8);
        HdrOutputMode start_mode = HdrOutputMode::Sdr;
        if (app.state.window_hdr_enabled || app.is_system_hdr_preset()) {
            start_mode = HdrOutputMode::Hdr10Pq;
        } else if (disp_bpc >= 10 && app.state.supports_sdr10) {
            start_mode = HdrOutputMode::Sdr10;
        }
        if (!app.apply_hdr_output_mode(start_mode)) {
            app.cleanup();
            return 1;
        }
    }

    std::vector<double> max_stops = {5.0, 20.0, 50.0, 100.0, 200.0, 400.0, 500.0, 600.0, 800.0, 1000.0, 1200.0, 1500.0, 2000.0, 4000.0, 6000.0, 10000.0};
    if (app.state.edid.has_max && app.state.edid.max_nits > 1.0) {
        max_stops.push_back(app.state.edid.max_nits);
        std::sort(max_stops.begin(), max_stops.end());
    }
    std::vector<double> min_stops = {0.0, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0, 50.0, 100.0, 200.0, 400.0};
    if (app.state.edid.has_min && app.state.edid.min_nits > 0.0) {
        min_stops.push_back(app.state.edid.min_nits);
        std::sort(min_stops.begin(), min_stops.end());
    }
    const std::vector<int> band_stops = {12, 20, 32, 64, 128};

    auto to_px_coords = [&](double log_x, double log_y) -> std::pair<double, double> {
        int w_log = 1, h_log = 1;
        SDL_GetWindowSize(app.window, &w_log, &h_log);
        double sx = static_cast<double>(app.state.width_px) / std::max(1, w_log);
        double sy = static_cast<double>(app.state.height_px) / std::max(1, h_log);
        return {log_x * sx, log_y * sy};
    };

    auto menu_hit_row = [&](double px, double py, double& out_mx, double& out_mw) -> int {
        if (!app.state.show_hud || !app.state.menu_visible) return -1;
        double mx = 0.0, my = 0.0, mw = 0.0, mh = 0.0, mhh = 0.0, mrh = 0.0;
        compute_hud_menu_rect(app.state, mx, my, mw, mh, mhh, mrh);
        out_mx = mx;
        out_mw = mw;
        double rows_top = my + mhh + 4.0 * app.state.ui_scale;
        if (px < mx || px > mx + mw || py < rows_top || py >= rows_top + kHudMenuItemCount * mrh) {
            return -1;
        }
        return std::clamp(static_cast<int>((py - rows_top) / mrh), 0, kHudMenuItemCount - 1);
    };

    bool running = true;

    auto activate_menu_item = [&](int item_idx, int dir) {
        auto item = static_cast<HudMenuItem>(item_idx);
        switch (item) {
            case HudMenuItem::View: {
                int step = (dir < 0) ? (kViewModeCount - 1) : 1;
                app.state.view_mode = static_cast<ViewMode>((static_cast<int>(app.state.view_mode) + step) % kViewModeCount);
                if (app.state.view_mode != ViewMode::CieTriangle) {
                    app.prev_non_cie_view = app.state.view_mode;
                }
                app.overlay_dirty = true;
                break;
            }
            case HudMenuItem::TestPattern: {
                if (app.state.view_mode != ViewMode::TestPattern) {
                    app.state.view_mode = ViewMode::TestPattern;
                    app.prev_non_cie_view = ViewMode::TestPattern;
                } else {
                    int step = (dir < 0) ? (kTestPatternCount - 1) : 1;
                    app.state.test_pattern = static_cast<TestPattern>(
                        (static_cast<int>(app.state.test_pattern) + step) % kTestPatternCount);
                }
                app.overlay_dirty = true;
                break;
            }
            case HudMenuItem::OutputMode:
                app.cycle_output_mode(dir < 0 ? -1 : 1);
                break;
            case HudMenuItem::HyprPreset:
                app.cycle_preset(dir < 0 ? -1 : 1);
                break;
            case HudMenuItem::SdrBrightness:
                app.adjust_sdr_brightness(dir < 0 ? -5 : 5);
                break;
            case HudMenuItem::Scale: {
                int step = (dir < 0) ? 2 : 1;
                app.state.scale_mode = static_cast<ScaleMode>((static_cast<int>(app.state.scale_mode) + step) % 3);
                app.overlay_dirty = true;
                break;
            }
            case HudMenuItem::Channel: {
                int step = (dir < 0) ? 3 : 1;
                app.state.channel_mode = static_cast<ChannelMode>((static_cast<int>(app.state.channel_mode) + step) % 4);
                app.overlay_dirty = true;
                break;
            }
            case HudMenuItem::BandCount: {
                auto it = std::find(band_stops.begin(), band_stops.end(), app.state.band_count);
                size_t cur = (it == band_stops.end()) ? 0 : static_cast<size_t>(it - band_stops.begin());
                size_t next = (dir < 0)
                    ? (cur + band_stops.size() - 1) % band_stops.size()
                    : (cur + 1) % band_stops.size();
                app.state.band_count = band_stops[next];
                app.overlay_dirty = true;
                break;
            }
            case HudMenuItem::ZoomPreset:
                app.apply_zoom_preset(app.zoom_preset + (dir < 0 ? -1 : 1));
                break;
            case HudMenuItem::MaxNits:
                if (dir < 0) {
                    double next_max = step_value_down(app.state.max_nits, max_stops);
                    if (next_max > app.state.min_nits + 0.1) app.state.max_nits = next_max;
                } else {
                    app.state.max_nits = step_value_up(app.state.max_nits, max_stops);
                }
                app.overlay_dirty = true;
                break;
            case HudMenuItem::MinNits:
                if (dir < 0) {
                    app.state.min_nits = step_value_down(app.state.min_nits, min_stops);
                } else {
                    double next_min = step_value_up(app.state.min_nits, min_stops);
                    if (next_min < app.state.max_nits - 0.1) app.state.min_nits = next_min;
                }
                app.overlay_dirty = true;
                break;
            case HudMenuItem::EdidMarkers:
                app.state.show_edid_markers = !app.state.show_edid_markers;
                app.overlay_dirty = true;
                break;
            case HudMenuItem::Fullscreen:
                app.fullscreen = !app.fullscreen;
                app.state.fullscreen = app.fullscreen;
                SDL_SetWindowFullscreen(app.window, app.fullscreen);
                app.overlay_dirty = true;
                break;
            case HudMenuItem::ExportMonique:
                if (dir == 0) {
                    app.export_to_monique_and_open(running);
                }
                break;
            case HudMenuItem::HideHud:
                if (dir == 0) {
                    app.state.show_hud = false;
                    app.overlay_dirty = true;
                }
                break;
            case HudMenuItem::Quit:
                if (dir == 0) {
                    running = false;
                }
                break;
        }
    };

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_WINDOW_HDR_STATE_CHANGED:
                    app.refresh_hdr_capabilities();
                    app.overlay_dirty = true;
                    break;
                case SDL_EVENT_WINDOW_RESIZED:
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                    app.overlay_dirty = true;
                    break;
                case SDL_EVENT_MOUSE_MOTION: {
                    auto [px, py] = to_px_coords(ev.motion.x, ev.motion.y);
                    double mx = 0.0, mw = 0.0;
                    int hit = menu_hit_row(px, py, mx, mw);
                    if (hit >= 0 && hit != app.state.menu_selected) {
                        app.state.menu_selected = hit;
                        app.overlay_dirty = true;
                    }
                    if (!app.cursor_locked) {
                        app.state.cursor_active = true;
                        app.state.cursor_x_px = px;
                        app.state.cursor_y_px = py;
                        app.overlay_dirty = true;
                    }
                    break;
                }
                case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                    if (!app.cursor_locked) {
                        app.state.cursor_active = false;
                        app.overlay_dirty = true;
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        auto [px, py] = to_px_coords(ev.button.x, ev.button.y);
                        double mx = 0.0, mw = 0.0;
                        int hit = menu_hit_row(px, py, mx, mw);
                        if (hit >= 0) {
                            app.state.menu_selected = hit;
                            bool is_action = (hit >= static_cast<int>(HudMenuItem::ExportMonique));
                            int dir = is_action ? 0 : ((px >= mx + mw * 0.55 && px < mx + mw * 0.77) ? -1 : 1);
                            activate_menu_item(hit, dir);
                            app.overlay_dirty = true;
                            break;
                        }
                        app.cursor_locked = !app.cursor_locked;
                        if (!app.cursor_locked) {
                            app.state.cursor_active = true;
                            app.state.cursor_x_px = px;
                            app.state.cursor_y_px = py;
                        }
                        app.overlay_dirty = true;
                    }
                    break;
                case SDL_EVENT_KEY_DOWN: {
                    bool shift = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
                    switch (ev.key.key) {
                        case SDLK_ESCAPE:
                        case SDLK_Q:
                            running = false;
                            break;
                        case SDLK_F:
                        case SDLK_F11:
                            app.fullscreen = !app.fullscreen;
                            app.state.fullscreen = app.fullscreen;
                            SDL_SetWindowFullscreen(app.window, app.fullscreen);
                            app.overlay_dirty = true;
                            break;
                        case SDLK_S:
                            app.state.scale_mode = static_cast<ScaleMode>((static_cast<int>(app.state.scale_mode) + 1) % 3);
                            app.overlay_dirty = true;
                            break;
                        case SDLK_V: {
                            int next_v = (static_cast<int>(app.state.view_mode) + (shift ? kViewModeCount - 1 : 1)) % kViewModeCount;
                            app.state.view_mode = static_cast<ViewMode>(next_v);
                            if (app.state.view_mode != ViewMode::CieTriangle) {
                                app.prev_non_cie_view = app.state.view_mode;
                            }
                            app.overlay_dirty = true;
                            break;
                        }
                        case SDLK_I: {
                            if (app.state.view_mode != ViewMode::TestPattern) {
                                app.state.view_mode = ViewMode::TestPattern;
                                app.prev_non_cie_view = ViewMode::TestPattern;
                            } else {
                                int next_p = (static_cast<int>(app.state.test_pattern) + (shift ? kTestPatternCount - 1 : 1)) % kTestPatternCount;
                                app.state.test_pattern = static_cast<TestPattern>(next_p);
                            }
                            app.overlay_dirty = true;
                            break;
                        }
                        case SDLK_SPACE:
                            if (app.state.view_mode == ViewMode::TestPattern) {
                                int next_p = (static_cast<int>(app.state.test_pattern) + (shift ? kTestPatternCount - 1 : 1)) % kTestPatternCount;
                                app.state.test_pattern = static_cast<TestPattern>(next_p);
                                app.overlay_dirty = true;
                            }
                            break;
                        case SDLK_T:
                            if (app.state.view_mode == ViewMode::CieTriangle) {
                                app.state.view_mode = app.prev_non_cie_view;
                            } else {
                                app.prev_non_cie_view = app.state.view_mode;
                                app.state.view_mode = ViewMode::CieTriangle;
                            }
                            app.overlay_dirty = true;
                            break;
                        case SDLK_C:
                            app.state.channel_mode = static_cast<ChannelMode>((static_cast<int>(app.state.channel_mode) + 1) % 4);
                            app.overlay_dirty = true;
                            break;
                        case SDLK_M:
                            app.cycle_output_mode(shift ? -1 : 1);
                            break;
                        case SDLK_P:
                            app.cycle_preset(shift ? -1 : 1);
                            break;
                        case SDLK_G:
                            app.export_to_monique_and_open(running);
                            break;
                        case SDLK_Z:
                            app.apply_zoom_preset(app.zoom_preset + 1);
                            break;
                        case SDLK_L:
                            if (app.state.mode_limits.has_active_limit && app.state.mode_limits.active_max_nits > 1.0) {
                                app.state.min_nits = 0.0;
                                app.state.max_nits = std::clamp(
                                    std::ceil(app.state.mode_limits.active_max_nits * 1.35 / 10.0) * 10.0,
                                    50.0, 10000.0);
                                app.overlay_dirty = true;
                            }
                            break;
                        case SDLK_N: {
                            auto it = std::find(band_stops.begin(), band_stops.end(), app.state.band_count);
                            size_t next_idx = (it == band_stops.end()) ? 0 : (static_cast<size_t>(it - band_stops.begin()) + 1) % band_stops.size();
                            app.state.band_count = band_stops[next_idx];
                            app.overlay_dirty = true;
                            break;
                        }
                        case SDLK_E:
                            app.state.show_edid_markers = !app.state.show_edid_markers;
                            app.overlay_dirty = true;
                            break;
                        case SDLK_H:
                            app.state.show_hud = !app.state.show_hud;
                            app.overlay_dirty = true;
                            break;
                        case SDLK_R:
                            app.reset_defaults(-1.0);
                            break;
                        case SDLK_RETURN:
                        case SDLK_KP_ENTER:
                            if (app.state.show_hud && app.state.menu_visible) {
                                activate_menu_item(app.state.menu_selected, 0);
                            }
                            break;
                        case SDLK_UP:
                            if (!shift && app.state.show_hud && app.state.menu_visible) {
                                app.state.menu_selected = (app.state.menu_selected + kHudMenuItemCount - 1) % kHudMenuItemCount;
                            } else if (shift) {
                                app.state.max_nits = std::min(10000.0, app.state.max_nits + 25.0);
                            } else {
                                app.state.max_nits = step_value_up(app.state.max_nits, max_stops);
                            }
                            app.overlay_dirty = true;
                            break;
                        case SDLK_DOWN: {
                            if (!shift && app.state.show_hud && app.state.menu_visible) {
                                app.state.menu_selected = (app.state.menu_selected + 1) % kHudMenuItemCount;
                                app.overlay_dirty = true;
                                break;
                            }
                            double next_max = shift
                                ? std::max(app.state.min_nits + 1.0, app.state.max_nits - 25.0)
                                : step_value_down(app.state.max_nits, max_stops);
                            if (next_max > app.state.min_nits + 0.1) {
                                app.state.max_nits = next_max;
                                app.overlay_dirty = true;
                            }
                            break;
                        }
                        case SDLK_RIGHT: {
                            if (!shift && app.state.show_hud && app.state.menu_visible) {
                                activate_menu_item(app.state.menu_selected, 1);
                                break;
                            }
                            double next_min = shift
                                ? std::min(app.state.max_nits - 1.0, app.state.min_nits + 1.0)
                                : step_value_up(app.state.min_nits, min_stops);
                            if (next_min < app.state.max_nits - 0.1) {
                                app.state.min_nits = next_min;
                                app.overlay_dirty = true;
                            }
                            break;
                        }
                        case SDLK_LEFT:
                            if (!shift && app.state.show_hud && app.state.menu_visible) {
                                activate_menu_item(app.state.menu_selected, -1);
                                break;
                            }
                            if (shift) {
                                app.state.min_nits = std::max(0.0, app.state.min_nits - 1.0);
                            } else {
                                app.state.min_nits = step_value_down(app.state.min_nits, min_stops);
                            }
                            app.overlay_dirty = true;
                            break;
                        case SDLK_PLUS:
                        case SDLK_KP_PLUS:
                        case SDLK_EQUALS:
                        case SDLK_MINUS:
                        case SDLK_KP_MINUS: {
                            bool is_inc = (ev.key.key == SDLK_PLUS || ev.key.key == SDLK_KP_PLUS || ev.key.key == SDLK_EQUALS);
                            int step = (ev.key.mod & SDL_KMOD_CTRL) ? 1 : 5;
                            app.adjust_sdr_brightness(is_inc ? step : -step);
                            break;
                        }
                        case SDLK_LEFTBRACKET:
                            app.state.hud_nits = std::max(10.0f, app.state.hud_nits - 15.0f);
                            app.overlay_dirty = true;
                            break;
                        case SDLK_RIGHTBRACKET:
                            app.state.hud_nits = std::min(250.0f, app.state.hud_nits + 15.0f);
                            app.overlay_dirty = true;
                            break;
                        default:
                            break;
                    }
                    break;
                }
                default:
                    break;
            }
        }

        if (app.limits.consume_dirty()) {
            app.state.mode_limits = app.limits.resolve(
                app.state.window_hdr_enabled,
                app.state.window_hdr_headroom,
                app.state.window_sdr_white_level,
                app.state.edid);
            app.overlay_dirty = true;
        }
        app.render_frame();
        SDL_Delay(8);
    }

    app.cleanup();
    return 0;
}
