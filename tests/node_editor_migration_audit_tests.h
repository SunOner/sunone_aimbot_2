#pragma once
namespace
{
void testRuntimeTuningAndReloadUseEditorTransactions()
{
    gestureScenario="Runtime tuning and reload transactions";TemporaryWorkspace directory;EditorHarness harness;
    editor.draft=defaults();auto inactive=editor.draft.graphs[0];inactive.workflow=5;inactive.workflowName="Disabled logic";inactive.enabled=false;const int inactiveIndex=static_cast<int>(editor.draft.graphs.size());editor.draft.graphs.push_back(inactive);edited();
    const auto findAim=[](const Workspace& workspace,int graph)->const Node&{for(const auto& node:workspace.graphs[graph].nodes)if(node.kind==NodeKind::TargetAnchor)return node;throw std::runtime_error("Missing aim offsets");};
    const double before=parameter(findAim(service().workspace(),0),"body_y_offset");const auto revision=service().revision();
    auto request=std::async(std::launch::async,[]{NodeEditor_RequestTuning(1,0,false);});request.get();
    require(service().revision()==revision,"The input thread published an editor change itself");NodeEditor_ProcessPendingActions();
    require(std::abs(parameter(findAim(service().workspace(),0),"body_y_offset")-before-0.01)<1e-9&&parameter(findAim(service().workspace(),inactiveIndex),"body_y_offset")==before,"Arrow tuning changed an inactive page or failed to apply immediately");
    history(false);require(parameter(findAim(service().workspace(),0),"body_y_offset")==before,"Arrow tuning bypassed Undo");history(true);
    auto& aim=editor.draft.graphs[0];const int target=findAim(editor.draft,0).id;
    aim.nodes.push_back(defaultNode(NodeKind::Number,999));aim.links.push_back({999,0,target,1});edited();
    const auto tuned=parameter(findAim(service().workspace(),0),"body_y_offset");NodeEditor_RequestTuning(1,0,false);NodeEditor_ProcessPendingActions();
    require(parameter(findAim(service().workspace(),0),"body_y_offset")==tuned,"Arrow tuning changed a value driven by another node");
    editor.editedAt=std::chrono::steady_clock::now()-std::chrono::seconds(1);NodeEditor_ProcessPendingActions();
    require(!editor.dirty&&std::filesystem::is_regular_file(GraphFile),"Tuning did not autosave while no editor frame was drawn");
    auto disk=service().workspace();for(auto& node:disk.graphs[0].nodes)if(node.kind==NodeKind::TargetAnchor)node.parameters["head_y_offset"]=0.7;
    std::string text,error;require(serialize(disk,text,error),"Cannot create reload fixture");{std::ofstream file(GraphFile);file<<text;}
    NodeEditor_RequestReload();NodeEditor_ProcessPendingActions();
    require(!editor.error&&!editor.loadFailed&&parameter(findAim(service().workspace(),0),"head_y_offset")==0.7,"Reload did not load workflows");
    history(false);require(parameter(findAim(service().workspace(),0),"head_y_offset")!=0.7,"Workflow reload bypassed Undo");
    const auto valid=service().revision();{std::ofstream file(GraphFile);file<<"broken graph";}
    NodeEditor_RequestReload();NodeEditor_ProcessPendingActions();
    require(service().revision()==valid&&editor.loadFailed&&!editor.dirty,"A malformed reload replaced the running workspace or enabled overwriting the bad file");
    {std::ofstream file(GraphFile);file<<text;}NodeEditor_RequestReload();NodeEditor_ProcessPendingActions();
    require(!editor.loadFailed&&parameter(findAim(service().workspace(),0),"head_y_offset")==0.7,"Reload could not recover from an invalid graph file");
}
}
