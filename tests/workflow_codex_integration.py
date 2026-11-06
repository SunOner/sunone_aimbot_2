"""Real Codex CLI -> stdio MCP -> isolated synthetic editor acceptance test.

Requires an authenticated local Codex CLI. Uses its auth without modifying its
config or creating persistent chats. Run after build_dml.bat/build_cuda.bat.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

from workflow_mcp_integration import wait_for_preview, stop_preview


def codex_command():
    executable = shutil.which("codex")
    if not executable:
        raise RuntimeError("Install/authenticate Codex CLI before running this acceptance test")
    # Invoke the npm entry directly to avoid cmd.exe reinterpreting TOML quotes.
    entry = Path(executable).parent / "node_modules/@openai/codex/bin/codex.js"
    if entry.is_file() and shutil.which("node"):
        return [shutil.which("node"), str(entry)]
    return [executable]


PROMPT = """Run an acceptance test of the sunone_qa MCP server. Use only its MCP
tools, no shell/files/network/other tools. The editor is an isolated synthetic
preview with no capture or physical input. Do not change global settings or
existing workflows. Discover node kinds and workflow IDs from tools.

1. Read application state (verify preview=true), workspace summary and catalog
   definitions for Float, Add and Preview. Create a new workflow named 'Codex MCP
   acceptance' with nodes 1=Float(value=5,x=32,y=64),
   2=Float(value=7,x=32,y=256), 3=Add(x=352,y=64),
   4=Preview(x=672,y=64). Wire both Floats to Add's A/B and its Result to
   Preview's Data. Validate the whole batch first and then apply it atomically
   using response=summary. Use explicit IDs and the current version.
2. Focus the new Preview using editor_action. Use inspect_node on Add (retry
   up to 8 times if current=false). Verify fresh value=12, both named incoming
   connections and the outgoing Preview Data connection.
3. Change Float 1 to 9 in one edit. Verify fresh Add value=16 using inspect_node
   (retry up to 8 times if stale). Deliberately submit another node.update with
   the version from BEFORE that change: it must fail with version_conflict.
   Read/inspect and verify Float 1 is still 9 and Add is still 16.
4. Undo that successful edit using the latest workspace version. Verify Add
   is back to 12 through fresh inspect_node. Leave the new workflow present for
   the external harness to verify. Briefly report observed values, stale-version
   rejection and Undo. Never report success without actual tool observations.
"""


def verify_events(events):
    completed = [event["item"] for event in events
                 if event.get("type") == "item.completed" and event.get("item", {}).get("type") == "mcp_tool_call"]
    assert completed, "Codex did not complete any actual MCP tool calls"
    assert all(item.get("server") == "sunone_qa" for item in completed), "Unexpected MCP server"
    calls = {item["tool"] for item in completed}
    required = {"get_application", "get_workspace", "get_catalog", "validate_workflows", "edit_workflows", "editor_action", "inspect_node"}
    assert required <= calls, f"Missing actual Codex MCP calls: {required - calls}"
    observations, conflict = [], False
    for item in completed:
        result = item.get("result") or {}
        data = result.get("structured_content") or result.get("structuredContent")
        if data is None:
            for content in result.get("content", []):
                if content.get("type") == "text":
                    try:
                        data = json.loads(content["text"])
                    except ValueError:
                        continue
                    break
        if not isinstance(data, dict):
            continue
        if data.get("code") == "version_conflict":
            conflict = True
        if item["tool"] == "inspect_node" and data.get("definition", {}).get("title") == "Add" and data.get("runtime", {}).get("current"):
            observations.append(data["runtime"].get("value", {}).get("value"))
    assert conflict, "No observed version_conflict response from an actual Codex MCP call"
    assert 12 in observations and 16 in observations and observations[-1] == 12, f"Unexpected fresh Add observations: {observations}"
    return dict(mcp_calls=len(completed), tools=sorted(calls), fresh_add_values=observations, version_conflict=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--model", help="Optional model override; otherwise use the CLI default")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    stage = repo / "build" / ("qa-codex-mcp-" + uuid.uuid4().hex)
    stage.mkdir()
    print("QA files:", stage, flush=True)
    source = args.build_dir.resolve()
    for path in (source / "ai.exe", source / "workflow_mcp.py", *source.glob("*.dll")):
        shutil.copy2(path, stage / path.name)
    instance = "codex_" + uuid.uuid4().hex
    environment = dict(os.environ, SUNONE_MCP_INSTANCE=instance)
    # JSON basic strings/arrays also form valid TOML values.
    enabled = ["get_application", "get_workspace", "get_catalog", "validate_workflows", "edit_workflows", "editor_action", "inspect_node"]
    config = ("mcp_servers.sunone_qa={command=" + json.dumps(sys.executable)
              + ",args=" + json.dumps(["-u", str(stage / "workflow_mcp.py"), "--instance", instance])
              + ",required=true,enabled_tools=" + json.dumps(enabled)
              + ',tools={edit_workflows={approval_mode="approve"},editor_action={approval_mode="approve"}}}')
    command = codex_command() + ["exec", "--ignore-user-config", "--ephemeral", "--json", "--color", "never",
        "--skip-git-repo-check", "-C", str(stage), "-c", 'approval_policy="never"',
        "-c", 'model_reasoning_effort="medium"', "-c", config, "-"]
    if args.model:
        command[-1:-1] = ["--model", args.model]
    with (stage / "preview.log").open("wb") as log:
        process = subprocess.Popen([str(stage / "ai.exe"), "--node-editor-preview"], cwd=stage, env=environment,
                                   stdout=log, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            wait_for_preview(process, stage, instance)
            with (stage / "codex-events.jsonl").open("w", encoding="utf-8") as stdout, (stage / "codex-stderr.log").open("w", encoding="utf-8") as stderr:
                # communicate drains stdin while the parent can report progress
                # by polling this test process and its on-disk event stream.
                client = subprocess.Popen(command, cwd=stage, stdin=subprocess.PIPE, stdout=stdout, stderr=stderr, text=True, encoding="utf-8")
                try:
                    client.communicate(PROMPT, timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    client.kill()
                    client.wait()
                    raise AssertionError("Codex acceptance test timed out; inspect codex-events.jsonl")
                assert client.returncode == 0, "Codex failed; inspect codex-stderr.log"
            events = [json.loads(line) for line in (stage / "codex-events.jsonl").read_text(encoding="utf-8").splitlines() if line.strip()]
            summary = verify_events(events)
            spec = importlib.util.spec_from_file_location("codex_mcp_verifier", stage / "workflow_mcp.py")
            adapter = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(adapter)
            server = adapter.Server(adapter.PipeBridge(instance))
            workspace = server.call("get_workspace", {})["workspace"]
            graph = next(item for item in workspace["graphs"] if item["name"] == "Codex MCP acceptance")
            assert len(graph["nodes"]) == 4 and len(graph["links"]) == 3
            assert next(item for item in graph["nodes"] if item["id"] == 1)["value"] == 5
            assert adapter.parse_workspace((stage / "node_graphs.ini").read_text(encoding="utf-8")) == workspace
            print(json.dumps(dict(codex="connected", autosave_verified=True, **summary)), flush=True)
        finally:
            stop_preview(process)


if __name__ == "__main__":
    main()
