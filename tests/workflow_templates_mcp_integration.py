"""Exercise all four standard workflows over real MCP in a synthetic editor.

No devices/providers are started. All temporary edits use the MCP protocol;
--workspace additionally checks a copy of an existing saved workspace.
"""
import argparse
import asyncio
from contextlib import AsyncExitStack
import json
import math
import os
from pathlib import Path
import shutil
import site
import subprocess
import sys
import time
import uuid

from workflow_mcp_integration import wait_for_preview, stop_preview


async def exercise(stage, instance):
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
    parameters = StdioServerParameters(command=sys.executable, args=[str(stage / "workflow_mcp.py"), "--instance", instance])
    calls, cases = 0, []
    async with AsyncExitStack() as stack:
        read, write = await stack.enter_async_context(stdio_client(parameters))
        session = await stack.enter_async_context(ClientSession(read, write, read_timeout_seconds=20))
        await session.initialize()
        journal = stack.enter_context((stage / "mcp-calls.jsonl").open("w", encoding="utf-8"))

        async def call(name, arguments=None):
            nonlocal calls
            calls += 1
            raw = (await session.call_tool(name, arguments or {})).model_dump(by_alias=True)
            data = raw.get("structuredContent") or json.loads(raw["content"][0]["text"])
            journal.write(json.dumps(dict(tool=name, arguments=arguments or {}, result=data), ensure_ascii=False) + "\n")
            journal.flush()
            assert not raw.get("isError"), f"{name}: {data}"
            return data

        assert (await call("get_application"))["state"]["preview"]
        initial = await call("get_workspace")
        baseline = initial["workspace"]
        catalog = await call("get_catalog")
        definitions = {node["kind"]: node for node in catalog["nodes"]}
        kinds = {node["title"]: node["kind"] for node in catalog["nodes"]}
        graphs = {graph["kind"]: graph for graph in baseline["graphs"]}
        assert set(range(4)) <= graphs.keys(), "The four standard workflow kinds are required"

        def find(domain, title):
            return next(node for node in graphs[domain]["nodes"] if definitions[node["kind"]]["title"] == title)

        def update(domain, title, **parameters):
            return dict(op="node.update", workflow=graphs[domain]["id"], id=find(domain, title)["id"], fields=dict(parameters=parameters))

        def connect(domain, source, target, pin):
            return dict(op="link.connect", workflow=graphs[domain]["id"], link=dict(from_node=source, from_port=0, to_node=target, to_port=pin))

        def override(domain, title, value):
            identifier = 900000 + find(domain, title)["id"]
            graph = graphs[domain]
            ops = [dict(op="node.add", workflow=graph["id"], id=identifier, kind=kinds["Boolean"], fields=dict(value=value))]
            ops += [connect(domain, identifier, link["to_node"], link["to_port"]) for link in graph["links"] if link["from_node"] == find(domain, title)["id"]]
            return ops

        def point_override(domain, title, pin):
            return [dict(op="node.add", workflow=graphs[domain]["id"], id=900099, kind=kinds["Make point"], fields=dict(input_values={"0": -10000, "1": -10000})),
                    connect(domain, 900099, find(domain, title)["id"], pin)]

        base = [update(1, "Auto shoot setting", enabled=1), update(2, "Recoil setting", enabled=1), update(2, "Recoil strength", strength=5),
                update(0, "Auto aim setting", enabled=0), update(1, "Auto aim setting", enabled=0),
                update(0, "Mouse calibration", sens=1, yaw=.022, pitch=.022, fov_x=106, fov_y=74, fov_scaled=0, base_fov=0),
                update(0, "Filter classes", class_b_enabled=0), update(1, "Filter classes", class_b_enabled=0), update(1, "Point in box?", multiplier=5),
                update(3, "Capture source", enabled=1), update(3, "Object inference", enabled=1), update(3, "Filter confidence", threshold=.1),
                update(3, "Limit detections", limit=100), update(3, "Non-maximum suppression", threshold=.5, class_agnostic=1),
                update(3, "Filter circle", enabled=1, radius_percent=0, radius=160), update(3, "Vision output", enabled=1),
                update(3, "Apply frame mask", enabled=0), update(3, "Filter detection mask", enabled=0), update(3, "Render overlay", enabled=0),
                update(3, "Overlay depth / mask", depth_mask_enabled=0, depth_debug_overlay_enabled=0)]
        sinks = [(0, "Mouse output"), (1, "Auto shoot"), (2, "Recoil movement"), (3, "Vision output")]

        async def run(name, operations=(), aim=True, shoot=True, recoil=True, detections=2, extra=None):
            current = await call("get_workspace", dict(summary=True))
            restored = await call("import_workflows", dict(expected_version=current["version"], workspace=baseline))
            proposed = dict(expected_version=restored["version"], operations=base + list(operations))
            assert not (await call("validate_workflows", proposed))["applied"]
            applied = await call("edit_workflows", dict(**proposed, response="summary"))
            assert applied["applied"] and applied["saved"]
            deadline = time.monotonic() + 5
            while True:
                state = await call("get_diagnostics")
                out = state.get("outputs", {})
                if out.get("control_current") and out.get("vision_current"):
                    assert (out["aim"], out["recoil"], out["vision_detections"]) == (aim, recoil, detections), f"{name}: {out}"
                    if shoot is not None:
                        assert out["auto_shoot"] == shoot, f"{name}: {out}"
                    if aim:
                        assert out["movement"]["steps"] > 0 and (out["movement"]["dx"] or out["movement"]["dy"]), f"{name}: active Aim produced no movement"
                    else:
                        assert out["movement"]["steps"] == 0, f"{name}: stale movement survived deactivation"
                    if recoil:
                        assert out["recoil_strength"] == 5 and math.isclose(out["recoil_pixel_delta_y"], 5 * .022 * 320 / 74, rel_tol=1e-8), f"{name}: shared calibration mismatch"
                    else:
                        assert out["recoil_strength"] == 0 and out["recoil_pixel_delta_y"] == 0, f"{name}: stale recoil survived deactivation"
                    break
                assert time.monotonic() < deadline, f"{name}: diagnostics never became current"
                await asyncio.sleep(.04)
            if extra:
                await extra(state)
            cases.append(dict(case=name, outputs=out))
            (stage / "results.json").write_text(json.dumps(dict(cases=cases, mcp_calls=calls), indent=2), encoding="utf-8")
            print(name, "PASS", json.dumps(out), flush=True)

        async def inactive_depth(state):
            assert state["outputs"]["active_depth"] == 0 and state["outputs"]["active_masks"] == 0

        async def active_depth(state):
            assert state["outputs"]["active_depth"] > 0 and state["outputs"]["active_masks"] > 0

        async def missing_calibration(state):
            for domain, title in (sinks[0], sinks[2]):
                inspected = await call("inspect_node", dict(workflow=graphs[domain]["id"], node=find(domain, title)["id"]))
                assert "Missing shared calibration" in inspected["runtime"]["value"]["error"]

        async def enable_errors(state):
            for domain, title in sinks:
                inspected = await call("inspect_node", dict(workflow=graphs[domain]["id"], node=find(domain, title)["id"]))
                value = inspected["runtime"]["value"]
                assert not value["valid"] and value["error"] == "Float division by zero.", f"{title} lost the Enable error: {value}"

        async def head_scope(state):
            graph = next(graph for graph in state["graphs"] if graph["workflow"] == graphs[1]["id"])
            values = {node["node"]: node["value"] for node in graph["nodes"]}
            assert values[find(1, "Track target")["id"]]["class_id"] == 1
            position, size = (values[find(1, title)["id"]] for title in ("Target position", "Target size"))
            inside = position["x"] < 160 < position["x"] + size["x"] and position["y"] < 160 < position["y"] + size["y"]
            assert state["outputs"]["auto_shoot"] == inside, "Auto Shoot disagrees with the selected head's actual box"

        await run("all_outputs_and_shared_calibration", extra=inactive_depth)
        await run("head_preference_and_original_scope", [update(0, "Filter classes", class_b_enabled=1), update(1, "Filter classes", class_b_enabled=1),
            update(0, "Prefer class", enabled=1, class_id=1), update(1, "Prefer class", enabled=1, class_id=1), update(1, "Point in box?", multiplier=1)], shoot=None, extra=head_scope)
        await run("reroute_deletion_keeps_all_workflows_connected", [dict(op="node.delete", workflow=graphs[domain]["id"], id=find(domain, title)["id"])
            for domain, title in ((0, "Reroute motion"), (1, "Reroute target"), (2, "Reroute point"), (3, "Reroute frame"))])
        released = override(0, "Aim held", 0) + override(1, "Aim held", 0)
        await run("aim_released_recoil_independent", released, aim=False, shoot=False)
        await run("auto_aim_without_held_button", released + [update(0, "Auto aim setting", enabled=1), update(1, "Auto aim setting", enabled=1)])
        await run("shoot_released", override(2, "Shoot held", 0), recoil=False)
        await run("zoom_released", override(2, "Zoom held", 0), recoil=False)
        await run("recoil_setting_off", [update(2, "Recoil setting", enabled=0)], recoil=False)
        await run("auto_shoot_setting_off", [update(1, "Auto shoot setting", enabled=0)], shoot=False)
        await run("crosshair_outside_target", point_override(1, "Point in box?", 2), shoot=False)
        await run("no_detections_and_no_stale_actions", [update(3, "Filter confidence", threshold=1)], aim=False, shoot=False, detections=0)
        await run("limit_detections", [update(3, "Limit detections", limit=1)], detections=1)
        await run("nms_suppresses_overlap", [update(3, "Non-maximum suppression", threshold=.01)], detections=1)
        await run("fov_filters_outside", point_override(3, "Filter circle", 1), aim=False, shoot=False, detections=0)
        await run("vision_output_off", [update(3, "Vision output", enabled=0)], aim=False, shoot=False, detections=0)
        await run("capture_off_percentage_fov", [update(3, "Filter circle", radius_percent=100), update(3, "Capture source", enabled=0)], aim=False, shoot=False, detections=0)
        await run("inference_off_percentage_fov", [update(3, "Filter circle", radius_percent=100), update(3, "Object inference", enabled=0)], aim=False, shoot=False, detections=0)
        masks = [update(3, "Depth inference", enabled=1), update(3, "Depth to mask", enabled=1), update(3, "Apply frame mask", enabled=1), update(3, "Filter detection mask", enabled=1)]
        await run("depth_and_mask_chain", masks, extra=active_depth)
        await run("missing_depth_stops_mask_chain", masks + [update(3, "Depth inference", enabled=0)], aim=False, shoot=False, detections=0)
        await run("disabled_masks_skip_missing_depth", [update(3, "Depth inference", enabled=0)], extra=inactive_depth)
        await run("overlay_style_chain", [update(3, "Render overlay", enabled=1)], extra=lambda state: check_render(state))
        await run("missing_calibration_stops_only_its_consumers", [dict(op="node.update", workflow=graphs[0]["id"], id=find(0, "Mouse calibration")["id"], fields=dict(text_parameters=dict(name="missing")))], aim=False, recoil=False, extra=missing_calibration)
        movement_link = next(link for link in graphs[0]["links"] if link["to_node"] == find(0, "Mouse output")["id"] and link["to_port"] == 1)
        await run("disconnected_movement_stops_aim", [dict(op="link.disconnect", workflow=graphs[0]["id"], link={key: movement_link[key] for key in ("from_node", "from_port", "to_node", "to_port")})], aim=False)
        errors = []
        for domain, title in sinks:
            errors += [dict(op="node.add", workflow=graphs[domain]["id"], id=900090, kind=kinds["Divide"], fields=dict(input_values={"0": 1, "1": 0})),
                       dict(op="node.add", workflow=graphs[domain]["id"], id=900091, kind=kinds["Greater than"]),
                       connect(domain, 900090, 900091, 0), connect(domain, 900091, find(domain, title)["id"], 0)]
        await run("enable_errors_reach_all_standard_outputs", errors, aim=False, shoot=False, recoil=False, detections=0, extra=enable_errors)
        await run("recovery_after_all_faults", extra=inactive_depth)
        current = await call("get_workspace", dict(summary=True))
        await call("import_workflows", dict(expected_version=current["version"], workspace=baseline))
        assert (await call("get_workspace"))["workspace"] == baseline
        print(json.dumps(dict(standard_workflows=4, cases=len(cases), mcp_calls=calls, restored=True)), flush=True)


async def check_render(state):
    assert state["outputs"]["active_renders"] == 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--sdk-dir", type=Path)
    parser.add_argument("--workspace", type=Path)
    args = parser.parse_args()
    if args.sdk_dir:
        site.addsitedir(str(args.sdk_dir.resolve()))
    stage = Path(__file__).resolve().parents[1] / "build" / ("qa-standard-mcp-" + uuid.uuid4().hex)
    stage.mkdir()
    print("QA files:", stage, flush=True)
    source = args.build_dir.resolve()
    for path in (source / "ai.exe", source / "workflow_mcp.py", *source.glob("*.dll")):
        shutil.copy2(path, stage / path.name)
    if args.workspace:
        shutil.copy2(args.workspace, stage / "node_graphs.ini")
    instance = "standard_" + uuid.uuid4().hex
    with (stage / "preview.log").open("wb") as log:
        process = subprocess.Popen([str(stage / "ai.exe"), "--node-editor-preview"], cwd=stage, env=dict(os.environ, SUNONE_MCP_INSTANCE=instance), stdout=log, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            wait_for_preview(process, stage, instance)
            asyncio.run(exercise(stage, instance))
        finally:
            stop_preview(process)


if __name__ == "__main__":
    main()
