"""Protocol/serialization/batch regressions; no devices, providers or network."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location("workflow_mcp", Path(__file__).resolve().parents[1] / "tools/workflow_mcp.py")
mcp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mcp)


def graph(identifier=1, name="Logic"):
    return dict(id=identifier, name=name, kind=0, icon=-1, enabled=True, active=True, page_name="", pan_x=40, pan_y=40, zoom=1, nodes=[], links=[], groups=[], ties=[])


CATALOG = dict(nodes=[dict(kind=12, title="Boolean", inputs=[], outputs=[dict(type="Bool")], defaults=dict(value=1, parameters={}, text_parameters={}, input_values={}, input_texts={})), dict(kind=13, title="Float", inputs=[], outputs=[dict(type="Float")], defaults=dict(value=0, parameters={}, text_parameters={}, input_values={}, input_texts={})), dict(kind=47, title="Reroute float", reroute=True, inputs=[dict(type="Float")], outputs=[dict(type="Float")], defaults={}), dict(kind=46, title="Reroute boolean", reroute=True, inputs=[dict(type="Bool")], outputs=[dict(type="Bool")], defaults={})])


class Bridge:
    def __init__(self):
        self.workspace = dict(format_version=20, graphs=[graph()])
        self.commits = 0
        self.race = False

    def __call__(self, **request):
        document = mcp.serialize_workspace(self.workspace)
        version = hashlib.sha256(document.encode()).hexdigest()
        if request["op"] == "read":
            return dict(ok=True, document=document, version=version)
        if request["op"] == "catalog":
            return copy.deepcopy(CATALOG)
        if request["op"] == "commit":
            if self.race:
                self.workspace["graphs"][0]["pan_x"] += 32
                self.race = False
                raise mcp.ToolError("Version conflict")
            if request["expected_version"] != version:
                raise mcp.ToolError("Version conflict")
            candidate = mcp.parse_workspace(request["document"])
            if not request.get("dry_run"):
                self.workspace = candidate
                self.commits += 1
                document = mcp.serialize_workspace(self.workspace)
                version = hashlib.sha256(document.encode()).hexdigest()
            else:
                document = request["document"]
            return dict(ok=True, applied=not request.get("dry_run"), version=version, document=document)
        raise mcp.ToolError("Unsupported fake bridge operation")


class WorkflowMcpTests(unittest.TestCase):
    def test_round_trip_preserves_unicode_escapes_precision_and_routes(self):
        item = graph(name='Тест "кадра" \\ маршруты')
        node = mcp.default_node(CATALOG["nodes"][0], 1, dict(x=32, y=64, text_parameters={"name": 'C:\\Тест\\a"b\nsecond'}, parameters={"test": 0.005555555555555556}))
        item["nodes"] = [node]
        item["groups"] = [dict(id=1, title="Группа", x=0, y=0, width=512, height=512, color=0xFFFFFFFF, nodes=[1])]
        item["ties"] = [dict(id=1, x=64, y=96, angle=-15)]
        item["links"] = [dict(from_node=1, from_port=0, to_node=1, to_port=0, ties=[1], bend=dict(roundness=0.3, twist=-0.2))]
        workspace = dict(format_version=20, graphs=[item])
        encoded = mcp.serialize_workspace(workspace)
        self.assertEqual(mcp.serialize_workspace(mcp.parse_workspace(encoded)), encoded)
        self.assertEqual(mcp.parse_workspace(encoded)["graphs"][0]["nodes"][0]["text_parameters"], node["text_parameters"])

    def test_batch_supports_nodes_reroutes_groups_ties_and_connection_replacement(self):
        bridge = Bridge()
        commands = [dict(op="node.add", workflow=1, id=1, kind=12), dict(op="node.add", workflow=1, id=2, kind=47), dict(op="link.connect", workflow=1, link=dict(from_node=1, from_port=0, to_node=2, to_port=0)), dict(op="group.add", workflow=1, id=1, fields=dict(title="Block", width=768)), dict(op="tie.add", workflow=1, id=1, fields=dict(x=64, y=64, angle=-7.5)), dict(op="link.update", workflow=1, link=dict(from_node=1, from_port=0, to_node=2, to_port=0), fields=dict(ties=[1], bend=dict(twist=0.25)))]
        result, arrange, created = mcp.edit_workspace(bridge.workspace, commands, CATALOG, None)
        item = result["graphs"][0]
        self.assertEqual(item["nodes"][1]["kind"], 46)
        self.assertEqual(item["ties"][0]["angle"], -15)
        self.assertEqual(item["links"][0]["bend"], dict(roundness=0.58, twist=0.25))
        self.assertEqual(len(created), 4)
        self.assertEqual(bridge.workspace["graphs"][0]["nodes"], [])

    def test_invalid_batch_and_stale_versions_do_not_commit(self):
        bridge = Bridge()
        server = mcp.Server(bridge)
        version = server.call("get_workspace", {})["version"]
        with self.assertRaises(mcp.ToolError):
            server.call("edit_workflows", dict(expected_version=version, operations=[dict(op="workflow.create", id=5), dict(op="node.update", workflow=5, id=999, fields=dict(value=2))]))
        self.assertEqual(bridge.commits, 0)
        with self.assertRaises(mcp.ToolError):
            server.call("edit_workflows", dict(expected_version="stale", operations=[dict(op="workflow.create")]))
        bridge.race = True
        with self.assertRaises(mcp.ToolError):
            server.call("edit_workflows", dict(expected_version=version, operations=[dict(op="workflow.create")]))
        self.assertEqual(bridge.commits, 0)

    def test_preview_does_not_publish_and_live_batch_is_one_commit(self):
        bridge = Bridge()
        server = mcp.Server(bridge)
        arguments = dict(expected_version=server.call("get_workspace", {})["version"], operations=[dict(op="workflow.create", id=5, fields=dict(name="Other")), dict(op="node.add", workflow=5, id=1, kind=12)])
        self.assertFalse(server.call("validate_workflows", arguments)["applied"])
        self.assertEqual(bridge.commits, 0)
        self.assertTrue(server.call("edit_workflows", arguments)["applied"])
        self.assertEqual(bridge.commits, 1)

    def test_protocol_handshake_notifications_and_argument_errors(self):
        server = mcp.Server(Bridge())
        self.assertIn("error", server.handle(dict(jsonrpc="2.0", id=1, method="tools/list")))
        initialized = server.handle(dict(jsonrpc="2.0", id=2, method="initialize", params=dict(protocolVersion="2025-11-25", capabilities={}, clientInfo=dict(name="tests", version="1"))))
        self.assertEqual(initialized["result"]["protocolVersion"], "2025-11-25")
        self.assertIsNone(server.handle(dict(jsonrpc="2.0", method="notifications/initialized")))
        self.assertEqual(len(server.handle(dict(jsonrpc="2.0", id=3, method="tools/list"))["result"]["tools"]), len(mcp.TOOLS))
        for arguments in (dict(expected_version="v", operations="invalid"), dict(expected_version="v", operations=[dict(op="node.add", workflow=True, kind=12)])):
            result = server.handle(dict(jsonrpc="2.0", id=4, method="tools/call", params=dict(name="edit_workflows", arguments=arguments)))
            self.assertTrue(result["result"]["isError"])
        self.assertIsNone(server.handle(dict(jsonrpc="2.0", method="notifications/cancelled", params=dict(requestId=99))))
        self.assertIn("error", server.handle([]))

    def test_defaults_removal_delete_and_workflow_append(self):
        bridge = Bridge()
        server = mcp.Server(bridge)
        current = server.call("get_workspace", {})
        other = dict(format_version=20, graphs=[graph(1, "Imported")])
        result = server.call("import_workflows", dict(expected_version=current["version"], workspace=other, mode="append"))
        self.assertEqual([item["id"] for item in result["workspace"]["graphs"]], [1, 5])
        with self.assertRaises(mcp.ToolError):
            mcp.serialize_workspace(dict(format_version=19, graphs=[]))
        with self.assertRaises(mcp.ToolError):
            mcp.number(float("inf"))
        with self.assertRaises(mcp.ToolError):
            mcp.identifier(1.5)

    def test_agent_discovery_resources_and_compact_responses(self):
        server = mcp.Server(Bridge())
        self.assertEqual(len(server.call("get_catalog", dict(query="BOOLEAN"))["nodes"]), 2)
        self.assertEqual(len(server.call("get_catalog", {})["nodes"]), 4)
        current = server.call("get_workspace", dict(summary=True))
        self.assertNotIn("workspace", current)
        self.assertEqual(current["workflows"][0]["nodes"], 0)
        edited = server.call("edit_workflows", dict(expected_version=current["version"], response="summary", operations=[dict(op="workflow.create", id=5)]))
        self.assertNotIn("workspace", edited)
        self.assertEqual(len(edited["workflows"]), 2)
        server.initialized = server.ready = True
        listed = server.handle(dict(jsonrpc="2.0", id=1, method="resources/list"))["result"]["resources"]
        result = server.handle(dict(jsonrpc="2.0", id=2, method="resources/read", params=dict(uri=listed[0]["uri"])))
        self.assertIn("settings_version", result["result"]["contents"][0]["text"])
        self.assertIn("error", server.handle(dict(jsonrpc="2.0", id=3, method="resources/read", params=dict(uri="file:///private"))))

    def test_application_tool_arguments_and_forwarding(self):
        calls = []
        def bridge(**request):
            calls.append(request)
            return dict(ok=True)
        server = mcp.Server(bridge)
        server.call("get_application", {})
        self.assertEqual(calls[-1]["op"], "application")
        server.call("edit_application", dict(expected_settings_version="v", settings=dict(overlay_ui_scale=1.1), dry_run=True))
        self.assertEqual(calls[-1]["op"], "application_edit")
        for name, arguments in (("application_action", dict(action="select_tab")), ("model_export", dict(action="start")), ("model_export", dict(action="start", path="a.onnx", options=dict(resolution=1))), ("application_action", dict(action="shell"))):
            with self.assertRaises(mcp.ToolError):
                server.call(name, arguments)
        server.call("model_export", dict(action="status"))
        self.assertEqual(calls[-1]["op"], "model_export")

    def test_reroute_deletion_heals_chains_and_fanout(self):
        base = graph()
        base["nodes"] = [mcp.default_node(CATALOG["nodes"][0 if identifier == 1 else 3], identifier) for identifier in range(1, 6)]
        base["links"] = [dict(from_node=source, from_port=0, to_node=target, to_port=0, ties=ties, bend=dict(twist=0.25)) for source, target, ties in ((1, 2, [1, 2]), (2, 3, [2, 3]), (3, 4, [4]), (2, 5, [3]))]
        base["groups"] = [dict(id=1, nodes=[2, 3])]
        for order in ((2, 3), (3, 2)):
            workspace = dict(format_version=20, graphs=[copy.deepcopy(base)])
            result, _, _ = mcp.edit_workspace(workspace, [dict(op="node.delete", workflow=1, id=identifier) for identifier in order], CATALOG, None)
            item = result["graphs"][0]
            self.assertEqual([(link["from_node"], link["to_node"]) for link in item["links"]], [(1, 4), (1, 5)])
            self.assertEqual(item["links"][0]["ties"], [1, 2, 3, 4])
            self.assertEqual(item["links"][0]["bend"]["twist"], 0.25)
            self.assertEqual(item["groups"][0]["nodes"], [])
        definitions = {item["kind"]: item for item in CATALOG["nodes"]}
        item = copy.deepcopy(base)
        mcp.remove_nodes(item, [1, 2, 3], definitions)
        self.assertEqual(item["links"], [])
        item = copy.deepcopy(base)
        item["links"].pop(0)
        mcp.remove_nodes(item, [2, 3], definitions)
        self.assertEqual(item["links"], [])

    def test_any_preview_does_not_lock_a_loose_reroute(self):
        catalog = copy.deepcopy(CATALOG)
        catalog["nodes"].append(dict(kind=1000, title="Preview", inputs=[dict(type="Bool"), dict(type="Any")], outputs=[], defaults={}))
        workspace = dict(format_version=20, graphs=[graph()])
        commands = [dict(op="node.add", workflow=1, id=1, kind=12), dict(op="node.add", workflow=1, id=2, kind=47), dict(op="node.add", workflow=1, id=3, kind=1000), dict(op="link.connect", workflow=1, link=dict(from_node=2, from_port=0, to_node=3, to_port=1)), dict(op="link.connect", workflow=1, link=dict(from_node=1, from_port=0, to_node=2, to_port=0))]
        result, _, _ = mcp.edit_workspace(workspace, commands, catalog, None)
        self.assertEqual(result["graphs"][0]["nodes"][1]["kind"], 46)


if __name__ == "__main__":
    unittest.main()
