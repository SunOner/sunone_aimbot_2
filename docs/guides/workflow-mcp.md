# Workflow MCP

Start `ai.exe`, then open **System → Agent access**. Choose **Claude Code**, **Claude Desktop**, **Codex**, or **Other MCP client**. The connector requires Python 3.10+ and no extra packages; wrapper builds place `workflow_mcp.py` beside `ai.exe`.

- **Claude Code:** copy the install command and run it in PowerShell. It registers this instance in user scope. Alternatively merge the copied JSON into `.mcp.json`.
- **Claude Desktop:** merge the copied `mcpServers` entry through **Developer → Edit Config**, preserving other entries, then restart Claude.
- **Codex:** run the copied command in PowerShell, or add the copied TOML to `~/.codex/config.toml`. Restart the MCP connection. The same configuration is used by local Codex clients.

Commands quote Windows paths containing spaces or apostrophes. Each application instance gets its own server name. No client configuration or permission policy is changed by copying a preset.

Formats follow the official [Codex MCP documentation](https://learn.chatgpt.com/docs/extend/mcp) and [Claude Code MCP documentation](https://code.claude.com/docs/en/mcp). Claude Desktop also supports [local desktop extensions](https://support.claude.com/en/articles/10949351-getting-started-with-local-mcp-servers-on-claude-desktop).

Access is enabled by default. The switch autosaves as `local_mcp_enabled` in `config.ini`, survives restart, and follows application reloads.

```json
{
  "mcpServers": {
    "sunone-workflows": {
      "command": "python",
      "args": ["C:/path/to/build/dml/Release/workflow_mcp.py"]
    }
  }
}
```

The adapter implements MCP stdio with protocol `2025-11-25` and older initialization-based versions. Modern dual-era clients can fall back to initialization. Stdout contains only JSON-RPC messages. Closing the connector leaves the application running.

The app uses a local Windows named pipe restricted to the current user and rejects remote connections. No TCP listener, shell commands or arbitrary filesystem tools are exposed. For several application instances, give each a different `SUNONE_MCP_INSTANCE` environment value and use the same value in its connector's environment (letters, digits, `_`, `-`; up to 64 characters). An occupied endpoint is reported in System rather than attaching to another instance silently.

| Tool | Purpose |
| --- | --- |
| `get_catalog` | All node kinds, port types, defaults, ranges, choices and limits |
| `get_sources` | Available model paths and monitor indexes/devices |
| `get_workspace` | Workflows, IDs, nodes, settings, wires, groups and layout |
| `edit_workflows` | An atomic batch of edits, one Undo step, immediate autosave |
| `validate_workflows` | Validate and normalize a proposed batch without applying it |
| `import_workflows` | Replace the workspace or append workflows from structured data |
| `export_workflows` | Structured workspace and `.sngraph` text; models remain paths |
| `get_diagnostics` | Cached live values, errors, activity and sample freshness |
| `editor_action` | Undo, redo, or focus a workflow/node |
| `get_application` | Build capabilities, pause/window state, global settings with ranges and valid hotkeys |
| `edit_application` | Atomic global settings update, immediate autosave, separate version and dry run |
| `application_action` | Pause/resume, show/hide, exact window mode, application tab selection |
| `model_export` | Start, inspect or cancel the Export tab's asynchronous TensorRT job (CUDA) |

The server provides agent instructions during initialization and an MCP resource, `sunone://agent-guide`. Start with `get_application`, `get_workspace(summary=true)` and `get_catalog(query="...")` to discover the app without filling the agent context with every node. `edit_workflows(response="summary")` returns the version, created IDs and workflow counts instead of the whole workspace.

Example request to an agent: **“Inspect the available models and my Vision workflow, fix disconnected inputs, arrange the nodes and verify fresh diagnostics. Keep processing paused until the graph is ready.”** The agent can perform this through MCP without editing project source or simulating mouse input.

Global settings use `expected_settings_version` from `get_application`. Only application-owned settings are accepted: devices, hotkeys, overlay and startup resources. Capture, inference and control options remain graph-owned. Startup changes are returned in `restart_required`; they take effect after the next app launch. These settings have no graph Undo; retain their previous values when you need to restore them.

Window and pause actions are queued; read `get_application` again for the resulting state. Setting an existing pause state or window mode does not toggle it. `model_export` uses the same job and cancellation as the Export tab. A finished job returns its output path; choose it in the inference node explicitly. A running export cannot be silently replaced.

Read the catalog and workspace before editing. Pass the returned opaque `version` as `expected_version`; it includes layout changes. Conflicts and invalid batches apply nothing. Requests are executed on the editor thread, including while the menu is hidden. An active user interaction returns a busy error. Read and retry after it ends.

Example arguments for `edit_workflows`:

```json
{
  "expected_version": "VERSION_FROM_GET_WORKSPACE",
  "operations": [
    {"op": "workflow.create", "id": 5, "kind": 0, "fields": {"name": "Custom logic"}},
    {"op": "node.add", "workflow": 5, "id": 1, "kind": 12, "fields": {"x": 64, "y": 64, "value": true}},
    {"op": "layout.arrange", "workflow": 5}
  ],
  "dry_run": true
}
```

Use catalog IDs instead of assuming the example's kind ID. Explicit IDs let later operations in a batch refer to new nodes. Node fields include `value`, `parameters`, `text_parameters`, `input_values`, `input_texts`, geometry, collapse, bypass and disable. Map fields merge; `null` removes an override. Links use `from_node`, `from_port`, `to_node`, `to_port`. Connecting replaces an existing connection to that input. Groups follow geometry; wire ties accept `angle` snapped to 15 degrees. The tool descriptions list every operation and editable field.

Applied changes are live and can activate graph outputs. `dry_run` does not execute them. A successful response reports `applied` and `saved` separately: a disk-write failure preserves the live edit and Undo and returns a warning. If initial loading failed, `import_workflows` with `recover=true` explicitly allows replacing the invalid workspace.

Diagnostics request a one-second telemetry lease without opening the menu or requesting image pixels. Read again after a runtime tick if `current` is false; stale or absent samples are not evidence of successful execution. Preview mode evaluates synthetic data only and stops ticking while its window is hidden.

To diagnose a connection, run `python workflow_mcp.py --check` beside the executable (add `--instance NAME` for a custom instance). It reports connection/build/window state and exits. A local stdio client must run on the same Windows host and user as the application; a cloud-only chat cannot reach this pipe directly.
