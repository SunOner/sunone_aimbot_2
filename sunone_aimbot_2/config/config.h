#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <vector>
#include <unordered_map>
#include <utility>

class Config
{
public:
    // Capture
    std::string capture_method; // "duplication_api", "winrt", "virtual_camera", "udp_capture"
    std::string capture_target;
    std::string capture_window_title;
    std::string udp_ip;
    int udp_port;
    int detection_resolution;
    int capture_fps;
    int monitor_idx;
    bool circle_fov_enabled;
    int circle_fov_radius_percent;
    bool circle_fov_show_preview;
    bool capture_borders;
    bool capture_cursor;
    std::string virtual_camera_name;
    int virtual_camera_width;
    int virtual_camera_heigth;

    // Target
    bool disable_headshot;
    float body_y_offset;
    float head_y_offset;
    bool auto_aim;
    bool tracker_enabled;
    bool tracker_overlay_table_enabled;

    // Mouse
    int fovX;
    int fovY;
    float minSpeedMultiplier;
    float maxSpeedMultiplier;

    float predictionInterval;
    int prediction_futurePositions;
    bool draw_futurePositions;
    bool kalman_enabled;
    float kalman_process_noise_position;
    float kalman_process_noise_velocity;
    float kalman_measurement_noise;
    float kalman_velocity_damping;
    float kalman_max_velocity;
    int kalman_warmup_frames;
    bool kalman_compensate_detection_delay;
    float kalman_additional_prediction_ms;
    float kalman_reset_timeout_sec;

    float snapRadius;
    float nearRadius;
    float speedCurveExponent;
    float snapBoostFactor;

    bool easynorecoil;
    float easynorecoilstrength;
    std::string input_method; // "WIN32", "GHUB", "RAZER", "ARDUINO", "RP2350", "TEENSY41", "TEENSY41_HID", "KMBOX_NET", "KMBOX_A", "MAKCU"

    // Wind mouse
    bool wind_mouse_enabled;
    float wind_G;
    float wind_W;
    float wind_M;
    float wind_D;

    // Arduino
    int arduino_baudrate;
    std::string arduino_port;
    bool arduino_16_bit_mouse;
    bool arduino_enable_keys;

    // RP2350
    int rp2350_baudrate;
    std::string rp2350_port;
    bool rp2350_16_bit_mouse;
    bool rp2350_enable_keys;

    // Teensy 4.1 RawHID generic mouse bridge
    std::string teensy_hid_serial;
    std::string teensy_hid_vid_filter;
    std::string teensy_hid_pid_filter;
    int teensy_hid_usage_page;
    int teensy_hid_usage_id;
    int teensy_hid_open_index;
    int teensy_hid_packet_timeout_ms;
    int teensy_hid_reconnect_interval_ms;

    // kmbox_net
    std::string kmbox_net_ip;
    std::string kmbox_net_port;
    std::string kmbox_net_uuid;

    // kmbox_a
    std::string kmbox_a_pidvid; // PIDVID in one field, format: PPPPVVVV

    // makcu
    int makcu_baudrate;
    std::string makcu_port;
    bool makcu_enable_keys;

    // Mouse shooting
    bool auto_shoot;
    float bScope_multiplier;

    // AI
    std::string backend;
#ifndef USE_CUDA
    int dml_device_id;
#endif
    std::string ai_model;
    float confidence_threshold;
    float nms_threshold;
    int max_detections;
#ifdef USE_CUDA
    bool export_enable_fp8;
    bool export_enable_fp16;
#endif
    bool fixed_input_size;

    // CUDA
#ifdef USE_CUDA
    bool use_cuda_graph;
    bool use_pinned_memory;
    int gpuMemoryReserveMB;
    bool enableGpuExclusiveMode;
    bool capture_use_cuda;
#endif

    // System
    int cpuCoreReserveCount;
    int systemMemoryReserveMB;

    // Buttons
    std::vector<std::string> button_targeting;
    std::vector<std::string> button_shoot;
    std::vector<std::string> button_zoom;
    std::vector<std::string> button_exit;
    std::vector<std::string> button_pause;
    std::vector<std::string> button_reload_config;
    std::vector<std::string> button_open_overlay;
    bool enable_arrows_settings;

    // Overlay
    int overlay_opacity;
    float overlay_ui_scale;
    bool overlay_exclude_from_capture;
    int overlay_x;
    int overlay_y;
    int overlay_width;
    int overlay_height;

    // Depth
    bool depth_inference_enabled;
    std::string depth_model_path;
    int depth_fps;
    int depth_colormap;
    bool depth_mask_enabled;
    int depth_mask_fps;
    int depth_mask_near_percent;
    int depth_mask_expand;
    int depth_mask_hold_frames;
    int depth_mask_alpha;
    bool depth_mask_invert;
    bool depth_debug_overlay_enabled;

    // Game Overlay
    bool game_overlay_enabled;
    int game_overlay_max_fps;
    bool game_overlay_draw_boxes;
    bool game_overlay_compensate_latency;
    bool game_overlay_draw_future;
    bool game_overlay_draw_wind_tail;
    bool game_overlay_draw_frame;
    bool game_overlay_draw_circle_fov;
    bool game_overlay_show_target_correction;
    int game_overlay_box_a;
    int game_overlay_box_r;
    int game_overlay_box_g;
    int game_overlay_box_b;
    int game_overlay_frame_a;
    int game_overlay_frame_r;
    int game_overlay_frame_g;
    int game_overlay_frame_b;
    float game_overlay_box_thickness;
    float game_overlay_frame_thickness;
    float game_overlay_future_point_radius;
    float game_overlay_future_alpha_falloff;

    bool game_overlay_icon_enabled;
    std::string game_overlay_icon_path;
    int game_overlay_icon_width;
    int game_overlay_icon_height;
    float game_overlay_icon_offset_x;
    float game_overlay_icon_offset_y;
    std::string game_overlay_icon_anchor; // "center", "top", "bottom", "head"
    int game_overlay_icon_class; // -1 = all

    // Data collection
    bool collect_data_while_playing;
    bool collect_only_when_aimbot_running;
    bool collect_only_when_targets_present;
    int collect_save_every_n_frames;
    int collect_jpeg_quality;
    std::string collect_output_dir;
    bool auto_label_data;
    float auto_label_min_conf;
    int auto_label_max_boxes;
    std::string auto_label_record_classes;

    void clampGameOverlayColor()
    {
        auto clamp255 = [](int& v) { if (v < 0) v = 0; if (v > 255) v = 255; };
        clamp255(game_overlay_box_a);
        clamp255(game_overlay_box_r);
        clamp255(game_overlay_box_g);
        clamp255(game_overlay_box_b);
        clamp255(game_overlay_frame_a);
        clamp255(game_overlay_frame_r);
        clamp255(game_overlay_frame_g);
        clamp255(game_overlay_frame_b);
    }

    // Classes
    int class_player;
    int class_head;

    // Debug
    bool show_window;
    bool show_fps;
    std::vector<std::string> screenshot_button;
    int screenshot_delay;
    bool verbose;

    struct GameProfile
    {
        std::string name;
        double sens;
        double yaw;
        double pitch;
        bool fovScaled;
        double baseFOV;
    };

    std::unordered_map<std::string, GameProfile> game_profiles;
    std::string                                  active_game;

    const GameProfile & currentProfile() const;
    std::pair<double, double> degToCounts(double degX, double degY, double fovNow) const;

    bool loadConfig(const std::string& filename = "config.ini");
    bool saveConfig(const std::string& filename = "config.ini");

    std::string joinStrings(const std::vector<std::string>& vec, const std::string& delimiter = ",");
private:
    std::vector<std::string> splitString(const std::string& str, char delimiter = ',');
    std::string config_path;
};

#endif // CONFIG_H
