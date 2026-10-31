# Node graphs

The editor has four graphs, initially populated from existing settings:

| Graph | Default pipeline |
| --- | --- |
| Aim Graph | Detections → target selection → aim point → prediction → speed → mouse movement |
| Auto Shoot | Target observation and scope checks → shooting conditions → button output |
| Recoil | Shoot/zoom conditions → enable and strength → recoil output |
| Vision | Capture → optional depth/mask → inference → filters → detections for the control graphs |

Logic runs through these graphs. A branch executes when an active output needs it. Collect Dataset, Render Overlay and Screenshot can run independently of Vision Output. Only one capture source can be active; depth inference requires CUDA. Provider errors appear on the affected node.

## Editing

- **Move and select:** left-drag nodes or group headers; drag empty canvas with LMB/MMB to pan. Right-drag selects a box; Shift adds to the selection. Shift/Ctrl+click toggles individual nodes. The wheel zooms around the cursor.
- **Connect:** drag between compatible ports in either direction. A new wire replaces the existing input connection. Drop on empty canvas to add a compatible node, or use RMB/Esc to cancel. Alt+click a port disconnects it; double-click a wire cuts it.
- **Add nodes:** short right-click opens the context menu. Hover categories and subcategories, or search by name. Toolbar icons show tooltips with their actions and shortcuts.
- **Edit values:** all runtime settings have visible typed inputs. Click Bool to toggle, drag Int/Float or Ctrl+click for exact entry, and click String to edit. Choices, keys and file pickers are available at the input. A wire overrides the stored local value until disconnected. Source literals and Get/Set names use local fields.
- **Read defaults:** a dash means required data is missing; Center, Local and None identify contextual or optional inputs. Hover for details. Changed values and consumed connections briefly highlight; stable data does not create pulses.
- **Resize and arrange:** select a card and drag its edges/corners. Nodes snap to the 32-pixel grid. The Size menu offers automatic, compact, wide and fit-height presets. Arrange graph orders nodes by connections and fits their groups. RMB/Esc cancels a move or resize.
- **Groups:** group selected nodes into a named, colored frame. Drag its header to move the contents; resize or remove the frame without deleting its nodes.
- **Reroute:** right-click a wire → Insert reroute, then drag the dot to guide the wire. Its side handles appear near the pointer.
- **Wire ties:** hold Z and drag across wires to gather their routes under a strap. Shift+drag rotates it in 15-degree steps. Double-click/Delete removes the strap while keeping its connections.
- **Bypass and disable:** bypass passes through a compatible input; bypassing an output stops its action. Disable makes the output unavailable. Unused upstream branches stop executing.
- **Collapse and zoom:** the header chevron folds a card. Zooming out shows large node names while preserving card dimensions and port positions. View options include Automatic, Detailed and Compact.
- **Sidebar:** its chevron switches between labels and icons. The preference saves automatically.

Double-click a node or press Enter for its description, live data and status. Click **?** for controls and the signal-color legend.

## Data and settings nodes

Ports have distinct types and colors. Int uses signed 32-bit values and connects directly to Float; use Float to Int for the reverse conversion and choose its rounding mode. Invalid types, cycles, overflow and division by zero stop the affected branch.

Select a monitor with `Get Monitors → Monitor At → Capture Source`. Monitor At takes a zero-based list position; Primary Monitor selects the primary screen. Connect Capture Source's Frame output to Inference, Collect Dataset or Screenshot. A connected Monitor controls the capture target, device and index. Source settings can change before the first frame arrives.

String, Join Text, Int to String and Float to String build paths and textual settings. Select nodes evaluate only the chosen branch. Dead Zone, Lerp Point, Scale/Limit Movement, Pulse, Key Held/Pressed/Released and Toggle provide common transforms and conditions; key 0 means None.

For a named dependency, give a Set and its Gets the same name within one graph. One writer is allowed per name; types must match. Gets read the current evaluation, not a value retained between ticks. Copying a Set with its Gets creates an independent pair; copying a Get alone keeps its reference.

## Persistence and runtime

Edits apply immediately and autosave to `node_graphs.ini` beside `ai.exe`. Undo/Redo restores edits. Unconfirmed move/resize previews are cancelled when leaving the editor. Restore Default replaces only the current graph.

Import/Export transfers all four graphs in a `.sngraph` file, including values, paths, connections, groups, layout and node states. Model files are not embedded. The file limit is 4 MiB; older formats migrate on load. If loading fails, autosave pauses to preserve the original file until a valid import or explicit restore.

`config.ini` holds devices, application hotkeys and menu preferences. Existing graph settings are edited in nodes. While the menu has input focus, graph gestures do not trigger control actions; Exit, Pause and the menu hotkey remain available.

## Keyboard controls

Canvas shortcuts pause while editing a field or using a popup.

| Shortcut | Action |
| --- | --- |
| Shift+A / Ctrl+Space | Open node library |
| G; Shift+D | Move selection; duplicate and move |
| Enter/LMB; Esc/RMB | Confirm; cancel move or connection |
| A / Ctrl+A; Alt+A; Ctrl+I | Select all; deselect all; invert selection |
| B | Box selection with the next LMB drag |
| Home; F / Numpad . | Frame graph; frame selection |
| Alt+L | Arrange graph |
| Arrows; Shift+Arrows | Move selection one; four grid cells |
| Tab / Shift+Tab | Focus ports; move to next/previous node |
| Up/Down; Home/End with port focus | Select port; first/last port |
| Left/Right with port focus | Follow upstream/downstream connection |
| Enter/Space with port focus | Start or complete a connection |
| Delete/Backspace with port focus | Disconnect port |
| Escape | Cancel gesture or leave port navigation |
| Ctrl+B; Ctrl+M | Bypass; disable/enable |
| Ctrl+G; Space | Group; collapse/expand |
| Ctrl+C / X / V; Ctrl+D | Copy/cut/paste; duplicate |
| Ctrl+Z; Ctrl+Y / Ctrl+Shift+Z | Undo; redo |
| X / Delete | Delete selection |
| Z+LMB drag; Ctrl+RMB drag | Tie wires; cut wires |
| Enter; Ctrl+S | Node information; flush autosave |

## Preview and performance checks

After a [wrapper build](../build.md), run:

```powershell
.\ai.exe --node-editor-preview
.\node_editor_input_tests.exe --benchmark benchmark_results\node_editor.csv
```

Run these from `build\dml\Release` or `build\cuda\Release`. Preview uses synthetic data without capture, inference or device input; F2 closes it by default. The offscreen UI benchmark measures 128 nodes and 256 connections at 1080p/1440p, including frequent updates, routing and editing. CSV results include CPU/GPU frame times and remain outside Git.
