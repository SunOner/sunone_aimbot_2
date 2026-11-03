#pragma once
namespace
{
Graph workflowTestPage(int workflow,const char* name,double strength,bool active=true)
{
    Graph graph;graph.workflow=workflow;graph.workflowName="Control "+std::to_string(workflow);graph.pageName=name;graph.active=active;
    graph.nodes={defaultNode(NodeKind::Boolean,1),defaultNode(NodeKind::Number,2),defaultNode(NodeKind::RecoilOutput,3)};
    graph.nodes[0].value=1;graph.nodes[1].value=strength;graph.links={{1,0,3,0},{2,0,3,1}};return graph;
}
void testWorkflowPagesAndOrdinaryTemplates()
{
    Workspace workspace;workspace.graphs={workflowTestPage(42,"Main",2),workflowTestPage(50,"Main",3)};
    for(auto& graph:workspace.graphs)graph.pageName.clear();
    Service service;Context context;publish(service,workspace);
    require(service.evaluate(context).recoilStrength==5,"Independent workflow actions overwrite each other");
    const auto revision=service.revision();workspace.graphs[0].workflowName=u8"My logic";workspace.graphs[0].workflowIcon=8;publish(service,workspace);
    require(service.revision()==revision,"Changing a workflow name/icon restarted execution");
    workspace.graphs[0].enabled=false;publish(service,workspace);require(service.evaluate(context).recoilStrength==3,"A disabled workflow still executed");
    const auto metadata=service.workspaceMetadata();require(metadata.graphs.size()==2&&metadata.graphs[0].nodes.empty()&&metadata.graphs[0].workflowIcon==8,"Sidebar metadata copied graph payloads or lost customization");
    std::string text,error;Workspace loaded;require(serialize(workspace,text,error)&&deserialize(text,loaded,error),"Workflow metadata did not roundtrip");
    require(loaded.graphs.size()==2&&!loaded.graphs[0].enabled&&loaded.graphs[0].workflowIcon==8&&loaded.graphs[0].workflowName=="My logic","Import lost state or appearance");
    auto bad=workspace;bad.graphs[1].workflow=42;require(!validate(bad,error),"Duplicate workflow graphs were accepted");
    bad=workspace;bad.graphs[1].active=false;require(!validate(bad,error),"A hidden legacy page was accepted in the current format");
    bad=workspace;bad.graphs[1].pageName="Page";require(!validate(bad,error),"A new page was accepted in the current format");
    bad=workspace;bad.graphs[0].workflowIcon=16;require(!validate(bad,error),"An invalid workflow icon was accepted");
    workspace.graphs.erase(workspace.graphs.begin());publish(service,workspace);require(service.evaluate(context).recoilStrength==3,"Reordering workflows changed execution");
    workspace.graphs.clear();publish(service,workspace);
    require(!service.evaluate(context).recoil&&serialize(workspace,text,error)&&deserialize(text,loaded,error)&&loaded.graphs.empty(),"An empty workspace cannot be persisted");
    const std::string version14="SUNONE_NODE_GRAPH 14\nPAGES 1\nGRAPH 0 40 40 1 0 0 0 0\nPAGE 42 1 1 \"Legacy page\" \"Default\"\nEND\n";
    require(deserialize(version14,loaded,error)&&loaded.graphs[0].workflowIcon==-1&&workflowTitle(loaded.graphs[0])=="Legacy page"&&loaded.graphs[0].pageName.empty(),"Legacy workflow metadata no longer imports");
    auto legacy=defaults();for(auto& graph:legacy.graphs)graph.workflow=0;publish(service,legacy);
    require(service.workspace().graphs[3].workflow==4,"Legacy workflow identities were not frozen before deletion/reordering");
}
void testLegacyPagesBecomeIndependentWorkflows()
{
    Workspace old;old.sourceVersion=19;old.graphs={workflowTestPage(42,"Main",2),workflowTestPage(42,"Alternative",7,false),workflowTestPage(1000000,"Main",3)};
    std::string text,error;Workspace loaded;require(serialize(old,text,error),"Cannot create the legacy page fixture");
    text.replace(0,text.find('\n'),"SUNONE_NODE_GRAPH 19");
    require(deserialize(text,loaded,error),"Legacy workflow pages did not migrate");
    require(loaded.graphs.size()==3&&loaded.graphs[0].workflow==42&&loaded.graphs[2].workflow==1000000,"Migration lost a graph or changed its original sidebar identity");
    require(loaded.graphs[1].active&&!loaded.graphs[1].enabled&&loaded.graphs[1].pageName.empty()&&loaded.graphs[1].workflow!=42&&workflowTitle(loaded.graphs[1]).find("Alternative")!=std::string::npos,"An inactive page is hidden, lost its name, or executes after migration");
    Service service;Context context;publish(service,loaded);require(service.evaluate(context).recoilStrength==5,"Page migration changed the running logic");
    loaded.graphs[1].enabled=true;publish(service,loaded);require(service.evaluate(context).recoilStrength==12,"The migrated sidebar workflow cannot run independently");
    loaded.sourceVersion=CurrentFormatVersion;require(serialize(loaded,text,error)&&deserialize(text,loaded,error)&&loaded.graphs.size()==3,"Flattened workflows do not roundtrip in the current format");
}
void testGeneratedProfileContainerFoldsIntoLogic()
{
    auto old=defaults();old.sourceVersion=19;auto& aim=old.graphs[0];Node calibration;
    for(const auto& node:aim.nodes)if(node.kind==NodeKind::MouseProfile)calibration=node;
    aim.nodes.erase(std::remove_if(aim.nodes.begin(),aim.nodes.end(),[](const Node& node){return node.kind==NodeKind::MouseProfile;}),aim.nodes.end());
    for(auto& group:aim.groups)group.nodes.erase(std::remove(group.nodes.begin(),group.nodes.end(),calibration.id),group.nodes.end());
    calibration.id=1;Graph source;source.workflow=5;source.workflowName="Profiles";source.pageName="First";
    source.nodes={calibration,defaultNode(NodeKind::KeyHeld,2)};source.nodes[1].parameters["virtual_key"]=65;
    source.ties={{1,128,128,15}};source.links={{2,0,1,optionPin(NodeKind::MouseProfile,"fov_scaled"),{1}}};
    source.groups={{1,"Shared aiming and recoil calibration",0,0,512,512,0x40ffcd60u,{1,2}}};
    auto alternative=source;alternative.pageName="Second";alternative.active=false;alternative.nodes[0].parameters["sens"]=3;
    old.graphs.push_back(source);old.graphs.push_back(alternative);const auto nodes=old.graphs[0].nodes.size();
    require(migrateWorkflowPages(old)&&old.graphs.size()==5,"The generated Profiles workflow was not folded into Logic");
    require(old.graphs[0].nodes.size()==nodes+2&&old.graphs[0].ties.size()==1,"Calibration migration lost custom upstream nodes or wire ties");
    std::string error;old.sourceVersion=CurrentFormatVersion;require(validate(old,error),"Merged calibration has broken IDs, ports, groups or ties");
    int sources=0;for(const auto& graph:old.graphs){require(graph.active&&graph.pageName.empty()&&workflowTitle(graph).find("Profiles")==std::string::npos,"A profile container or hidden page remains");for(const auto& node:graph.nodes)sources+=node.kind==NodeKind::MouseProfile;}
    require(sources==2&&!old.graphs.back().enabled&&parameter(old.graphs.back().nodes[0],"sens")==3,"Inactive calibration data was discarded or enabled");
    require(!migrateWorkflowPages(old),"Workflow migration is not idempotent");
}
void testAdditionalVisionWorkflowUsesSharedFramesAndDistinctKeys()
{
    Service service;VisionFixture fixture;auto workspace=emptyWorkspace();auto& original=workspace.graphs[3];
    original.nodes={defaultNode(NodeKind::CaptureSource,1),defaultNode(NodeKind::Inference,2),defaultNode(NodeKind::VisionOutput,3)};
    original.links={{1,0,2,0},{2,0,3,1}};
    Graph extra;extra.kind=GraphKind::Vision;extra.workflow=5;extra.workflowName="Second model";
    extra.nodes={defaultNode(NodeKind::CurrentFrame,1),defaultNode(NodeKind::Inference,2),defaultNode(NodeKind::VisionOutput,3)};
    extra.links={{1,0,2,0},{2,0,3,1}};node(extra,2).textParameters["model_path"]="second.onnx";
    workspace.graphs.push_back(extra);publish(service,workspace);
    auto models=service.providerNodes(NodeKind::Inference);
    require(models.size()==2&&models[0].runtimeKey==3*NodeKeyStride+2&&models[1].runtimeKey==4*NodeKeyStride+2,"Providers with equal local node IDs share runtime state");
    const auto configured=service.evaluateConfiguration(fixture.context);
    require(configured.size()==3&&fixture.captures==0&&fixture.inferences==0,"Workflow preflight executed pixels/providers or dropped a workflow");
    fixture.context.useConfigurationSnapshot=true;
    const auto actions=service.evaluateVision(fixture.context);
    require(fixture.captures==1&&fixture.inferences==2&&actions.visionDetections&&actions.visionDetections->objects.size()==8,"Additional workflows do not reuse the frame and combine model output");
    workspace.graphs.back().nodes[0].kind=NodeKind::CaptureSource;std::string error;
    require(!validate(workspace,error)&&!service.publish(workspace,error),"Multiple reachable physical capture sources were accepted");
    workspace.graphs.back().enabled=false;require(validate(workspace,error),"An inactive capture workflow prevents editing");
}
void testAdditionalPreviewIsLazyAndUnambiguous()
{
    Service service;VisionFixture fixture;auto workspace=emptyWorkspace();workspace.graphs[3].nodes={defaultNode(NodeKind::CaptureSource,1)};
    Graph extra;extra.kind=GraphKind::Vision;extra.workflow=5;extra.workflowName="Preview";
    extra.nodes={defaultNode(NodeKind::CurrentFrame,1),defaultNode(NodeKind::ImagePreview,4)};extra.links={{1,0,4,1}};
    workspace.graphs.push_back(extra);int conversions=0;
    fixture.context.preview=[&](const Value&,const DetectionStream&,const Node&){++conversions;auto image=std::make_shared<PreviewImage>();image->width=image->height=1;image->rgba.resize(4,255);return Preview(image);};
    publish(service,workspace);service.evaluateVision(fixture.context);
    require(service.providerNodes(NodeKind::CaptureSource).empty()&&service.evaluateConfiguration(fixture.context).empty()&&conversions==0,"A hidden additional preview activated capture or pixels");
    const int key=previewKey(4,4);require(key!=previewKey(3,4)&&previewKey(0,4)!=previewKey(3,4),"Preview keys collide across reordered workflows");service.setPreviewNodes({key});
    require(service.providerNodes(NodeKind::CaptureSource).size()==1&&service.evaluateConfiguration(fixture.context).size()==1,"Shared-frame preview did not retain/configure the capture workflow");
    service.evaluateVision(fixture.context);require(conversions==1,"Additional preview did not receive the current frame");
    for(int i=0;i<20;++i)service.evaluateVision(fixture.context);require(conversions==1,"Additional preview ignores its update budget");
    service.setPreviewNodes({});service.evaluateVision(fixture.context);require(service.providerNodes(NodeKind::CaptureSource).empty(),"Closing a shared preview kept capture alive");
}
void testWorkflowRegressions()
{
    testWorkflowPagesAndOrdinaryTemplates();testLegacyPagesBecomeIndependentWorkflows();testGeneratedProfileContainerFoldsIntoLogic();testAdditionalVisionWorkflowUsesSharedFramesAndDistinctKeys();testAdditionalPreviewIsLazyAndUnambiguous();
}
}
