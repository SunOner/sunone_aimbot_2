#pragma once
#include "../sunone_aimbot_2/overlay/export_job.h"
#include <future>
namespace
{
void testSharedCalibrationTemplatesAndLayout()
{
    gestureScenario="Shared calibration templates and overview";TemporaryWorkspace files;EditorHarness harness;std::string error;
    editor.draft.graphs.clear();edited();
    const int aim=NodeEditor_CreateWorkflow("Aim copy",0,error,8,true);require(aim>0,"Aiming template could not create its shared profile dependency");
    auto workspace=service().workspace();int profileWorkflow=0,sources=0;
    for(std::size_t g=0;g<workspace.graphs.size();++g)for(const auto& node:workspace.graphs[g].nodes)if(node.kind==NodeKind::MouseProfile){++sources;profileWorkflow=workflowId(workspace.graphs[g],g);}
    require(sources==1&&profileWorkflow==aim,"An empty workspace received a missing or duplicated profile source");
    require(NodeEditor_CreateWorkflow("Recoil copy",2,error,5,true)>0,"Recoil template could not reuse the shared source");
    workspace=service().workspace();sources=0;for(const auto& page:workspace.graphs)for(const auto& node:page.nodes)sources+=node.kind==NodeKind::MouseProfile;
    require(sources==1,"Adding a recoil template duplicated profile settings");
    std::vector<Node> copies{defaultNode(NodeKind::MouseProfile,1),defaultNode(NodeKind::SharedCalibration,2)};
    remapNamedCopies(editor.draft.graphs[0],copies);require(textParameter(copies[0],"name")!="mouse"&&textParameter(copies[0],"name")==textParameter(copies[1],"name"),"Copying a source with a reader shadowed the global channel or broke their pairing");
    auto graph=defaults().graphs[0];const ImVec2 size{1034,644};fitGraph(graph,size);
    for(const auto& node:graph.nodes)
    {
        const auto position=screenPoint(graph,{0,0},node.x,node.y);
        require(position.x>=0&&position.y>=0&&position.x+cardWidth(node)*graph.zoom<=size.x&&position.y+cardHeight(node)*graph.zoom<=size.y,"The default split workflow is clipped in its initial overview");
    }
}
void testWorkflowManagementIsImmediateAndUndoable()
{
    gestureScenario="Workflow templates, pages and appearance";TemporaryWorkspace files;EditorHarness harness;std::string error;
    const int created=NodeEditor_CreateWorkflow("Custom vision",3,error,13);
    require(created>4,"An empty additional workflow could not be created");
    auto workspace=service().workspace();const int first=activePage(workspace,created);
    require(first>=0&&workspace.graphs[first].nodes.empty()&&workspace.graphs[first].workflowIcon==13,"Additional workflows contain unsolicited starter nodes or lost their icon");
    require(kindAllowed(workspace.graphs[first],NodeKind::CaptureSource)&&kindAllowed(workspace.graphs[first],NodeKind::CurrentFrame)&&kindAllowed(workspace.graphs[first],NodeKind::VisionOutput),"Starter sources and outputs are hidden from an empty workflow");
    require(NodeEditor_ConfigureWorkflow(created,"Renamed vision",2,error),"Workflow appearance could not be changed");
    workspace=service().workspace();require(workspace.graphs[first].workflowName=="Renamed vision"&&workspace.graphs[first].workflowIcon==2,"Appearance does not apply to every workflow page");
    require(!NodeEditor_ConfigureWorkflow(created,"Aim Graph",2,error),"Duplicate workflow names were accepted");
    require(NodeEditor_RemoveWorkflow(created,error)&&activePage(service().workspace(),created)<0,"Custom workflow deletion did not apply immediately");
    history(false);require(activePage(service().workspace(),created)>=0,"Undo did not restore a removed workflow");history(true);
    require(NodeEditor_RemoveWorkflow(1,error)&&activePage(service().workspace(),1)<0,"The starting templates still have undeletable special status");
    const int templated=NodeEditor_CreateWorkflow("Aim template copy",0,error,8,true);
    workspace=service().workspace();const auto& page=workspace.graphs[activePage(workspace,templated)];
    require(!page.nodes.empty()&&page.workflowIcon==8&&kindAllowed(page,NodeKind::RecoilOutput)&&kindAllowed(page,NodeKind::AutoShootOutput),"A filled template did not copy its graph or still restricts control output choices");
    require(!NodeEditor_RemoveWorkflow(999999,error),"Deleting an absent workflow silently succeeded");
}
void testWorkflowUsesCanvasWithoutPageTabs()
{
    gestureScenario="Single graph workflow canvas";TemporaryWorkspace files;EditorHarness harness;harness.workflow=1;
    harness.frame();harness.frame();
    require(GImGui->TabBars.GetMapSize()==0,"Workflow still renders a duplicate page tab bar");
    const float aimTop=editor.canvasOrigin[0].y;
    harness.workflow=3;harness.frame();harness.frame();
    require(editor.activeGraph==2&&std::abs(editor.canvasOrigin[2].y-aimTop)<1,"Switching the sidebar workflow did not open its canvas at the same position");
    std::string error;
    editor.draft.graphs[3].nodes={defaultNode(NodeKind::CaptureSource,1)};edited();
    const int vision=NodeEditor_CreateWorkflow("Vision template copy",3,error,0,true);
    require(vision>0,"An additional Vision template conflicts with the existing capture workflow");
    const auto workspace=service().workspace();const auto& page=workspace.graphs[activePage(workspace,vision)];
    require(std::any_of(page.nodes.begin(),page.nodes.end(),[](const Node& node){return node.kind==NodeKind::CurrentFrame;})&&std::none_of(page.nodes.begin(),page.nodes.end(),[](const Node& node){return node.kind==NodeKind::CaptureSource;}),"An additional Vision template starts a second physical capture");
}
void testModelExportJobLifecycle()
{
    gestureScenario="Background model export lifecycle";ModelExport::Job job;
    std::promise<void> entered,release;auto ready=entered.get_future();auto unblock=release.get_future().share();
    require(job.start("model.onnx",[&](const auto&){entered.set_value();unblock.wait();return std::string("model.engine");}),"Export could not start");
    require(ready.wait_for(std::chrono::seconds(2))==std::future_status::ready,"Export did not start in the background");
    require(job.snapshot().state==ModelExport::State::Running&&!job.start("second.onnx",[](const auto&){return std::string("second.engine");}),"Concurrent exports were accepted or UI status blocked");
    release.set_value();
    const auto wait=[&]{const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(job.snapshot().state==ModelExport::State::Running&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();return job.snapshot();};
    require(wait().state==ModelExport::State::Complete&&job.snapshot().output=="model.engine","Export result was lost");
    require(job.start("bad.onnx",[](const auto&)->std::string{throw std::runtime_error("Builder error");}),"Completed export could not restart");
    require(wait().state==ModelExport::State::Failed&&job.snapshot().message=="Builder error","Builder errors did not reach the UI");
    require(job.start("cancel.onnx",[](const auto& cancel){while(!cancel.load())std::this_thread::yield();return std::string{};}),"Cancelled export could not start");
    job.cancel();require(wait().state==ModelExport::State::Cancelled,"Cancellation was reported as a failure");job.shutdown();
}
}
