# Overlay and GUI Behavior

- `show_window` controls the debug preview, which may require CPU frame copies.
- Render Overlay controls detection boxes, predictions, frame outlines, Circle FOV and icons.

```ini
overlay_exclude_from_capture = true
game_overlay_enabled = false
game_overlay_compensate_latency = true
game_overlay_draw_circle_fov = true
```

Overlay settings seed the graph on first launch; edit an existing Render Overlay node directly. Compare performance with the preview open and closed.

## Editor Window Placement

The editor fits the monitor's work area and DPI. Saved geometry is reused when it fits; oversized windows shrink, and a disconnected monitor falls back to the screen containing the cursor. Limits update after display changes.

## Latency Compensation

`game_overlay_compensate_latency` adjusts boxes and icons for frame age and mouse movement. Disable it if the visual correction is inaccurate.

See [capture diagnostics](capture-diagnostics.md) and [Circle FOV](circle-fov.md).
