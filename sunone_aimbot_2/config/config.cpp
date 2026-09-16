#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <algorithm>

#include "config.h"
#include "modules/SimpleIni.h"

std::vector<std::string> Config::splitString(const std::string& str, char delimiter)
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, delimiter))
    {
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
            item.erase(item.begin());
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
            item.pop_back();

        tokens.push_back(item);
    }
    return tokens;
}

std::string Config::joinStrings(const std::vector<std::string>& vec, const std::string& delimiter)
{
    std::ostringstream oss;
    for (size_t i = 0; i < vec.size(); ++i)
    {
        if (i != 0) oss << delimiter;
        oss << vec[i];
    }
    return oss.str();
}

bool Config::loadConfig(const std::string& filename)
{
    std::string target = filename.empty() ? "config.ini" : filename;
    std::error_code absEc;
    std::filesystem::path absPath = std::filesystem::absolute(target, absEc);
    config_path = absEc ? target : absPath.string();

    if (!std::filesystem::exists(target))
    {
        std::cerr << "[Config] Config file does not exist, creating default config: " << target << std::endl;

        // Capture
        capture_method = "duplication_api";
        capture_target = "monitor";
        capture_window_title = "";
        udp_ip = "0.0.0.0";
        udp_port = 1234;
        detection_resolution = 320;
        capture_fps = 60;
        monitor_idx = 0;
        circle_fov_enabled = true;
        circle_fov_radius_percent = 100;
        circle_fov_show_preview = true;
        capture_borders = true;
        capture_cursor = true;
        virtual_camera_name = "None";
        virtual_camera_width = 1920;
        virtual_camera_heigth = 1080;

        // Target
        disable_headshot = false;
        body_y_offset = 0.15f;
        head_y_offset = 0.05f;
        auto_aim = false;
        tracker_enabled = true;
        tracker_overlay_table_enabled = true;

        // Mouse
        fovX = 106;
        fovY = 74;
        minSpeedMultiplier = 0.1f;
        maxSpeedMultiplier = 0.1f;

        predictionInterval = 0.01f;
        prediction_futurePositions = 20;
        draw_futurePositions = true;
        kalman_enabled = true;
        kalman_process_noise_position = 40.0f;
        kalman_process_noise_velocity = 1800.0f;
        kalman_measurement_noise = 35.0f;
        kalman_velocity_damping = 0.08f;
        kalman_max_velocity = 20000.0f;
        kalman_warmup_frames = 2;
        kalman_compensate_detection_delay = true;
        kalman_additional_prediction_ms = 0.0f;
        kalman_reset_timeout_sec = 0.5f;

        snapRadius = 1.5f;
        nearRadius = 25.0f;
        speedCurveExponent = 3.0f;
        snapBoostFactor = 1.15f;

        easynorecoil = false;
        easynorecoilstrength = 0.0f;
        input_method = "WIN32";

        // Wind mouse
        wind_mouse_enabled = false;
        wind_G = 18.0f;
        wind_W = 15.0f;
        wind_M = 10.0f;
        wind_D = 8.0f;

        // Arduino
        arduino_baudrate = 115200;
        arduino_port = "COM0";
        arduino_16_bit_mouse = false;
        arduino_enable_keys = false;

        // RP2350
        rp2350_baudrate = 115200;
        rp2350_port = "COM0";
        rp2350_16_bit_mouse = true;
        rp2350_enable_keys = false;

        // Teensy 4.1 RawHID generic mouse bridge
        teensy_hid_serial = "AUTO";
        teensy_hid_vid_filter = "AUTO";
        teensy_hid_pid_filter = "AUTO";
        teensy_hid_usage_page = 65451;
        teensy_hid_usage_id = 512;
        teensy_hid_open_index = 0;
        teensy_hid_packet_timeout_ms = 2;
        teensy_hid_reconnect_interval_ms = 500;

        // kmbox_net
        kmbox_net_ip = "10.42.42.42";
        kmbox_net_port = "1984";
        kmbox_net_uuid = "DEADC0DE";

        // kmbox_a
        kmbox_a_pidvid = "";

        // makcu
        makcu_baudrate = 115200;
        makcu_port = "COM0";
        makcu_enable_keys = false;

        // Mouse shooting
        auto_shoot = false;
        bScope_multiplier = 1.0f;

        // AI
#ifdef USE_CUDA
        backend = "TRT";
#else
        backend = "DML";
        dml_device_id = 0;
#endif

#ifdef USE_CUDA
        ai_model = "sunxds_0.5.6.engine";
#else
        ai_model = "sunxds_0.5.6.onnx";
#endif

        confidence_threshold = 0.10f;
        nms_threshold = 0.50f;
        max_detections = 100;
#ifdef USE_CUDA
        export_enable_fp8 = false;
        export_enable_fp16 = true;
#endif
        fixed_input_size = false;

        // CUDA
#ifdef USE_CUDA
        use_cuda_graph = false;
        use_pinned_memory = false;
        gpuMemoryReserveMB = 2048;
        enableGpuExclusiveMode = true;
        capture_use_cuda = true;
#endif

        // System
        cpuCoreReserveCount = 4;
        systemMemoryReserveMB = 2048;

        // Buttons
        button_targeting = splitString("RightMouseButton");
        button_shoot = splitString("LeftMouseButton");
        button_zoom = splitString("RightMouseButton");
        button_exit = splitString("F2");
        button_pause = splitString("F3");
        button_reload_config = splitString("F4");
        button_open_overlay = splitString("Home");
        enable_arrows_settings = false;

        // Overlay
        overlay_opacity = 225;
        overlay_ui_scale = 1.0f;
        overlay_exclude_from_capture = true;
        overlay_x = 0;
        overlay_y = 0;
        overlay_width = 860;
        overlay_height = 526;

        // Depth
        depth_inference_enabled = true;
        depth_model_path = "depth_anything_v2.engine";
        depth_fps = 100;
        depth_colormap = 18;
        depth_mask_enabled = false;
        depth_mask_fps = 5;
        depth_mask_near_percent = 20;
        depth_mask_expand = 0;
        depth_mask_hold_frames = 0;
        depth_mask_alpha = 90;
        depth_mask_invert = false;
        depth_debug_overlay_enabled = false;

        // Game overlay
        game_overlay_enabled = false;
        game_overlay_max_fps = 0;
        game_overlay_draw_boxes = true;
        game_overlay_compensate_latency = true;
        game_overlay_draw_future = true;
        game_overlay_draw_wind_tail = true;
        game_overlay_draw_frame = true;
        game_overlay_draw_circle_fov = true;
        game_overlay_show_target_correction = true;
        game_overlay_box_a = 255;
        game_overlay_box_r = 0;
        game_overlay_box_g = 255;
        game_overlay_box_b = 0;
        game_overlay_frame_a = 180;
        game_overlay_frame_r = 255;
        game_overlay_frame_g = 255;
        game_overlay_frame_b = 255;
        game_overlay_box_thickness = 2.0f;
        game_overlay_frame_thickness = 1.5f;
        game_overlay_future_point_radius = 5.0f;
        game_overlay_future_alpha_falloff = 1.0f;

        game_overlay_icon_enabled = false;
        game_overlay_icon_path = "icon.png";
        game_overlay_icon_width = 64;
        game_overlay_icon_height = 64;
        game_overlay_icon_offset_x = 0.0f;
        game_overlay_icon_offset_y = 0.0f;
        game_overlay_icon_anchor = "center";
        game_overlay_icon_class = -1;

        // Data collection
        collect_data_while_playing = false;
        collect_only_when_aimbot_running = false;
        collect_only_when_targets_present = true;
        collect_save_every_n_frames = 15;
        collect_jpeg_quality = 95;
        collect_output_dir.clear();
        auto_label_data = true;
        auto_label_min_conf = 0.30f;
        auto_label_max_boxes = 20;
        auto_label_record_classes.clear();

        // Classes
        class_player = 0;
        class_head = 1;

        // Debug
        show_window = true;
        show_fps = false;
        screenshot_button = splitString("None");
        screenshot_delay = 500;
        verbose = false;

        // Game profiles
        game_profiles.clear();
        GameProfile uni;
        uni.name = "UNIFIED";
        uni.sens = 1.0;
        uni.yaw = 0.022;
        uni.pitch = uni.yaw;
        uni.fovScaled = false;
        uni.baseFOV = 0.0;
        game_profiles[uni.name] = uni;
        active_game = uni.name;

        saveConfig(target);
        return true;
    }

    CSimpleIniA ini;
    ini.SetUnicode();
    SI_Error rc = ini.LoadFile(target.c_str());
    if (rc < 0)
    {
        std::cerr << "[Config] Error parsing INI file: " << target << std::endl;
        return false;
    }

    auto get_string = [&](const char* key, const std::string& defval)
    {
        const char* val = ini.GetValue("", key, defval.c_str());
        return val ? std::string(val) : defval;
    };

    auto get_bool = [&](const char* key, bool defval)
        {
            return ini.GetBoolValue("", key, defval);
        };

    auto get_long = [&](const char* key, long defval)
        {
            return (int)ini.GetLongValue("", key, defval);
        };

    auto get_double = [&](const char* key, double defval)
        {
            return ini.GetDoubleValue("", key, defval);
        };

    game_profiles.clear();

    CSimpleIniA::TNamesDepend keys;
    ini.GetAllKeys("Games", keys);

    for (const auto& k : keys)
    {
        std::string name = k.pItem;
        std::string val = ini.GetValue("Games", k.pItem, "");
        auto parts = splitString(val, ',');

        try
        {
            if (parts.size() < 2)
                throw std::runtime_error("not enough values");

            GameProfile gp;
            gp.name = name;
            gp.sens = std::stod(parts[0]);
            gp.yaw = std::stod(parts[1]);
            gp.pitch = parts.size() > 2 ? std::stod(parts[2]) : gp.yaw;
            gp.fovScaled = parts.size() > 3 && (parts[3] == "true" || parts[3] == "1");
            gp.baseFOV = parts.size() > 4 ? std::stod(parts[4]) : 0.0;

            game_profiles[name] = gp;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[Config] Failed to parse profile: " << name
                << " = " << val << " (" << e.what() << ")" << std::endl;
        }
    }

    if (!game_profiles.count("UNIFIED"))
    {
        GameProfile uni;
        uni.name = "UNIFIED";
        uni.sens = 1.0;
        uni.yaw = 0.022;
        uni.pitch = uni.yaw;
        uni.fovScaled = false;
        uni.baseFOV = 0.0;
        game_profiles[uni.name] = uni;
    }

    active_game = get_string("active_game", active_game);
    if (!game_profiles.count(active_game) && !game_profiles.empty())
        active_game = game_profiles.begin()->first;

    // Capture
    capture_method = get_string("capture_method", "duplication_api");
    capture_target = get_string("capture_target", "monitor");
    capture_window_title = get_string("capture_window_title", "");
    udp_ip = get_string("udp_ip", "0.0.0.0");
    udp_port = get_long("udp_port", 1234);
    if (udp_port < 1 || udp_port > 65535)
        udp_port = 1234;
    detection_resolution = get_long("detection_resolution", 320);
    if (detection_resolution != 160 && detection_resolution != 320 && detection_resolution != 640)
        detection_resolution = 320;

    capture_fps = get_long("capture_fps", 60);
    monitor_idx = get_long("monitor_idx", 0);
    circle_fov_enabled = get_bool("circle_fov_enabled", true);
    circle_fov_radius_percent = get_long("circle_fov_radius_percent", 100);
    if (circle_fov_radius_percent < 1) circle_fov_radius_percent = 1;
    if (circle_fov_radius_percent > 100) circle_fov_radius_percent = 100;
    circle_fov_show_preview = get_bool("circle_fov_show_preview", true);
    capture_borders = get_bool("capture_borders", true);
    capture_cursor = get_bool("capture_cursor", true);
    virtual_camera_name = get_string("virtual_camera_name", "None");
    virtual_camera_width = get_long("virtual_camera_width", 1920);
    virtual_camera_heigth = get_long("virtual_camera_heigth", 1080);

    // Target
    disable_headshot = get_bool("disable_headshot", false);
    body_y_offset = (float)get_double("body_y_offset", 0.15);
    head_y_offset = (float)get_double("head_y_offset", 0.05);
    auto_aim = get_bool("auto_aim", false);
    tracker_enabled = get_bool("tracker_enabled", true);
    tracker_overlay_table_enabled = get_bool("tracker_overlay_table_enabled", true);

    // Mouse
    fovX = get_long("fovX", 106);
    fovY = get_long("fovY", 74);
    minSpeedMultiplier = (float)get_double("minSpeedMultiplier", 0.1);
    maxSpeedMultiplier = (float)get_double("maxSpeedMultiplier", 0.1);

    predictionInterval = (float)get_double("predictionInterval", 0.01);
    prediction_futurePositions = get_long("prediction_futurePositions", 20);
    draw_futurePositions = get_bool("draw_futurePositions", true);
    kalman_enabled = get_bool("kalman_enabled", true);
    kalman_process_noise_position = (float)get_double("kalman_process_noise_position", 40.0);
    kalman_process_noise_velocity = (float)get_double("kalman_process_noise_velocity", 1800.0);
    kalman_measurement_noise = (float)get_double("kalman_measurement_noise", 35.0);
    kalman_velocity_damping = (float)get_double("kalman_velocity_damping", 0.08);
    kalman_max_velocity = (float)get_double("kalman_max_velocity", 20000.0);
    kalman_warmup_frames = get_long("kalman_warmup_frames", 2);
    kalman_compensate_detection_delay = get_bool("kalman_compensate_detection_delay", true);
    kalman_additional_prediction_ms = (float)get_double("kalman_additional_prediction_ms", 0.0);
    kalman_reset_timeout_sec = (float)get_double("kalman_reset_timeout_sec", 0.5);
    
    snapRadius = (float)get_double("snapRadius", 1.5);
    nearRadius = (float)get_double("nearRadius", 25.0);
    speedCurveExponent = (float)get_double("speedCurveExponent", 3.0);
    snapBoostFactor = (float)get_double("snapBoostFactor", 1.15);

    easynorecoil = get_bool("easynorecoil", false);
    easynorecoilstrength = (float)get_double("easynorecoilstrength", 0.0);
    input_method = get_string("input_method", "WIN32");

    // Wind mouse
    wind_mouse_enabled = get_bool("wind_mouse_enabled", false);
    wind_G = (float)get_double("wind_G", 18.0f);
    wind_W = (float)get_double("wind_W", 15.0f);
    wind_M = (float)get_double("wind_M", 10.0f);
    wind_D = (float)get_double("wind_D", 8.0f);

    // Arduino
    arduino_baudrate = get_long("arduino_baudrate", 115200);
    arduino_port = get_string("arduino_port", "COM0");
    arduino_16_bit_mouse = get_bool("arduino_16_bit_mouse", false);
    arduino_enable_keys = get_bool("arduino_enable_keys", false);

    // RP2350
    rp2350_baudrate = get_long("rp2350_baudrate", 115200);
    rp2350_port = get_string("rp2350_port", "COM0");
    rp2350_16_bit_mouse = get_bool("rp2350_16_bit_mouse", true);
    rp2350_enable_keys = get_bool("rp2350_enable_keys", false);

    // Teensy 4.1 RawHID generic mouse bridge
    teensy_hid_serial = get_string("teensy_hid_serial", "AUTO");
    teensy_hid_vid_filter = get_string("teensy_hid_vid_filter", "AUTO");
    teensy_hid_pid_filter = get_string("teensy_hid_pid_filter", "AUTO");
    teensy_hid_usage_page = get_long("teensy_hid_usage_page", 65451);
    teensy_hid_usage_id = get_long("teensy_hid_usage_id", 512);
    teensy_hid_open_index = get_long("teensy_hid_open_index", 0);
    teensy_hid_packet_timeout_ms = get_long("teensy_hid_packet_timeout_ms", 2);
    teensy_hid_reconnect_interval_ms = get_long("teensy_hid_reconnect_interval_ms", 500);

    // kmbox_net
    kmbox_net_ip = get_string("kmbox_net_ip", "10.42.42.42");
    kmbox_net_port = get_string("kmbox_net_port", "1984");
    kmbox_net_uuid = get_string("kmbox_net_uuid", "DEADC0DE");

    // kmbox_a
    kmbox_a_pidvid = get_string("kmbox_a_pidvid", "");

    // makcu
    makcu_baudrate = get_long("makcu_baudrate", 115200);
    makcu_port = get_string("makcu_port", "COM0");
    makcu_enable_keys = get_bool("makcu_enable_keys", false);

    // Mouse shooting
    auto_shoot = get_bool("auto_shoot", false);
    bScope_multiplier = (float)get_double("bScope_multiplier", 1.2);

    // AI
#ifdef USE_CUDA
    backend = "TRT";
#else
    backend = "DML";
    dml_device_id = get_long("dml_device_id", 0);
#endif

#ifdef USE_CUDA
    ai_model = get_string("ai_model", "sunxds_0.8.0.engine");
#else
    ai_model = get_string("ai_model", "sunxds_0.8.0.onnx");
#endif
    confidence_threshold = (float)get_double("confidence_threshold", 0.15);
    nms_threshold = (float)get_double("nms_threshold", 0.50);
    max_detections = get_long("max_detections", 20);
#ifdef USE_CUDA
    export_enable_fp8 = get_bool("export_enable_fp8", true);
    export_enable_fp16 = get_bool("export_enable_fp16", true);
#endif

    // CUDA
#ifdef USE_CUDA
    use_cuda_graph = get_bool("use_cuda_graph", false);
    use_pinned_memory = get_bool("use_pinned_memory", true);
    gpuMemoryReserveMB = get_long("gpuMemoryReserveMB", 2048);
    enableGpuExclusiveMode = get_bool("enableGpuExclusiveMode", true);
    capture_use_cuda = get_bool("capture_use_cuda", true);
#endif

    // System
    cpuCoreReserveCount = get_long("cpuCoreReserveCount", 4);
    systemMemoryReserveMB = get_long("systemMemoryReserveMB", 2048);

    // Buttons
    button_targeting = splitString(get_string("button_targeting", "RightMouseButton"));
    button_shoot = splitString(get_string("button_shoot", "LeftMouseButton"));
    button_zoom = splitString(get_string("button_zoom", "RightMouseButton"));
    button_exit = splitString(get_string("button_exit", "F2"));
    button_pause = splitString(get_string("button_pause", "F3"));
    button_reload_config = splitString(get_string("button_reload_config", "F4"));
    button_open_overlay = splitString(get_string("button_open_overlay", "Home"));
    enable_arrows_settings = get_bool("enable_arrows_settings", false);

    // Overlay
    overlay_opacity = get_long("overlay_opacity", 225);
    overlay_ui_scale = (float)get_double("overlay_ui_scale", 1.0);
    overlay_exclude_from_capture = get_bool("overlay_exclude_from_capture", true);
    overlay_x = get_long("overlay_x", 0);
    overlay_y = get_long("overlay_y", 0);
    overlay_width = get_long("overlay_width", 860);
    overlay_height = get_long("overlay_height", 526);

    // Depth
    depth_inference_enabled = get_bool("depth_inference_enabled", true);
    depth_model_path = get_string("depth_model_path", "depth_anything_v2.engine");
    depth_fps = get_long("depth_fps", 100);
    if (depth_fps < 0) depth_fps = 0;
    depth_colormap = get_long("depth_colormap", 18);
    if (depth_colormap < 0 || depth_colormap > 21) depth_colormap = 18;
    depth_mask_enabled = get_bool("depth_mask_enabled", false);
    depth_mask_fps = get_long("depth_mask_fps", 5);
    if (depth_mask_fps < 0) depth_mask_fps = 0;
    depth_mask_near_percent = get_long("depth_mask_near_percent", 20);
    if (depth_mask_near_percent < 1) depth_mask_near_percent = 1;
    if (depth_mask_near_percent > 100) depth_mask_near_percent = 100;
    depth_mask_expand = get_long("depth_mask_expand", 0);
    if (depth_mask_expand < 0) depth_mask_expand = 0;
    if (depth_mask_expand > 128) depth_mask_expand = 128;
    depth_mask_hold_frames = get_long("depth_mask_hold_frames", 0);
    if (depth_mask_hold_frames < 0) depth_mask_hold_frames = 0;
    if (depth_mask_hold_frames > 120) depth_mask_hold_frames = 120;
    depth_mask_alpha = get_long("depth_mask_alpha", 90);
    if (depth_mask_alpha < 0) depth_mask_alpha = 0;
    if (depth_mask_alpha > 255) depth_mask_alpha = 255;
    depth_mask_invert = get_bool("depth_mask_invert", false);
    depth_debug_overlay_enabled = get_bool("depth_debug_overlay_enabled", false);

    game_overlay_enabled = get_bool("game_overlay_enabled", false);
    game_overlay_max_fps = get_long("game_overlay_max_fps", 0);
    game_overlay_draw_boxes = get_bool("game_overlay_draw_boxes", true);
    game_overlay_compensate_latency = get_bool("game_overlay_compensate_latency", true);
    game_overlay_draw_future = get_bool("game_overlay_draw_future", true);
    game_overlay_draw_wind_tail = get_bool("game_overlay_draw_wind_tail", true);
    game_overlay_draw_frame = get_bool("game_overlay_draw_frame", true);
    game_overlay_draw_circle_fov = get_bool("game_overlay_draw_circle_fov", true);
    game_overlay_show_target_correction = get_bool("game_overlay_show_target_correction", true);
    game_overlay_box_a = get_long("game_overlay_box_a", 255);
    game_overlay_box_r = get_long("game_overlay_box_r", 0);
    game_overlay_box_g = get_long("game_overlay_box_g", 255);
    game_overlay_box_b = get_long("game_overlay_box_b", 0);
    game_overlay_frame_a = get_long("game_overlay_frame_a", 180);
    game_overlay_frame_r = get_long("game_overlay_frame_r", 255);
    game_overlay_frame_g = get_long("game_overlay_frame_g", 255);
    game_overlay_frame_b = get_long("game_overlay_frame_b", 255);
    game_overlay_box_thickness = (float)get_double("game_overlay_box_thickness", 2.0);
    game_overlay_frame_thickness = (float)get_double("game_overlay_frame_thickness", 1.5);
    game_overlay_future_point_radius = (float)get_double("game_overlay_future_point_radius", 5.0);
    game_overlay_future_alpha_falloff = (float)get_double("game_overlay_future_alpha_falloff", 1.0);
    clampGameOverlayColor();

    game_overlay_icon_enabled = get_bool("game_overlay_icon_enabled", false);
    game_overlay_icon_path = get_string("game_overlay_icon_path", "icon.png");
    game_overlay_icon_width = get_long("game_overlay_icon_width", 64);
    game_overlay_icon_height = get_long("game_overlay_icon_height", 64);
    game_overlay_icon_offset_x = (float)get_double("game_overlay_icon_offset_x", 0.0f);
    game_overlay_icon_offset_y = (float)get_double("game_overlay_icon_offset_y", 0.0f);
    game_overlay_icon_anchor = get_string("game_overlay_icon_anchor", "center");
    game_overlay_icon_class = get_long("game_overlay_icon_class", -1);

    collect_data_while_playing = get_bool("collect_data_while_playing", false);
    collect_only_when_aimbot_running = get_bool("collect_only_when_aimbot_running", false);
    collect_only_when_targets_present = get_bool("collect_only_when_targets_present", true);
    collect_save_every_n_frames = get_long("collect_save_every_n_frames", 15);
    collect_output_dir = get_string("collect_output_dir", "");
    collect_jpeg_quality = get_long("collect_jpeg_quality", 95);
    auto_label_data = get_bool("auto_label_data", true);
    auto_label_min_conf = (float)get_double("auto_label_min_conf", 0.30);
    auto_label_max_boxes = get_long("auto_label_max_boxes", 20);
    auto_label_record_classes = get_string("auto_label_record_classes", "");

    if (kalman_process_noise_position < 0.0001f) kalman_process_noise_position = 0.0001f;
    if (kalman_process_noise_position > 5000.0f) kalman_process_noise_position = 5000.0f;
    if (kalman_process_noise_velocity < 0.0001f) kalman_process_noise_velocity = 0.0001f;
    if (kalman_process_noise_velocity > 50000.0f) kalman_process_noise_velocity = 50000.0f;
    if (kalman_measurement_noise < 0.0001f) kalman_measurement_noise = 0.0001f;
    if (kalman_measurement_noise > 5000.0f) kalman_measurement_noise = 5000.0f;
    if (kalman_velocity_damping < 0.0f) kalman_velocity_damping = 0.0f;
    if (kalman_velocity_damping > 3.0f) kalman_velocity_damping = 3.0f;
    if (kalman_max_velocity < 100.0f) kalman_max_velocity = 100.0f;
    if (kalman_max_velocity > 60000.0f) kalman_max_velocity = 60000.0f;
    if (kalman_warmup_frames < 0) kalman_warmup_frames = 0;
    if (kalman_warmup_frames > 20) kalman_warmup_frames = 20;
    if (kalman_additional_prediction_ms < -80.0f) kalman_additional_prediction_ms = -80.0f;
    if (kalman_additional_prediction_ms > 120.0f) kalman_additional_prediction_ms = 120.0f;
    if (kalman_reset_timeout_sec < 0.05f) kalman_reset_timeout_sec = 0.05f;
    if (kalman_reset_timeout_sec > 3.0f) kalman_reset_timeout_sec = 3.0f;

    if (overlay_width < 560) overlay_width = 560;
    if (overlay_width > 3840) overlay_width = 3840;
    if (overlay_height < 340) overlay_height = 340;
    if (overlay_height > 2160) overlay_height = 2160;

    if (collect_save_every_n_frames < 1) collect_save_every_n_frames = 1;
    if (collect_save_every_n_frames > 600) collect_save_every_n_frames = 600;
    if (collect_jpeg_quality < 50) collect_jpeg_quality = 50;
    if (collect_jpeg_quality > 100) collect_jpeg_quality = 100;
    if (auto_label_min_conf < 0.01f) auto_label_min_conf = 0.01f;
    if (auto_label_min_conf > 0.99f) auto_label_min_conf = 0.99f;
    if (auto_label_max_boxes < 1) auto_label_max_boxes = 1;
    if (auto_label_max_boxes > 200) auto_label_max_boxes = 200;

    // Classes
    class_player = get_long("class_player", 0);
    class_head = get_long("class_head", 1);

    // Debug window
    show_window = get_bool("show_window", true);
    show_fps = get_bool("show_fps", false);
    screenshot_button = splitString(get_string("screenshot_button", "None"));
    screenshot_delay = get_long("screenshot_delay", 500);
    verbose = get_bool("verbose", false);

    return true;
}

bool Config::saveConfig(const std::string& filename)
{
    std::string target = filename.empty() ? "config.ini" : filename;
    if (target == "config.ini" && !config_path.empty())
    {
        target = config_path;
    }

    std::ofstream file(target);
    if (!file.is_open())
    {
        std::cerr << "Error opening config for writing: " << target << std::endl;
        return false;
    }

    file << "# An explanation of the options can be found at:\n";
    file << "# https://github.com/SunOner/sunone_aimbot_2/blob/main/docs/config.md\n\n";

    // Capture
    file << "# Capture\n"
        << "capture_method = " << capture_method << "\n"
        << "capture_target = " << capture_target << "\n"
        << "capture_window_title = " << capture_window_title << "\n"
        << "udp_ip = " << udp_ip << "\n"
        << "udp_port = " << udp_port << "\n"
        << "detection_resolution = " << detection_resolution << "\n"
        << "capture_fps = " << capture_fps << "\n"
        << "monitor_idx = " << monitor_idx << "\n"
        << "circle_fov_enabled = " << (circle_fov_enabled ? "true" : "false") << "\n"
        << "circle_fov_radius_percent = " << circle_fov_radius_percent << "\n"
        << "circle_fov_show_preview = " << (circle_fov_show_preview ? "true" : "false") << "\n"
        << "capture_borders = " << (capture_borders ? "true" : "false") << "\n"
        << "capture_cursor = " << (capture_cursor ? "true" : "false") << "\n"
        << "virtual_camera_name = " << virtual_camera_name << "\n"
        << "virtual_camera_width = " << virtual_camera_width << "\n"
        << "virtual_camera_heigth = " << virtual_camera_heigth << "\n\n";

    // Target
    file << "# Target\n"
        << "disable_headshot = " << (disable_headshot ? "true" : "false") << "\n"
        << std::fixed << std::setprecision(2)
        << "body_y_offset = " << body_y_offset << "\n"
        << "head_y_offset = " << head_y_offset << "\n"
        << "auto_aim = " << (auto_aim ? "true" : "false") << "\n"
        << "tracker_enabled = " << (tracker_enabled ? "true" : "false") << "\n"
        << "tracker_overlay_table_enabled = " << (tracker_overlay_table_enabled ? "true" : "false") << "\n\n";

    // Mouse
    file << "# Mouse move\n"
        << "fovX = " << fovX << "\n"
        << "fovY = " << fovY << "\n"
        << "minSpeedMultiplier = " << minSpeedMultiplier << "\n"
        << "maxSpeedMultiplier = " << maxSpeedMultiplier << "\n"

        << std::fixed << std::setprecision(2)
        << "predictionInterval = " << predictionInterval << "\n"
        << "prediction_futurePositions = " << prediction_futurePositions << "\n"
        << "draw_futurePositions = " << (draw_futurePositions ? "true" : "false") << "\n"
        << "kalman_enabled = " << (kalman_enabled ? "true" : "false") << "\n"
        << "kalman_process_noise_position = " << kalman_process_noise_position << "\n"
        << "kalman_process_noise_velocity = " << kalman_process_noise_velocity << "\n"
        << "kalman_measurement_noise = " << kalman_measurement_noise << "\n"
        << "kalman_velocity_damping = " << kalman_velocity_damping << "\n"
        << "kalman_max_velocity = " << kalman_max_velocity << "\n"
        << std::setprecision(0)
        << "kalman_warmup_frames = " << kalman_warmup_frames << "\n"
        << "kalman_compensate_detection_delay = " << (kalman_compensate_detection_delay ? "true" : "false") << "\n"
        << std::fixed << std::setprecision(2)
        << "kalman_additional_prediction_ms = " << kalman_additional_prediction_ms << "\n"
        << "kalman_reset_timeout_sec = " << kalman_reset_timeout_sec << "\n"

        << "snapRadius = " << snapRadius << "\n"
        << "nearRadius = " << nearRadius << "\n"
        << "speedCurveExponent = " << speedCurveExponent << "\n"
        << std::fixed << std::setprecision(2)
        << "snapBoostFactor = " << snapBoostFactor << "\n"

        << "easynorecoil = " << (easynorecoil ? "true" : "false") << "\n"
        << std::fixed << std::setprecision(1)
        << "easynorecoilstrength = " << easynorecoilstrength << "\n"

        << "# WIN32, GHUB, RAZER, ARDUINO, RP2350, TEENSY41, TEENSY41_HID, KMBOX_NET, KMBOX_A, MAKCU\n"
        << "input_method = " << input_method << "\n\n";

    // Wind mouse
    file << "# Wind mouse\n"
        << "wind_mouse_enabled = " << (wind_mouse_enabled ? "true" : "false") << "\n"
        << "wind_G = " << wind_G << "\n"
        << "wind_W = " << wind_W << "\n"
        << "wind_M = " << wind_M << "\n"
        << "wind_D = " << wind_D << "\n\n";

    // Arduino
    file << "# Arduino\n"
        << "arduino_baudrate = " << arduino_baudrate << "\n"
        << "arduino_port = " << arduino_port << "\n"
        << "arduino_16_bit_mouse = " << (arduino_16_bit_mouse ? "true" : "false") << "\n"
        << "arduino_enable_keys = " << (arduino_enable_keys ? "true" : "false") << "\n\n";

    // RP2350
    file << "# RP2350\n"
        << "rp2350_baudrate = " << rp2350_baudrate << "\n"
        << "rp2350_port = " << rp2350_port << "\n"
        << "rp2350_16_bit_mouse = " << (rp2350_16_bit_mouse ? "true" : "false") << "\n"
        << "rp2350_enable_keys = " << (rp2350_enable_keys ? "true" : "false") << "\n\n";

    file << "# Teensy 4.1 RawHID generic mouse bridge\n"
        << "teensy_hid_serial = " << teensy_hid_serial << "\n"
        << "teensy_hid_vid_filter = " << teensy_hid_vid_filter << "\n"
        << "teensy_hid_pid_filter = " << teensy_hid_pid_filter << "\n"
        << "teensy_hid_usage_page = " << teensy_hid_usage_page << "\n"
        << "teensy_hid_usage_id = " << teensy_hid_usage_id << "\n"
        << "teensy_hid_open_index = " << teensy_hid_open_index << "\n"
        << "teensy_hid_packet_timeout_ms = " << teensy_hid_packet_timeout_ms << "\n"
        << "teensy_hid_reconnect_interval_ms = " << teensy_hid_reconnect_interval_ms << "\n\n";

    // kmbox_net
    file << "# Kmbox_net\n"
        << "kmbox_net_ip = " << kmbox_net_ip << "\n"
        << "kmbox_net_port = " << kmbox_net_port << "\n"
        << "kmbox_net_uuid = " << kmbox_net_uuid << "\n\n";

    file << "# Kmbox_a\n"
        << "kmbox_a_pidvid = " << kmbox_a_pidvid << "\n\n";

    // makcu
    file << "# Makcu\n"
        << "makcu_baudrate = " << makcu_baudrate << "\n"
        << "makcu_port = " << makcu_port << "\n"
        << "makcu_enable_keys = " << (makcu_enable_keys ? "true" : "false") << "\n\n";

    // Mouse shooting
    file << "# Mouse shooting\n"
        << "auto_shoot = " << (auto_shoot ? "true" : "false") << "\n"
        << std::fixed << std::setprecision(1)
        << "bScope_multiplier = " << bScope_multiplier << "\n\n";

    // AI
    file << "# AI\n"
        << "backend = " << backend << "\n";
#ifndef USE_CUDA
    file << "dml_device_id = " << dml_device_id << "\n";
#endif
    file << "ai_model = " << ai_model << "\n"
        << std::fixed << std::setprecision(2)
        << "confidence_threshold = " << confidence_threshold << "\n"
        << "nms_threshold = " << nms_threshold << "\n"
        << std::setprecision(0)
        << "max_detections = " << max_detections << "\n"
#ifdef USE_CUDA
        << "export_enable_fp8 = " << (export_enable_fp8 ? "true" : "false") << "\n"
        << "export_enable_fp16 = " << (export_enable_fp16 ? "true" : "false") << "\n"
#endif
        ;

    // CUDA
#ifdef USE_CUDA
    file << "# CUDA\n"
        << "use_cuda_graph = " << (use_cuda_graph ? "true" : "false") << "\n"
        << "use_pinned_memory = " << (use_pinned_memory ? "true" : "false") << "\n"
        << "gpuMemoryReserveMB = " << gpuMemoryReserveMB << "\n"
        << "enableGpuExclusiveMode = " << (enableGpuExclusiveMode ? "true" : "false") << "\n"
        << "capture_use_cuda = " << (capture_use_cuda ? "true" : "false") << "\n\n";
#endif

	// System
    file << "# System\n"
        << "cpuCoreReserveCount = " << cpuCoreReserveCount << "\n"
        << "systemMemoryReserveMB = " << systemMemoryReserveMB << "\n\n";

    // Buttons
    file << "# Buttons\n"
        << "button_targeting = " << joinStrings(button_targeting) << "\n"
        << "button_shoot = " << joinStrings(button_shoot) << "\n"
        << "button_zoom = " << joinStrings(button_zoom) << "\n"
        << "button_exit = " << joinStrings(button_exit) << "\n"
        << "button_pause = " << joinStrings(button_pause) << "\n"
        << "button_reload_config = " << joinStrings(button_reload_config) << "\n"
        << "button_open_overlay = " << joinStrings(button_open_overlay) << "\n"
        << "enable_arrows_settings = " << (enable_arrows_settings ? "true" : "false") << "\n\n";

    // Overlay
    file << "# Overlay\n"
        << "overlay_opacity = " << overlay_opacity << "\n"
        << std::fixed << std::setprecision(2)
        << "overlay_ui_scale = " << overlay_ui_scale << "\n"
        << "overlay_exclude_from_capture = " << (overlay_exclude_from_capture ? "true" : "false") << "\n"
        << std::setprecision(0)
        << "overlay_x = " << overlay_x << "\n"
        << "overlay_y = " << overlay_y << "\n"
        << "overlay_width = " << overlay_width << "\n"
        << "overlay_height = " << overlay_height << "\n\n";

    file << "# Depth\n"
        << "depth_inference_enabled = " << (depth_inference_enabled ? "true" : "false") << "\n"
        << "depth_model_path = " << depth_model_path << "\n"
        << "depth_fps = " << depth_fps << "\n"
        << "depth_colormap = " << depth_colormap << "\n"
        << "depth_mask_enabled = " << (depth_mask_enabled ? "true" : "false") << "\n"
        << "depth_mask_fps = " << depth_mask_fps << "\n"
        << "depth_mask_near_percent = " << depth_mask_near_percent << "\n"
        << "depth_mask_expand = " << depth_mask_expand << "\n"
        << "depth_mask_hold_frames = " << depth_mask_hold_frames << "\n"
        << "depth_mask_alpha = " << depth_mask_alpha << "\n"
        << "depth_mask_invert = " << (depth_mask_invert ? "true" : "false") << "\n"
        << "depth_debug_overlay_enabled = " << (depth_debug_overlay_enabled ? "true" : "false") << "\n\n";

    file << "# Game Overlay\n"
        << "game_overlay_enabled = " << (game_overlay_enabled ? "true" : "false") << "\n"
        << "game_overlay_max_fps = " << game_overlay_max_fps << "\n"
        << "game_overlay_draw_boxes = " << (game_overlay_draw_boxes ? "true" : "false") << "\n"
        << "game_overlay_compensate_latency = " << (game_overlay_compensate_latency ? "true" : "false") << "\n"
        << "game_overlay_draw_future = " << (game_overlay_draw_future ? "true" : "false") << "\n"
        << "game_overlay_draw_wind_tail = " << (game_overlay_draw_wind_tail ? "true" : "false") << "\n"
        << "game_overlay_draw_frame = " << (game_overlay_draw_frame ? "true" : "false") << "\n"
        << "game_overlay_draw_circle_fov = " << (game_overlay_draw_circle_fov ? "true" : "false") << "\n"
        << "game_overlay_show_target_correction = " << (game_overlay_show_target_correction ? "true" : "false") << "\n"
        << "game_overlay_box_a = " << game_overlay_box_a << "\n"
        << "game_overlay_box_r = " << game_overlay_box_r << "\n"
        << "game_overlay_box_g = " << game_overlay_box_g << "\n"
        << "game_overlay_box_b = " << game_overlay_box_b << "\n"
        << "game_overlay_frame_a = " << game_overlay_frame_a << "\n"
        << "game_overlay_frame_r = " << game_overlay_frame_r << "\n"
        << "game_overlay_frame_g = " << game_overlay_frame_g << "\n"
        << "game_overlay_frame_b = " << game_overlay_frame_b << "\n"
        << std::fixed << std::setprecision(2)
        << "game_overlay_box_thickness = " << game_overlay_box_thickness << "\n"
        << "game_overlay_frame_thickness = " << game_overlay_frame_thickness << "\n"
        << "game_overlay_future_point_radius = " << game_overlay_future_point_radius << "\n"
        << "game_overlay_future_alpha_falloff = " << game_overlay_future_alpha_falloff << "\n\n";

    file << "game_overlay_icon_enabled = " << (game_overlay_icon_enabled ? "true" : "false") << "\n"
        << "game_overlay_icon_path = " << game_overlay_icon_path << "\n"
        << "game_overlay_icon_width = " << game_overlay_icon_width << "\n"
        << "game_overlay_icon_height = " << game_overlay_icon_height << "\n"
        << std::fixed << std::setprecision(2)
        << "game_overlay_icon_offset_x = " << game_overlay_icon_offset_x << "\n"
        << std::fixed << std::setprecision(2)
        << "game_overlay_icon_offset_y = " << game_overlay_icon_offset_y << "\n"
        << "game_overlay_icon_anchor = " << game_overlay_icon_anchor << "\n"
        << "game_overlay_icon_class = " << game_overlay_icon_class << "\n\n";

    file << "# Data Collection\n"
        << "collect_data_while_playing = " << (collect_data_while_playing ? "true" : "false") << "\n"
        << "collect_only_when_aimbot_running = " << (collect_only_when_aimbot_running ? "true" : "false") << "\n"
        << "collect_only_when_targets_present = " << (collect_only_when_targets_present ? "true" : "false") << "\n"
        << "collect_save_every_n_frames = " << collect_save_every_n_frames << "\n"
        << "collect_jpeg_quality = " << collect_jpeg_quality << "\n"
        << "collect_output_dir = " << collect_output_dir << "\n"
        << "auto_label_data = " << (auto_label_data ? "true" : "false") << "\n"
        << std::fixed << std::setprecision(2)
        << "auto_label_min_conf = " << auto_label_min_conf << "\n"
        << std::setprecision(0)
        << "auto_label_max_boxes = " << auto_label_max_boxes << "\n"
        << "auto_label_record_classes = " << auto_label_record_classes << "\n\n";

    // Classes
    file << "# Custom Classes\n"
        << "class_player = " << class_player << "\n"
        << "class_head = " << class_head << "\n\n";

    // Debug
    file << "# Debug\n"
        << "show_window = " << (show_window ? "true" : "false") << "\n"
        << "show_fps = " << (show_fps ? "true" : "false") << "\n"
        << "screenshot_button = " << joinStrings(screenshot_button) << "\n"
        << "screenshot_delay = " << screenshot_delay << "\n"
        << "verbose = " << (verbose ? "true" : "false") << "\n\n";

    // Active game
    file << "# Active game profile\n";
    file << "active_game = " << active_game << "\n\n";
    file << std::defaultfloat << std::setprecision(6);
    file << "[Games]\n";
    for (auto& kv : game_profiles)
    {
        auto & gp = kv.second;
        file << gp.name << " = "
             << gp.sens << "," << gp.yaw;
        file << "," << gp.pitch;
        if (gp.fovScaled)
            file << ",true," << gp.baseFOV;
        file << "\n";
    }

    file.close();
    return true;
}

const Config::GameProfile& Config::currentProfile() const
{
    auto it = game_profiles.find(active_game);
    if (it != game_profiles.end()) return it->second;
    throw std::runtime_error("Active game profile not found: " + active_game);
}

std::pair<double, double> Config::degToCounts(double degX, double degY, double fovNow) const
{
    const auto& gp = currentProfile();
    double scale = (gp.fovScaled && gp.baseFOV > 1.0) ? (fovNow / gp.baseFOV) : 1.0;

    if (gp.sens == 0.0 || gp.yaw == 0.0 || gp.pitch == 0.0)
        return { 0.0, 0.0 };

    double cx = degX / (gp.sens * gp.yaw * scale);
    double cy = degY / (gp.sens * gp.pitch * scale);
    return { cx, cy };
}
