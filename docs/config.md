# Configuration Guide

`config.ini` is created on first launch. Settings use root-level `key = value` lines; `[Games]` stores sensitivity profiles.

Capture, detection, aiming and output settings seed `node_graphs.ini` on first launch. Edit existing graphs in the [node editor](guides/node-graphs.md); device connections, application hotkeys and window preferences remain in `config.ini`.

## Basic Rules

- Use `true` or `false` for booleans.
- Use a dot for decimal values, for example `0.35`.
- Button lists are comma-separated, for example `RightMouseButton,LeftShift`.
- Paths may be relative to the executable folder.
- If a value is outside its accepted range, the app clamps it or falls back to a valid default.

Tables show generated defaults. Different fallbacks for missing keys in older configs are noted separately.

## Quick Backend Setup

### DML Build

Use ONNX models:

```ini
backend = DML
ai_model = sunxds_0.5.6.onnx
```

If DML runs but does not detect anything, check that the model is `.onnx`, lower `confidence_threshold` temporarily, and confirm your class IDs.

### CUDA + TensorRT Build

Use TensorRT engine models:

```ini
backend = TRT
ai_model = sunxds_0.5.6.engine
capture_use_cuda = true
```

## Capture

| Key | Default | Meaning |
|---|---:|---|
| `capture_method` | `duplication_api` | Capture source. Common values are `duplication_api`, `winrt`, `virtual_camera`, and `udp_capture`. |
| `capture_target` | `monitor` | Target type for capture. Usually `monitor`. |
| `capture_window_title` | empty | Window title used when a window capture target is selected. |
| `udp_ip` | `0.0.0.0` | Sender IP filter for UDP capture. Use `0.0.0.0` to accept frames from any sender. |
| `udp_port` | `1234` | UDP capture port. |
| `detection_resolution` | `320` | Square inference/capture processing size. Valid values are `160`, `320`, and `640`. Higher can improve detail but costs performance. |
| `capture_fps` | `60` | Requested capture rate. |
| `monitor_idx` | `0` | Monitor index for monitor capture. |
| `circle_fov_enabled` | `true` | Enables the current circular FOV limiter. |
| `circle_fov_radius_percent` | `100` | Circle FOV radius as a percent of the processed capture area. Clamped to `1..100`. |
| `capture_borders` | `true` | Include window borders when applicable. |
| `capture_cursor` | `true` | Include cursor when applicable. |
| `virtual_camera_name` | `None` | Camera name for virtual camera capture. |
| `virtual_camera_width` | `1920` | Requested virtual camera width. |
| `virtual_camera_heigth` | `1080` | Requested virtual camera height. The key is currently spelled `heigth` in the config for compatibility. |

### UDP Capture

```ini
capture_method = udp_capture
udp_ip = 0.0.0.0
udp_port = 1234
detection_resolution = 320
capture_fps = 60
```

The receiver expects JPEG frames over UDP, not RTP/RTSP. `udp_ip` filters senders; `0.0.0.0` accepts any sender. Allow `udp_port` through the receiver firewall.

See [FFmpeg sender examples](guides/udp-capture.md) and [Circle FOV setup](guides/circle-fov.md).

## Targeting

| Key | Default | Meaning |
|---|---:|---|
| `disable_headshot` | `false` | Disables head-specific targeting behavior. |
| `body_y_offset` | `0.15` | Vertical target offset for body aim. |
| `head_y_offset` | `0.05` | Vertical target offset for head aim. |
| `auto_aim` | `false` | Enables automatic aim behavior when supported by current controls and buttons. |
| `tracker_enabled` | `true` | Enables the simple persistent target tracker. When disabled, aiming falls back to nearest-target selection each detection frame. |

## Mouse Movement and Tracking

| Key | Default | Meaning |
|---|---:|---|
| `fovX` | `106` | Horizontal game FOV used for movement conversion. |
| `fovY` | `74` | Vertical game FOV used for movement conversion. |
| `minSpeedMultiplier` | `0.1` | Minimum movement multiplier. |
| `maxSpeedMultiplier` | `0.1` | Maximum movement multiplier. |
| `predictionInterval` | `0.01` | Prediction time step. |
| `prediction_futurePositions` | `20` | Number of predicted future positions to keep/draw. |
| `draw_futurePositions` | `true` | Draws predicted future positions in supported overlays. |
| `kalman_enabled` | `true` | Enables Kalman smoothing/tracking. |
| `kalman_process_noise_position` | `40.0` | Position process noise. Higher reacts faster but can be less stable. |
| `kalman_process_noise_velocity` | `1800.0` | Velocity process noise. Higher follows quick movement more aggressively. |
| `kalman_measurement_noise` | `35.0` | Detection measurement noise. Higher trusts detections less. |
| `kalman_velocity_damping` | `0.08` | Damps velocity over time. |
| `kalman_max_velocity` | `20000.0` | Caps estimated velocity. |
| `kalman_warmup_frames` | `2` | Frames before Kalman output is considered warmed up. |
| `kalman_compensate_detection_delay` | `true` | Compensates for capture/inference delay. |
| `kalman_additional_prediction_ms` | `0.0` | Extra prediction time in milliseconds. |
| `kalman_reset_timeout_sec` | `0.5` | Resets tracking after this long without detections. |
| `snapRadius` | `1.5` | Close target snap radius. |
| `nearRadius` | `25.0` | Radius where near-target behavior starts. |
| `speedCurveExponent` | `3.0` | Curve shape for speed scaling. |
| `snapBoostFactor` | `1.15` | Extra speed near snap radius. |
| `easynorecoil` | `false` | Enables simple recoil compensation. |
| `easynorecoilstrength` | `0.0` | Recoil compensation strength. |
| `input_method` | `WIN32` | Output/control method. See below. |

## Input Method

| Method | Plain meaning |
|---|---|
| `WIN32` | Standard Windows mouse events. |
| `GHUB` | GHub DLL output if available. |
| `RAZER` | Razer control DLL output through `rzctl.dll`. |
| `ARDUINO` | Serial Arduino mouse bridge. |
| `RP2350` | Serial RP2350 mouse bridge. |
| `TEENSY41` | Teensy 4.1 serial mouse bridge. |
| `TEENSY41_HID` | Teensy 4.1 RawHID control path. |
| `KMBOX_NET` | Network kmbox control. |
| `KMBOX_A` | kmbox A serial/HID style control. |
| `MAKCU` | MAKCU serial control. |

Some applications ignore Win32 synthetic input. Razer and Teensy do not fall back to another method when unavailable. See [input setup and checks](guides/input-methods.md).

## Wind Mouse

| Key | Default | Meaning |
|---|---:|---|
| `wind_mouse_enabled` | `false` | Enables wind-mouse style movement. |
| `wind_G` | `18.0` | Gravity term. |
| `wind_W` | `15.0` | Wind term. |
| `wind_M` | `10.0` | Max step term. |
| `wind_D` | `8.0` | Distance term. |

## Device Control Sections

### Arduino

| Key | Default | Meaning |
|---|---:|---|
| `arduino_baudrate` | `115200` | Serial baud rate. |
| `arduino_port` | `COM0` | Serial port. |
| `arduino_16_bit_mouse` | `false` | Use 16-bit mouse movement protocol. |
| `arduino_enable_keys` | `false` | Allow key/button handling through Arduino. |

### RP2350

| Key | Default | Meaning |
|---|---:|---|
| `rp2350_baudrate` | `115200` | Serial baud rate. |
| `rp2350_port` | `COM0` | Serial port. |
| `rp2350_16_bit_mouse` | `true` | Use 16-bit mouse movement protocol. |
| `rp2350_enable_keys` | `false` | Allow key/button handling through RP2350. |

### Teensy 4.1 RawHID

For `input_method = TEENSY41_HID`.

| Key | Default | Meaning |
|---|---:|---|
| `teensy_hid_serial` | `AUTO` | Serial filter. Use `AUTO` to skip serial filtering. |
| `teensy_hid_vid_filter` | `AUTO` | Vendor ID filter. Use `AUTO` to skip VID filtering. |
| `teensy_hid_pid_filter` | `AUTO` | Product ID filter. Use `AUTO` to skip PID filtering. |
| `teensy_hid_usage_page` | `65451` | HID usage page, decimal form of `0xFFAB`. |
| `teensy_hid_usage_id` | `512` | HID usage ID, decimal form of `0x0200`. |
| `teensy_hid_open_index` | `0` | Which matching HID device to open if multiple match. |
| `teensy_hid_packet_timeout_ms` | `2` | Packet write/read timeout. |
| `teensy_hid_reconnect_interval_ms` | `500` | Reconnect interval after device loss. |

The Teensy path sends report-ID-prefixed 64-byte packets and expects matching firmware.

### Kmbox Net

| Key | Default | Meaning |
|---|---:|---|
| `kmbox_net_ip` | `10.42.42.42` | Device IP address. |
| `kmbox_net_port` | `1984` | Device port. |
| `kmbox_net_uuid` | `DEADC0DE` | Device UUID/token. |

### Kmbox A

| Key | Default | Meaning |
|---|---:|---|
| `kmbox_a_pidvid` | empty | Combined PID/VID string in `PPPPVVVV` format. |

### MAKCU

| Key | Default | Meaning |
|---|---:|---|
| `makcu_baudrate` | `115200` | Serial baud rate. |
| `makcu_port` | `COM0` | Serial port. |

## Mouse Shooting

| Key | Default | Meaning |
|---|---:|---|
| `auto_shoot` | `false` | Enables automatic shooting behavior. |
| `bScope_multiplier` | `1.0` | Scope multiplier. Missing-key fallback is `1.2` for older configs. |

## AI

| Key | Default | Meaning |
|---|---:|---|
| `backend` | `TRT` in CUDA, `DML` in DML | Inference backend. |
| `dml_device_id` | `0` | DirectML device index. |
| `ai_model` | CUDA: `sunxds_0.5.6.engine`, DML: `sunxds_0.5.6.onnx` | Model file. Missing-key fallback currently uses `sunxds_0.8.0.*` for older configs. |
| `confidence_threshold` | `0.10` | Minimum detection confidence. Missing-key fallback is `0.15`. |
| `nms_threshold` | `0.50` | Non-max suppression threshold. |
| `max_detections` | `100` | Maximum detections kept per frame. Missing-key fallback is `20`. |
| `export_enable_fp8` | `false` in generated CUDA config | TensorRT export option, CUDA builds only. Missing-key fallback is `true`. |
| `export_enable_fp16` | `true` in CUDA | TensorRT export option, CUDA builds only. |

## CUDA

These keys are written only in CUDA builds.

| Key | Default | Meaning |
|---|---:|---|
| `use_cuda_graph` | `false` | Enables CUDA graph path where supported. |
| `use_pinned_memory` | `false` | Generated default for pinned memory. Missing-key fallback is `true`. |
| `gpuMemoryReserveMB` | `2048` | GPU memory reserve. |
| `enableGpuExclusiveMode` | `true` | Enables exclusive GPU mode behavior where supported by the app. |
| `capture_use_cuda` | `true` | Allows CUDA capture path usage. |

CUDA capture can still create CPU copies when preview, debugging, data collection, or other CPU-readable features need pixels.

## System

| Key | Default | Meaning |
|---|---:|---|
| `cpuCoreReserveCount` | `4` | CPU cores to avoid using heavily. |
| `systemMemoryReserveMB` | `2048` | System memory reserve. |

## Buttons

| Key | Default | Meaning |
|---|---:|---|
| `button_targeting` | `RightMouseButton` | Aim/targeting button list. |
| `button_shoot` | `LeftMouseButton` | Shoot button list. |
| `button_zoom` | `RightMouseButton` | Zoom/scope button list. |
| `button_exit` | `F2` | Exit hotkey. |
| `button_pause` | `F3` | Pause hotkey. |
| `button_reload_config` | `F4` | Reload application settings and workflow graphs. |
| `button_open_overlay` | `Home` | Open overlay hotkey. |
| `enable_arrows_settings` | `false` | Enables arrow-key settings behavior. |

Use `None` to disable a button where supported.

## Overlay

| Key | Default | Meaning |
|---|---:|---|
| `overlay_opacity` | `225` | Overlay opacity, `0..255`. |
| `overlay_ui_scale` | `1.0` | Overlay UI scale. |
| `overlay_sidebar_collapsed` | `false` | Show only sidebar icons. The sidebar chevron toggles this preference; changes save automatically. |
| `overlay_exclude_from_capture` | `true` | Attempts to keep overlay out of captured frames. |
| `overlay_x` | `0` | Overlay editor window X position. Auto-saved after moving. |
| `overlay_y` | `0` | Overlay editor window Y position. Auto-saved after moving. |
| `overlay_width` | `1180` | Preferred editor window width. Limited to the monitor work area and auto-saved after resizing. |
| `overlay_height` | `740` | Preferred editor window height. Limited to the monitor work area and auto-saved after resizing. |

## Depth

| Key | Default | Meaning |
|---|---:|---|
| `depth_inference_enabled` | `true` | Enables depth inference feature. |
| `depth_model_path` | `depth_anything_v2.engine` | Depth model path. |
| `depth_fps` | `100` | Depth update FPS. Minimum `0`. |
| `depth_colormap` | `18` | OpenCV colormap index. Clamped `0..21`. |
| `depth_mask_enabled` | `false` | Enables depth mask. |
| `depth_mask_fps` | `5` | Depth mask update FPS. Minimum `0`. |
| `depth_mask_near_percent` | `20` | Near-depth percent. Clamped `1..100`. |
| `depth_mask_expand` | `0` | Expand mask pixels. Clamped `0..128`. |
| `depth_mask_hold_frames` | `0` | Hold mask for extra frames. Clamped `0..120`. |
| `depth_mask_alpha` | `90` | Mask alpha. Clamped `0..255`. |
| `depth_mask_invert` | `false` | Inverts depth mask. |
| `depth_debug_overlay_enabled` | `false` | Shows depth debug overlay. |

## Game Overlay

| Key | Default | Meaning |
|---|---:|---|
| `game_overlay_enabled` | `false` | Enables in-game overlay rendering. |
| `game_overlay_max_fps` | `0` | Overlay FPS cap. `0` means uncapped/default behavior. |
| `game_overlay_draw_boxes` | `true` | Draw detection boxes. |
| `game_overlay_compensate_latency` | `true` | Shifts overlay boxes/icons using frame age and mouse movement recorded after capture. |
| `game_overlay_draw_future` | `true` | Draw predicted future positions. |
| `game_overlay_draw_wind_tail` | `true` | Draw wind mouse trail. |
| `game_overlay_draw_frame` | `true` | Draw frame border. |
| `game_overlay_draw_circle_fov` | `true` | Draw Circle FOV in the game overlay. |
| `game_overlay_show_target_correction` | `true` | Draw target correction indicator. |
| `game_overlay_box_a/r/g/b` | `255/0/255/0` | Box color as alpha/red/green/blue. |
| `game_overlay_frame_a/r/g/b` | `180/255/255/255` | Frame color as alpha/red/green/blue. |
| `game_overlay_box_thickness` | `2.0` | Detection box line thickness. |
| `game_overlay_frame_thickness` | `1.5` | Frame line thickness. |
| `game_overlay_future_point_radius` | `5.0` | Future-point radius. |
| `game_overlay_future_alpha_falloff` | `1.0` | Future-point alpha falloff. |
| `game_overlay_icon_enabled` | `false` | Enables drawing an icon marker. |
| `game_overlay_icon_path` | `icon.png` | Icon file path. |
| `game_overlay_icon_width` | `64` | Icon width. |
| `game_overlay_icon_height` | `64` | Icon height. |
| `game_overlay_icon_offset_x` | `0.0` | Icon X offset. |
| `game_overlay_icon_offset_y` | `0.0` | Icon Y offset. |
| `game_overlay_icon_anchor` | `center` | Icon anchor: `center`, `top`, `bottom`, or `head`. |
| `game_overlay_icon_class` | `-1` | Class to draw icon for. `-1` means all. |

## Data Collection

| Key | Default | Meaning |
|---|---:|---|
| `collect_data_while_playing` | `false` | Save data while running. |
| `collect_only_when_aimbot_running` | `false` | Collect only while the aimbot is actively running. |
| `collect_only_when_targets_present` | `true` | Collect only frames with targets. |
| `collect_save_every_n_frames` | `15` | Save interval. Clamped `1..600`. |
| `collect_jpeg_quality` | `95` | JPEG quality. Clamped `50..100`. |
| `collect_output_dir` | empty | Output folder. |
| `auto_label_data` | `true` | Write labels automatically. |
| `auto_label_min_conf` | `0.30` | Auto-label confidence. Clamped `0.01..0.99`. |
| `auto_label_max_boxes` | `20` | Auto-label box limit. Clamped `1..200`. |
| `auto_label_record_classes` | empty | Optional class filter list. |

Data collection needs CPU-readable frames, so it can change capture performance diagnostics.

## Classes

| Key | Default | Meaning |
|---|---:|---|
| `class_player` | `0` | Model class ID for player/body detections. |
| `class_head` | `1` | Model class ID for head detections. |

## Debug

| Key | Default | Meaning |
|---|---:|---|
| `screenshot_button` | `None` | Screenshot hotkey. |
| `screenshot_delay` | `500` | Screenshot delay in milliseconds. |
| `verbose` | `false` | Enables more logging. |


## Game Profiles

On migration, alternative `[Games]` settings become disabled Mouse calibration nodes. Edit calibration inputs directly in the graph; aiming and recoil share the same named source. Legacy entry format: `name = sensitivity,yaw,pitch[,fovScaled,baseFOV]`.

```ini
active_game = UNIFIED

[Games]
UNIFIED = 1,0.022,0.022
MY_GAME = 2.5,0.02,0.02,true,90
```

If `active_game` is missing or invalid, the app falls back to an available profile.
