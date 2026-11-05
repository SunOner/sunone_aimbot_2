# Node graphs

The editor starts with four processing workflows:

| Graph | Default pipeline |
| --- | --- |
| Aim Graph | Detections → target selection → aim point → prediction → speed → mouse movement |
| Auto Shoot | Target observation and scope checks → shooting conditions → button output |
| Recoil | Shoot/zoom conditions → enable and strength → recoil output |
| Vision | Capture → optional depth/mask → inference → filters → detections for the control graphs |

The former capture, target/tracker, mouse tuning/profile, depth and game-overlay settings panels have been removed. Their replacements are graph nodes.

Logic runs through these graphs. A branch executes when an active output needs it. Collect Dataset, Render Overlay and Screenshot can run independently of Vision Output. Only one capture source can be active; depth inference requires CUDA. Provider errors appear on the affected node.

## Workflows

The four starting workflows use the standard layout, including groups, reroutes, wire ties and bends. First launch, Restore Default and filled template copies use that layout. Dataset and screenshot nodes remain available in the library. Right-click a sidebar item to change its name/icon, enable it or remove it. **+** creates an empty Logic/Vision workflow or a filled template copy. Additional Vision templates use Frame input when capture already exists.

Each sidebar item opens one graph. There are no page tabs or profile workflows. Workflows support Undo/Redo and autosave; up to 64 workflows are supported. Legacy extra pages become separate disabled sidebar workflows, preserving their graphs and names.

Logic workflows can use Aim, Auto Shoot and Recoil outputs together. Vision workflows share one physical capture source; **Frame input** reads its current frame without starting another capture. Connect it to processing, recording or preview nodes as needed.

**Export** builds object/depth TensorRT engines in the background, with progress and cancellation. Select the resulting engine in an inference node. Input Device, Hotkeys, Overlay, System, Stats and Debug remain application tabs. System resource reservations apply on the next launch. Debug shows active node errors, recording counters and tracker diagnostics, with links back to nodes.

## Editing

- **Move and select:** left-drag nodes or group headers. Items follow the cursor freely; a background outline shows their grid placement on release. Reroutes and wire ties snap their visible centers to grid intersections. RMB/Esc cancels. Click empty canvas with LMB to deselect; drag it with LMB/MMB to pan while keeping the selection. Right-drag selects a box; Shift adds to the selection. Shift/Ctrl+click toggles individual nodes. The wheel zooms around the cursor.
- **Connect:** drag between compatible ports in either direction. Nearby compatible sockets enlarge and smoothly attract the wire; release anywhere in that capture area to connect. A new wire replaces the existing input connection. Drop on empty canvas to add a compatible node, or use RMB/Esc to cancel. Alt+click a port disconnects it; double-click a wire cuts it.
- **Bend wires:** hover a wire to reveal its center handle. Drag horizontally for roundness and vertically for twist; Shift+drag gives finer control. Alt+click the handle or choose Reset bend from the wire menu to reset. RMB/Esc cancels a drag. Wires do not automatically detour around nodes; bends persist with the graph, clipboard and Undo.
- **Add nodes:** RMB, Shift+A or Ctrl+Space opens the library with search focused. Type a name, use Up/Down to select, and Tab/Enter to add at the requested position. Hovering or selecting a node shows a read-only node card with all its ports, fields and defaults beside the menu. Long text wraps; large previews scroll with the wheel or scrollbar. Esc closes search; Down on an empty search browses all nodes. Exact names and abbreviations rank first. A menu opened from a wire shows compatible nodes and connects the chosen result automatically. Categories remain available when search is empty.
- **Edit values:** all runtime settings have visible typed inputs. Click Bool to toggle, drag Int/Float or Ctrl+click for exact entry, and click String to edit. At 100% zoom or closer, clicking a numeric value enables wheel editing while the cursor stays on that field; Shift gives finer Float steps, Ctrl speeds up changes. Moving away, zooming out or pressing Esc disables wheel editing. Before activation the wheel zooms the graph. Choices, keys and file pickers are available at the input. A wire overrides the stored local value until disconnected. Source literals and Get/Set names use matching single-row local fields in the graph and node preview.
- **Read defaults:** a dash means required data is missing; Center, Local and None identify contextual or optional inputs. Hover for details. Changed values and consumed connections briefly highlight; stable data does not create pulses.
- **Resize and arrange:** select a card and drag its edges/corners. Card positions and dimensions align to the 32-pixel grid, including automatic sizes and collapsed cards. The Size menu offers automatic, compact, wide and fit-height presets. Arrange graph wraps long chains and fits their stage groups. RMB/Esc cancels a resize.
- **Groups:** group selected nodes into a named, colored frame. Drag its header to move its nodes and nested frames; resize or remove the frame without deleting its nodes. Drag a node outside to detach it, or fully inside to attach it to the innermost frame. Node-only moves do not carry the frame.
- **Reroute:** double-click empty canvas to add a point, or right-click a wire → Insert reroute. A loose point adopts its first connection's type; connected points keep their type. Drag the dot to move it; side handles appear near the pointer. Deleting points reconnects their source to all surviving destinations, including when deleting a chain at once. Points created inside a group join that group.
- **Wire ties:** hold Z and drag across wires to gather their routes under a strap. Shift+drag rotates it in 15-degree steps. Double-click/Delete removes the strap while keeping its connections.
- **Bypass and disable:** bypass passes through a compatible input; bypassing an output stops its action. Disable makes the output unavailable. Unused upstream branches stop executing.
- **Collapse and zoom:** the header chevron folds a card. Zooming out shows large node names and group titles while preserving card dimensions and port positions. Group titles appear above their frames and remain draggable. View options include Automatic, Detailed and Compact.
- **Sidebar:** its chevron switches between labels and icons. The preference saves automatically.

Double-click a node or press Enter for its description, live data and status. Click **?** for controls and the signal-color legend.

## Data and settings nodes

Target processing is explicit in the starter graphs: Select target → Target position / size / class → Aim offsets → Point in box → Camera compensation. Estimate motion updates one filter per observation; Filtered position, Velocity, Velocity damping and Motion ready read that shared sample. Prediction time and Project point calculate the lead; Future trail supplies Overlay trails. Replace the velocity or time wire to use a different prediction rule. Scope uses box geometry and a separate observation gate.

Older graphs expand these stages on import when their settings can be preserved and node/link limits allow it. Combined nodes remain under Compatibility for advanced legacy selection, bypassed nodes and full graphs. New and migrated stages use the grid layout; values, input wires and disabled states carry over.

**Render overlay** takes a Style chain: Overlay boxes → Frame / FOV → Trails → Icon → Depth / mask → Correction. Use only the layers you need; each has editable inputs. Dataset labels connects separately to Collect dataset. Existing outputs split automatically on load, preserving settings and wires; graphs at the node/link limit retain compatible legacy cards.

**Preview** is under Utilities → Inspection in every workflow. Its **Data** input accepts any type: video/image frames, depth, masks, text, numbers and structured values. Existing Frame/Depth/Mask connections keep working. Detections adds matching boxes to images. Up to four visible previews update at once, at 15 FPS by default; unchanged data reuses its cached preview. Collapsed, offscreen and hidden cards stop requesting updates and pixels.

Text wraps without abbreviation. Scroll inside the panel with the wheel; Ctrl+wheel zooms the graph. **Copy preview value** in the node menu copies full text. Large detection/movement lists show the first 64 entries and the remaining count to bound formatting cost.

Mouse movement is **Point delta → Pixels to counts → Apply speed → Normalize FPS**. Precise counts survive the intermediate stages; calibration stays attached to Movement for camera compensation and trails.

**Mouse calibration** owns FOV, sensitivity, yaw, pitch and FOV scaling in Aim Graph. **Shared calibration** reads it by shared name (`mouse` by default); recoil uses Calibration → Pixels per count. Edit the source inputs to update both consumers immediately, or place the source in another enabled Logic workflow. Disabled, missing, ambiguous or cyclic sources stop only dependent branches.

The former generated Profiles container folds into an existing Logic graph when space permits. Alternative calibration pages remain disabled ordinary workflows; older INI alternatives are retained as disabled calibration nodes. Full graphs use shared combined cards; custom bindings that cannot be moved losslessly retain their compatible local nodes. Zero sensitivity stops aim movement while recoil retains the original raw-count feedback fallback. Runtime reads no mouse tuning from INI.

Collect dataset's node menu and information panel provide recording counters, Copy recording folder and Reset counters. Reset affects only that recorder and preserves its sampling cadence and filename sequence.

Ports have distinct types and colors. Int uses signed 32-bit values and connects directly to Float; use Float to Int for the reverse conversion and choose its rounding mode. Invalid types, cycles, overflow and division by zero stop the affected branch.

Object inference and Depth inference offer a model dropdown from `models`. Depth also lists `depth_models`. Reopening refreshes the list; Choose file and Custom path allow other locations. Selection applies immediately, and a connected input overrides the local choice.

Select a monitor with `Get Monitors → Monitor At → Capture Source`. Monitor At takes a zero-based list position; Primary Monitor selects the primary screen. Connect Capture Source's Frame output to Inference, Collect Dataset or Screenshot. A connected Monitor controls the capture target, device and index. Source settings can change before the first frame arrives.

Speed curve and Scope check accept a Center input; unwired centers use the capture center. Speed curve also exposes its maximum distance. Older option wires and manual card sizes migrate automatically.

String, Join Text, Int to String and Float to String build paths and textual settings. Select nodes evaluate only the chosen branch. Dead Zone, Lerp Point, Scale/Limit Movement, Pulse, Key Held/Pressed/Released and Toggle provide common transforms and conditions; key 0 means None.

Toggle keeps its latch when unrelated options or workflows change. Rebinding a trigger, restoring an unavailable input, or resuming an unchosen branch samples the current input without inventing a key edge. A new observed edge rearms the event. Runtime errors retain their original cause through dependent nodes and clear when the input recovers.

For a named dependency, give a Set and its Gets the same name within one graph. One writer is allowed per name; types must match. Gets read the current evaluation, not a value retained between ticks. Copying a Set with its Gets creates an independent pair; copying a Get alone keeps its reference.

## Persistence and runtime

[Workflow MCP](workflow-mcp.md) lets local agents inspect, validate and edit complete workflows through atomic, versioned transactions with Undo and autosave.

Edits apply immediately and autosave to `node_graphs.ini` beside `ai.exe`. Undo/Redo restores edits. Unconfirmed move/resize previews are cancelled when leaving the editor. Restore Default replaces only the current graph.

Import/Export transfers all workflows in a `.sngraph` file, including values, paths, connections, groups, layout and node states. Model files are not embedded. The file limit is 4 MiB; older formats migrate on load. If loading fails, autosave pauses to preserve the original file until a valid import or explicit restore.

Reload (F4 by default) reads application settings and workflows, preserving graph ownership of capture and tuning values. Invalid graph files leave the last valid workspace running and pause autosave.

Arrow tuning affects only enabled workflows and unwired local defaults; changes support Undo and autosave while the menu is hidden. Key nodes read physical device buttons when available and ignore injected mouse feedback.

`config.ini` holds devices, application hotkeys, startup resources and menu preferences. Existing graph settings are edited in nodes. While the menu has input focus, graph gestures do not trigger control actions; Exit, Pause and the menu hotkey remain available.

## Keyboard controls

Canvas shortcuts pause while editing a field or using a popup.

| Shortcut | Action |
| --- | --- |
| Shift+A / Ctrl+Space | Open node library |
| Up/Down in node search | Select result; Down with no query browses all nodes |
| Tab / Enter in node search; Esc | Add selected result; close search |
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
