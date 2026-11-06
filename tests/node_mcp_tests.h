#pragma once
namespace
{
void testMcpEditorTransactions()
{
    gestureScenario="MCP transactions use validation, optimistic versions, Undo and autosave";
    TemporaryWorkspace directory;EditorHarness harness;McpJson::Apartment apartment;
    using namespace McpJson;
    const auto call=[](const JsonObject& command){return JsonObject::Parse(winrt::to_hstring(NodeEditor_McpCall(encode(command))));};
    JsonObject readCommand;put(readCommand,L"op","read");auto original=call(readCommand);
    require(original.GetNamedBoolean(L"ok"),"MCP cannot read a workspace");
    JsonObject catalogCommand;put(catalogCommand,L"op","catalog");const auto catalog=call(catalogCommand);
    require(catalog.GetNamedArray(L"nodes").Size()==definitions().size(),"MCP hides node kinds from agents");
    require(catalog.GetNamedArray(L"nodes").GetObjectAt(static_cast<unsigned>(NodeKind::Boolean)).GetNamedObject(L"defaults").GetNamedNumber(L"value")==1,"MCP defaults differ from actual Boolean source defaults");
    JsonArray kinds;kinds.Append(JsonValue::CreateNumberValue(static_cast<int>(NodeKind::Boolean)));put(catalogCommand,L"kinds",kinds);
    require(call(catalogCommand).GetNamedArray(L"nodes").Size()==1,"MCP ignored the exact catalog filter");
    std::filesystem::create_directory("models");{std::ofstream fixture("models/custom.onnx");fixture<<"Model-name discovery fixture, never loaded";}
    modelMenu.value="Existing picker selection";const auto scans=modelMenu.scans;
    JsonObject sourceCommand;put(sourceCommand,L"op","sources");const auto sources=call(sourceCommand);
    require(sources.GetNamedBoolean(L"ok")&&sources.GetNamedArray(L"entries").Size()==1&&modelMenu.value=="Existing picker selection"&&modelMenu.scans==scans,"MCP source discovery disturbed the UI model picker");
    const auto proposal=[&](Workspace workspace,const JsonObject& before,bool dryRun=false){
        std::string document,error;require(serialize(workspace,document,error),error.c_str());JsonObject command;
        put(command,L"op","commit");put(command,L"expected_version",text(before,L"version"));put(command,L"document",document);put(command,L"dry_run",dryRun);return command;
    };
    const auto before=service().workspace();auto candidate=before;
    candidate.graphs[0].nodes[0].x=37;candidate.graphs[0].nodes[0].width=193;
    const auto undoCount=editor.undo.size();const auto revision=service().revision();
    auto preview=call(proposal(candidate,original,true));
    require(preview.GetNamedBoolean(L"ok")&&!preview.GetNamedBoolean(L"applied")&&editor.undo.size()==undoCount&&service().revision()==revision,"MCP dry run changed live execution or history");
    Workspace normalized;std::string issue;require(deserialize(text(preview,L"document"),normalized,issue),"MCP preview did not return a valid document");
    require(findNode(normalized.graphs[0],1)->x==32&&std::fmod(findNode(normalized.graphs[0],1)->width,32)==0,"MCP bypassed grid normalization");
    const auto command=proposal(candidate,original);
    const auto busy=JsonObject::Parse(winrt::to_hstring(NodeEditor_McpCall(encode(command),true)));
    require(!busy.GetNamedBoolean(L"ok")&&text(busy,L"code")=="busy","MCP overwrote an active user interaction or lost its error code");
    auto applied=call(command);require(applied.GetNamedBoolean(L"ok")&&applied.GetNamedBoolean(L"applied")&&applied.GetNamedBoolean(L"saved"),"MCP did not apply and persist a valid transaction");
    require(editor.undo.size()==undoCount+1&&service().revision()==revision,"A presentation-only MCP edit affected execution or created multiple Undo steps");
    require(text(applied,L"version")!=text(original,L"version"),"MCP versioning ignores presentation-only edits");
    const auto conflict=call(command);require(!conflict.GetNamedBoolean(L"ok")&&text(conflict,L"code")=="version_conflict","A stale MCP version overwrote a newer workspace or lost its error code");
    require(std::filesystem::is_regular_file(GraphFile),"A remote transaction did not autosave");
    JsonObject broken;put(broken,L"op","commit");put(broken,L"expected_version",text(applied,L"version"));put(broken,L"document","SUNONE_NODE_GRAPH 20\nPAGES 1\nGRAPH 0 0 0 1 2 2 0 0\nPAGE 1 1 1 \"Invalid\" \"\" -1\nNODE 1 16 0 0 0 0 0 0 0 0 0 0 0 0\nNODE 2 16 64 0 0 0 0 0 0 0 0 0 0 0\nLINK 1 0 2 0 0 0.58 0\nLINK 2 0 1 0 0 0.58 0\nEND\n");
    require(!call(broken).GetNamedBoolean(L"ok")&&text(call(readCommand),L"version")==text(applied,L"version")&&editor.undo.size()==undoCount+1,"Invalid MCP input partially changed the workspace");
    JsonObject historyCommand;put(historyCommand,L"op","history");put(historyCommand,L"expected_version",text(applied,L"version"));put(historyCommand,L"redo",false);
    auto undone=call(historyCommand);require(undone.GetNamedBoolean(L"ok")&&text(undone,L"version")==text(original,L"version"),"MCP Undo did not restore the complete transaction");
    put(historyCommand,L"expected_version",text(undone,L"version"));put(historyCommand,L"redo",true);
    auto redone=call(historyCommand);require(redone.GetNamedBoolean(L"ok")&&text(redone,L"version")==text(applied,L"version"),"MCP Redo changed the transaction");
    JsonObject focus;put(focus,L"op","focus");put(focus,L"expected_version",text(redone,L"version"));put(focus,L"workflow",1);put(focus,L"node",1);
    require(call(focus).GetNamedBoolean(L"ok")&&testMcpFocusedGraph==0&&testMcpFocusedNode==1,"MCP cannot focus a node in the actual sidebar workflow");
}
void testMcpDiagnosticsLease()
{
    gestureScenario="Remote telemetry is sampled while hidden and expires";
    EditorHarness harness;McpJson::Apartment apartment;using namespace McpJson;
    service().setTelemetryEnabled(false);service().requestTelemetry(std::chrono::milliseconds(0));
    JsonObject command;put(command,L"op","diagnostics");const auto response=JsonObject::Parse(winrt::to_hstring(NodeEditor_McpCall(encode(command))));
    require(response.GetNamedBoolean(L"ok")&&response.GetNamedNumber(L"sampling_lease_ms")==1000,"MCP cannot request bounded diagnostics");
    require(response.GetNamedNumber(L"execution_revision")==service().revision(),"MCP diagnostics omitted the sampled execution revision");
    put(command,L"workflow",999999);const auto missing=JsonObject::Parse(winrt::to_hstring(NodeEditor_McpCall(encode(command))));
    require(!missing.GetNamedBoolean(L"ok")&&text(missing,L"code")=="not_found","MCP reported success for missing workflow diagnostics");
    std::this_thread::sleep_for(std::chrono::milliseconds(35));NodeGraph::Context context;service().evaluate(context);
    const auto sampled=service().telemetry().graphSampledAt[0];require(sampled.time_since_epoch().count()!=0,"Hidden runtime ignored the remote telemetry lease");
    service().requestTelemetry(std::chrono::milliseconds(0));std::this_thread::sleep_for(std::chrono::milliseconds(35));service().evaluate(context);
    require(service().telemetry().graphSampledAt[0]==sampled,"Remote diagnostics left continuous telemetry enabled");
    service().setTelemetryEnabled(true);
}
void testMcpApplicationSettings()
{
    gestureScenario="Application MCP isolates graph settings and applies validated global settings atomically";
    TemporaryWorkspace directory;EditorHarness harness;using namespace McpJson;
    struct Restore{Config previous=config;~Restore(){config=std::move(previous);}} restore;
    require(config.loadConfig(),"Cannot initialize test application settings");
    const auto call=[](const JsonObject& command){return JsonObject::Parse(winrt::to_hstring(NodeEditor_McpCall(encode(command))));};
    JsonObject read;put(read,L"op","application");const auto original=call(read);
    require(original.GetNamedBoolean(L"ok")&&original.GetNamedArray(L"fields").Size()>=40,"MCP global settings discovery is incomplete");
    const auto execution=service().revision();const auto undoCount=editor.undo.size();const auto notifications=testApplicationNotifications;
    JsonObject change,settings;put(change,L"op","application_edit");put(change,L"expected_settings_version",text(original,L"settings_version"));
    put(settings,L"overlay_ui_scale",1.2);put(settings,L"overlay_sidebar_collapsed",true);put(change,L"settings",settings);put(change,L"dry_run",true);
    require(!call(change).GetNamedBoolean(L"applied")&&config.overlay_ui_scale!=1.2f&&testApplicationNotifications==notifications,"Settings dry run changed the application");
    put(change,L"dry_run",false);
    require(!JsonObject::Parse(winrt::to_hstring(NodeEditor_McpCall(encode(change),true))).GetNamedBoolean(L"ok"),"Application MCP ignored busy UI state");
    put(settings,L"overlay_opacity",10);put(change,L"settings",settings);
    require(!call(change).GetNamedBoolean(L"ok")&&text(call(read),L"settings_version")==text(original,L"settings_version")&&testApplicationNotifications==notifications,"Invalid settings batch partially applied");
    settings.Remove(L"overlay_opacity");put(change,L"settings",settings);const auto edited=call(change);
    require(edited.GetNamedBoolean(L"applied")&&edited.GetNamedBoolean(L"saved")&&config.overlay_sidebar_collapsed&&config.overlay_ui_scale==1.2f,"Global settings were not applied and saved");
    require(!call(change).GetNamedBoolean(L"ok"),"Stale application settings overwrote newer values");
    require(service().revision()==execution&&editor.undo.size()==undoCount&&testApplicationNotifications==notifications+1,"Application settings changed graph execution/history or reconnected twice");
    Config loaded=config;require(loaded.loadConfig()&&loaded.overlay_ui_scale==1.2f&&loaded.overlay_sidebar_collapsed,"Global settings were not persisted");
    JsonObject invalid;put(invalid,L"capture_fps",30);put(change,L"settings",invalid);put(change,L"expected_settings_version",text(edited,L"settings_version"));
    require(!call(change).GetNamedBoolean(L"ok"),"MCP allowed a global setting to bypass the graph");
    JsonArray keys;keys.Append(JsonValue::CreateStringValue(L"Unknown key"));invalid.Clear();put(invalid,L"button_exit",keys);put(change,L"settings",invalid);
    require(!call(change).GetNamedBoolean(L"ok"),"MCP accepted a nonfunctional hotkey name");
    invalid.Clear();put(invalid,L"arduino_port","COM1\nlocal_mcp_enabled=false");put(change,L"settings",invalid);
    require(!call(change).GetNamedBoolean(L"ok"),"MCP accepted INI line injection through a setting");
}
void testMcpClientPresets()
{
    gestureScenario="Client presets preserve Windows paths, instance names and PowerShell quoting";
    const std::string script="C:\\Test user's folder\\workflow_mcp.py";
    for(const auto client:{WorkflowMcp::Client::ClaudeCode,WorkflowMcp::Client::ClaudeDesktop,WorkflowMcp::Client::Generic})
    {
        const auto config=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(WorkflowMcp::connectionText(client,script,"qa_1"))).GetNamedObject(L"mcpServers").GetNamedObject(L"sunone-workflows-qa_1");
        require(winrt::to_string(config.GetNamedArray(L"args").GetStringAt(1))==script&&config.GetNamedArray(L"args").GetStringAt(3)==L"qa_1","Client JSON lost a path or instance");
    }
    const auto command=WorkflowMcp::connectionText(WorkflowMcp::Client::Codex,script,"qa_1",true);
    require(command.find("user''s folder")!=std::string::npos&&command.find("--instance 'qa_1'")!=std::string::npos,"Install command cannot handle spaces/apostrophes or instance names");
    require(WorkflowMcp::connectionText(WorkflowMcp::Client::Codex,script,"default").find("[mcp_servers.sunone-workflows]")==0,"Codex preset is not TOML");
}
}
