# Input Methods

Set `input_method` to match the connected device or runtime:

```text
WIN32, GHUB, RAZER, ARDUINO, RP2350, TEENSY41, TEENSY41_HID, KMBOX_NET, KMBOX_A, MAKCU
```

Razer and Teensy do not fall back to another method when unavailable. Device parameters are listed in [configuration](../config.md#input-method).

## When WIN32 Does Not Move In Game

WIN32 sends Windows synthetic mouse events, which some applications ignore. If detections are visible but movement is missing:

1. Check the selected method in console logs, such as `[Mouse] Using WIN32 input.`
2. Test movement on the desktop.
3. If only the game rejects it, check compatibility with your selected method and device.
4. For device methods, confirm the device is connected and its logs show an open state.

## Razer Setup

```ini
input_method = RAZER
```

Place the x64 `rzctl.dll` beside `ai.exe` or at `sunone_aimbot_2\rzctl.dll`. Check `[Razer]` logs for loading or export errors.

## Teensy 4.1 RawHID Setup

```ini
input_method = TEENSY41_HID
teensy_hid_serial = AUTO
teensy_hid_vid_filter = AUTO
teensy_hid_pid_filter = AUTO
teensy_hid_usage_page = 65451
teensy_hid_usage_id = 512
teensy_hid_open_index = 0
```

Firmware must support report-ID-prefixed 64-byte RawHID packets. Start with AUTO filters; use `teensy_hid_open_index` to choose between matching devices. Check `[Mouse] Using TEENSY41_HID input.` in the console.

| Button ID | Function |
| ---: | --- |
| 1 | Shoot / left mouse |
| 2 | Zoom / right mouse |
| 5 | Aim / side button |
