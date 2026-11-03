# Overlay and GUI Behavior

- Preview nodes show capture, depth and masks. Visible previews request CPU pixels only when needed.
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

Hide, Maximize (keeps the taskbar visible), and Fullscreen stay at the top right on every tab, above scrolling settings. Graph help sits immediately to their left. Hide and Alt+F4 dismiss the editor; the overlay hotkey reopens it with its layout and window mode preserved. The editor does not appear in the taskbar or Alt+Tab. F11 toggles fullscreen and returns to the previous window mode. Click an active mode again to restore the normal window size. Narrow graph toolbars move secondary actions into the tools menu.

## Latency Compensation

`game_overlay_compensate_latency` adjusts boxes and icons for frame age and mouse movement. Disable it if the visual correction is inaccurate.

See [capture diagnostics](capture-diagnostics.md) and [Circle FOV](circle-fov.md).
