"""Live MCP check against an isolated synthetic preview and the official SDK.

Run after a wrapper build. --sdk-dir may point to an ignored pip --target dir.
No capture, inference providers or physical input workers are started.
"""
import argparse
import asyncio
import ctypes
from contextlib import AsyncExitStack
import importlib.util
import json
import os
from pathlib import Path
import shutil
import site
import subprocess
import sys
import time
import uuid


def wait_for_preview(process, stage, instance):
    spec = importlib.util.spec_from_file_location("preview_mcp_connector", stage / "workflow_mcp.py")
    connector = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(connector)
    endpoint = connector.PipeBridge(instance).endpoint
    wait_pipe = ctypes.WinDLL("kernel32", use_last_error=True).WaitNamedPipeW
    wait_pipe.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32]
    wait_pipe.restype = ctypes.c_int
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        assert process.poll() is None, f"Synthetic preview exited during startup: {process.returncode}"
        if wait_pipe(endpoint, 100):
            return
        time.sleep(0.1)
    raise AssertionError("Synthetic preview did not expose its MCP endpoint within 45 seconds")


def stop_preview(process):
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=15)


async def exercise(stage, instance):
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
    parameters = StdioServerParameters(command=sys.executable, args=[str(stage / "workflow_mcp.py"), "--instance", instance])
    async with AsyncExitStack() as stack:
        sessions = []
        for _ in range(2):
            read, write = await stack.enter_async_context(stdio_client(parameters))
            session = await stack.enter_async_context(ClientSession(read, write, read_timeout_seconds=20))
            await session.initialize()
            sessions.append(session)

        async def call(name, arguments=None, client=0, error=False):
            response = await sessions[client].call_tool(name, arguments or {})
            raw = response.model_dump(by_alias=True)
            if bool(raw.get("isError")) != error:
                raise AssertionError(f"{name}: {raw}")
            return raw.get("structuredContent") or json.loads(raw["content"][0]["text"])

        tools = await sessions[0].list_tools()
        assert len(tools.tools) == 14
        resources = await sessions[0].list_resources()
        assert str(resources.resources[0].uri) == "sunone://agent-guide"
        guide = await sessions[0].read_resource(resources.resources[0].uri)
        assert "settings_version" in guide.contents[0].text
        application = await call("get_application")
        assert application["state"]["preview"] and len(application["fields"]) >= 40
        changes = dict(expected_settings_version=application["settings_version"], settings=dict(overlay_ui_scale=1.1))
        assert not (await call("edit_application", dict(**changes, dry_run=True)))["applied"]
        applied = await call("edit_application", changes)
        assert applied["applied"] and applied["saved"]
        await call("edit_application", changes, client=1, error=True)
        await call("edit_application", dict(expected_settings_version=applied["settings_version"], settings=dict(overlay_ui_scale=1.0, capture_fps=13)), error=True)
        assert (await call("get_application"))["settings"]["overlay_ui_scale"] == applied["settings"]["overlay_ui_scale"]
        await call("edit_application", dict(expected_settings_version=applied["settings_version"], settings=dict(overlay_ui_scale=application["settings"]["overlay_ui_scale"])))
        for action in ("pause", "pause", "resume"):
            await call("application_action", dict(action=action))
            await asyncio.sleep(0.04)
            assert (await call("get_application"))["state"]["paused"] == (action == "pause")
        assert (await call("model_export", dict(action="status")))["state"] == "idle"
        await call("model_export", dict(action="start", path="missing-model.onnx"), error=True)
        assert isinstance((await call("get_sources"))["entries"], list)
        assert isinstance((await call("get_sources", dict(source="monitors")))["entries"], list)
        catalog = await call("get_catalog")
        assert len((await call("get_catalog", dict(kinds=[catalog["nodes"][0]["kind"]])))["nodes"]) == 1
        assert all("inference" in (node["title"] + " " + node["category"]).casefold() for node in (await call("get_catalog", dict(query="inference")))["nodes"])
        assert "workspace" not in await call("get_workspace", dict(summary=True))
        definitions = {item["title"].casefold(): item for item in catalog["nodes"]}
        boolean, negate = definitions["boolean"]["kind"], definitions["not"]["kind"]
        reroute, strength = definitions["reroute float"]["kind"], definitions["float"]["kind"]
        output = definitions["recoil movement"]["kind"]
        preview_kind = definitions["preview"]["kind"]
        assert definitions["preview"]["inputs"][1]["type"] == "Any"
        current = await call("get_workspace")
        for template in range(4):
            preview = await call("validate_workflows", dict(expected_version=current["version"], operations=[dict(op="workflow.create", id=900000+template, kind=template, template=True, fields=dict(name=f"Template {template}"))]))
            assert not preview["applied"]
        identifier = max(graph["id"] for graph in current["workspace"]["graphs"]) + 1
        universal = await call("validate_workflows", dict(expected_version=current["version"], operations=[dict(op="workflow.create", id=identifier), dict(op="node.add", workflow=identifier, id=1, kind=definitions["string"]["kind"], fields=dict(text_parameters=dict(value="Full preview text"))), dict(op="node.add", workflow=identifier, id=2, kind=preview_kind), dict(op="link.connect", workflow=identifier, link=dict(from_node=1, from_port=0, to_node=2, to_port=1))]))
        assert not universal["applied"]
        link = dict(from_node=1, from_port=0, to_node=3, to_port=0)
        operations = [dict(op="workflow.create", id=identifier, fields=dict(name="MCP integration"))]
        for node, kind, x, y in ((1, boolean, 64, 64), (2, negate, 448, 64), (3, reroute, 320, 96), (4, output, 640, 64), (5, strength, 64, 256)):
            operations.append(dict(op="node.add", workflow=identifier, id=node, kind=kind, fields=dict(x=x, y=y)))
        for source, target, pin in ((1, 3, 0), (3, 2, 0), (2, 4, 0), (5, 4, 1)):
            operations.append(dict(op="link.connect", workflow=identifier, link=dict(from_node=source, from_port=0, to_node=target, to_port=pin)))
        operations += [dict(op="group.add", workflow=identifier, id=1, fields=dict(title="Agent block", x=0, y=0, width=1024, height=512)), dict(op="tie.add", workflow=identifier, id=1, fields=dict(x=192, y=96, angle=17)), dict(op="link.update", workflow=identifier, link=link, fields=dict(ties=[1], bend=dict(twist=0.2)))]
        arguments = dict(expected_version=current["version"], operations=operations)
        assert not (await call("validate_workflows", arguments))["applied"]
        assert (await call("get_workspace"))["version"] == current["version"]
        edited = await call("edit_workflows", arguments)
        assert edited["applied"] and edited["saved"]
        graph = next(graph for graph in edited["workspace"]["graphs"] if graph["id"] == identifier)
        assert len(graph["nodes"]) == 5 and graph["ties"][0]["angle"] == 15
        assert graph["nodes"][2]["kind"] == definitions["reroute boolean"]["kind"]
        assert set(graph["groups"][0]["nodes"]) == {1, 2, 3, 4, 5}
        conflict = await call("edit_workflows", arguments, client=1, error=True)
        assert conflict["code"] == "version_conflict" and conflict["retryable"]
        invalid = dict(expected_version=edited["version"], operations=[dict(op="node.update", workflow=identifier, id=1, fields=dict(value=0)), dict(op="link.connect", workflow=identifier, link=dict(from_node=2, from_port=0, to_node=3, to_port=0))])
        await call("edit_workflows", invalid, error=True)
        assert (await call("get_workspace"))["version"] == edited["version"]
        undone = await call("editor_action", dict(expected_version=edited["version"], action="undo"))
        assert all(graph["id"] != identifier for graph in undone["workspace"]["graphs"])
        undone = await call("get_workspace")
        assert all(graph["id"] != identifier for graph in undone["workspace"]["graphs"])
        await call("editor_action", dict(expected_version=undone["version"], action="redo"))
        current = await call("get_workspace")
        # Two independent MCP processes race on the same version: exactly one wins.
        competing = [dict(expected_version=current["version"], operations=[dict(op="node.update", workflow=identifier, id=5, fields=dict(value=value))]) for value in (2, 3)]
        responses = await asyncio.gather(*(sessions[index].call_tool("edit_workflows", argument) for index, argument in enumerate(competing)))
        assert sum(not bool(response.model_dump(by_alias=True).get("isError")) for response in responses) == 1
        await call("get_diagnostics", dict(workflow=identifier))
        await asyncio.sleep(0.12)
        diagnostics = await call("get_diagnostics", dict(workflow=identifier))
        assert diagnostics["graphs"][0]["current"]
        inspection = await call("inspect_node", dict(workflow=identifier, node=2))
        assert inspection["runtime"]["current"] and inspection["inputs"][0]["connected"]
        assert inspection["inputs"][0]["source"]["node"] == 3
        assert inspection["outputs"][0]["destinations"][0]["node"] == 4
        missing = await call("inspect_node", dict(workflow=identifier, node=999999), error=True)
        assert missing["code"] == "not_found" and not missing["retryable"]
        await call("get_diagnostics", dict(workflow=999999), error=True)
        assert next(node for node in diagnostics["graphs"][0]["nodes"] if node["node"] == 2)["value"]["value"] is False
        preview_text = "Universal preview: полный текст\n0.005555555555555556"
        current = await call("get_workspace")
        preview_workflow = identifier + 1
        inspected = await call("edit_workflows", dict(expected_version=current["version"], operations=[dict(op="workflow.create", id=preview_workflow, fields=dict(name="MCP value preview")), dict(op="node.add", workflow=preview_workflow, id=1, kind=definitions["string"]["kind"], fields=dict(x=32, y=32, text_parameters=dict(value=preview_text))), dict(op="node.add", workflow=preview_workflow, id=2, kind=preview_kind, fields=dict(x=352, y=32)), dict(op="link.connect", workflow=preview_workflow, link=dict(from_node=1, from_port=0, to_node=2, to_port=1))]))
        await call("editor_action", dict(expected_version=inspected["version"], action="focus", workflow=preview_workflow, node=2))
        for _ in range(50):
            visible = await call("get_diagnostics", dict(workflow=preview_workflow, node=2))
            nodes = visible["graphs"][0]["nodes"]
            if nodes and nodes[0]["value"].get("preview", {}).get("text") == preview_text:
                break
            await asyncio.sleep(0.05)
        else:
            raise AssertionError("Visible universal preview did not publish full text through MCP")
        exported = await call("export_workflows")
        assert "SUNONE_NODE_GRAPH 20" in exported["document"]
        assert (stage / "node_graphs.ini").read_text(encoding="utf-8") == exported["document"]
        healed = await call("edit_workflows", dict(expected_version=exported["version"], operations=[dict(op="node.delete", workflow=identifier, id=3)]))
        healed_graph = next(graph for graph in healed["workspace"]["graphs"] if graph["id"] == identifier)
        connection = next(link for link in healed_graph["links"] if link["to_node"] == 2)
        assert connection["from_node"] == 1 and connection["ties"] == [1]
        assert len(healed_graph["nodes"]) == 4
        restored = await call("editor_action", dict(expected_version=healed["version"], action="undo"))
        assert restored["version"] == exported["version"]
        for action in ("hide", "show", "maximized", "fullscreen", "windowed"):
            await call("application_action", dict(action=action))
            await asyncio.sleep(0.05)
            state = (await call("get_application"))["state"]
            assert state["visible"] == (action != "hide")
            if action in ("maximized", "fullscreen", "windowed"):
                assert state["window_mode"] == action
        await call("application_action", dict(action="select_tab", tab="system"))
        assert (await call("get_application"))["state"]["tab"] == "system"
        print(json.dumps(dict(official_sdk="connected", tools=len(tools.tools), atomic_edits=True, stale_version_rejected=True, concurrent_clients=True, diagnostics=True, autosave=True, undo_redo=True, application_settings=True, window_actions=True, resources=True)))


async def check_disabled(stage, instance):
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
    parameters = StdioServerParameters(command=sys.executable, args=[str(stage / "workflow_mcp.py"), "--instance", instance])
    async with stdio_client(parameters) as (read, write), ClientSession(read, write, read_timeout_seconds=20) as session:
        await session.initialize()
        response = await session.call_tool("get_workspace", {})
        assert response.model_dump(by_alias=True).get("isError"), "A disabled MCP endpoint accepted a connection after restart"
    print("Disabled access remained disabled after restart")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--sdk-dir", type=Path)
    arguments = parser.parse_args()
    if arguments.sdk_dir:
        site.addsitedir(str(arguments.sdk_dir.resolve()))
    repo = Path(__file__).resolve().parents[1]
    run_id = uuid.uuid4().hex
    stage = repo / "build" / f"qa-mcp-{run_id}"
    stage.mkdir()
    source = arguments.build_dir.resolve()
    for path in (source / "ai.exe", source / "workflow_mcp.py", *source.glob("*.dll")):
        shutil.copy2(path, stage / path.name)
    instance = "qa_" + run_id
    environment = dict(os.environ, SUNONE_MCP_INSTANCE=instance)
    with (stage / "preview.log").open("wb") as log:
        process = subprocess.Popen([str(stage / "ai.exe"), "--node-editor-preview"], cwd=stage, env=environment, stdout=log, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            wait_for_preview(process, stage, instance)
            asyncio.run(exercise(stage, instance))
        finally:
            stop_preview(process)
        configuration = stage / "config.ini"
        configuration.write_text(configuration.read_text(encoding="utf-8").replace("local_mcp_enabled = true", "local_mcp_enabled = false"), encoding="utf-8")
        process = subprocess.Popen([str(stage / "ai.exe"), "--node-editor-preview"], cwd=stage, env=environment, stdout=log, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            time.sleep(1)
            assert process.poll() is None, "Disabled preview exited during startup"
            asyncio.run(check_disabled(stage, instance))
        finally:
            stop_preview(process)
    print("QA files:", stage)


if __name__ == "__main__":
    main()
