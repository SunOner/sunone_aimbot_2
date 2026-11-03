#pragma once

namespace
{
std::string legacyOutputFormat(const std::string& text)
{
    std::istringstream rows(text);std::ostringstream legacy;std::string line;
    while(std::getline(rows,line))
    {
        if(line.rfind("PAGES ",0)==0||line.rfind("PAGE ",0)==0)continue;
        if(line.rfind("SUNONE_NODE_GRAPH ",0)==0)line="SUNONE_NODE_GRAPH 11";
        else if(line.rfind("LINK ",0)==0)for(int field=0;field<2;++field)line.resize(line.rfind(' '));
        legacy<<line<<'\n';
    }
    return legacy.str();
}
void testCompactStylesAreLiveSharedAndLazy()
{
    auto workspace=emptyWorkspace();auto& graph=workspace.graphs[3];
    graph.nodes={defaultNode(NodeKind::CaptureSource,1),defaultNode(NodeKind::OverlayBoxes,2),defaultNode(NodeKind::OverlayDepth,3),
        defaultNode(NodeKind::OverlayOutput,4),defaultNode(NodeKind::DepthInference,5),defaultNode(NodeKind::DepthToMask,6),
        defaultNode(NodeKind::Integer,7),defaultNode(NodeKind::KeyHeld,8),defaultNode(NodeKind::SetOverlay,9),
        defaultNode(NodeKind::GetOverlay,10),defaultNode(NodeKind::RerouteOverlay,11)};
    node(graph,4).parameters["enabled"]=1;node(graph,5).parameters["enabled"]=1;node(graph,6).parameters["enabled"]=1;
    node(graph,7).value=191;node(graph,8).parameters["virtual_key"]=65;
    node(graph,9).textParameters["name"]="hud";node(graph,10).textParameters["name"]="hud";
    graph.links={{2,0,3,0},{3,0,9,0},{10,0,11,0},{11,0,4,5},{1,0,4,1},{1,0,5,0},{5,0,6,0},{5,0,4,4},{6,0,4,3},
        {7,0,2,optionPin(NodeKind::OverlayBoxes,"game_overlay_box_r")},{8,0,3,optionPin(NodeKind::OverlayDepth,"depth_debug_overlay_enabled")}};
    Service service;VisionFixture fixture;bool held=false;Node received;
    fixture.context.keyDown=[&](int key){return key==65&&held;};
    fixture.context.render=[&](int,const FrameStream&,const DetectionStream&,const MaskStream& mask,const FrameStream& depth,const Node& settings){
        ++fixture.renders;fixture.renderedDepth=static_cast<bool>(depth);fixture.renderedMask=static_cast<bool>(mask);received=settings;return true;};
    publish(service,workspace);service.evaluateVision(fixture.context);
    require(fixture.renders==1&&fixture.depths==0&&fixture.masks==0&&parameter(received,"game_overlay_box_r")==191,
        "A compact style lost a wired value or executed disabled depth/mask layers");
    require(parameter(received,"game_overlay_draw_boxes")==1&&parameter(received,"game_overlay_draw_future")==0,
        "Unconnected overlay layers remained implicitly enabled");
    const auto first=service.telemetry();const auto style=sampleNode(first,3,3).value.settings;
    std::this_thread::sleep_for(std::chrono::milliseconds(35));service.evaluateVision(fixture.context);
    require(style&&sampleNode(service.telemetry(),3,3).value.settings==style&&sampleNode(service.telemetry(),3,3).signalSequence==sampleNode(first,3,3).signalSequence,
        "Unchanged styles were allocated again or generated synthetic pulses");
    held=true;service.evaluateVision(fixture.context);
    require(fixture.renderedDepth&&fixture.depths==1&&fixture.masks==0,"A live style wire did not enable only the selected depth branch");
    node(graph,3).bypassed=true;publish(service,workspace);service.evaluateVision(fixture.context);
    require(!fixture.renderedDepth&&parameter(received,"game_overlay_box_r")==191,"Bypassing a style did not preserve the preceding layers");
    graph.links.erase(std::remove_if(graph.links.begin(),graph.links.end(),[](const Link& link){return link.toNode==4&&link.toPort==5;}),graph.links.end());
    publish(service,workspace);const int captures=fixture.captures,renders=fixture.renders;service.evaluateVision(fixture.context);
    require(fixture.captures==captures&&fixture.renders==renders&&service.providerNodes(NodeKind::CaptureSource).empty(),"An output without a style kept acquiring frames");
}

void testOutputMigrationKeepsSettingsWiresAndState()
{
    auto workspace=emptyWorkspace();auto& graph=workspace.graphs[3];
    graph.nodes={defaultNode(NodeKind::CaptureSource,1),defaultNode(NodeKind::RenderOverlay,2),defaultNode(NodeKind::CollectDataset,3),
        defaultNode(NodeKind::Integer,4),stringNode(5,u8"icons/пример.png"),defaultNode(NodeKind::Float,6),defaultNode(NodeKind::Boolean,7)};
    node(graph,2).parameters["enabled"]=1;node(graph,2).parameters["game_overlay_box_g"]=47;
    node(graph,2).parameters["game_overlay_icon_enabled"]=1;node(graph,2).parameters["snap_boost"]=1.8;
    node(graph,3).parameters["enabled"]=1;node(graph,3).parameters["auto_label_data"]=1;
    node(graph,3).textParameters["collect_output_dir"]=u8"dataset/пример";node(graph,3).textParameters["auto_label_record_classes"]="1,3";
    node(graph,4).value=173;node(graph,6).value=0.42;
    graph.links={{1,0,2,1},{1,0,3,1},{4,0,2,optionPin(NodeKind::RenderOverlay,"game_overlay_box_r")},
        {5,0,2,optionPin(NodeKind::RenderOverlay,"game_overlay_icon_path")},{6,0,3,optionPin(NodeKind::CollectDataset,"auto_label_min_conf")},
        {7,0,2,optionPin(NodeKind::RenderOverlay,"depth_mask_enabled")}};
    node(graph,7).value=0;graph.ties={{1,512,256,15}};graph.links[2].ties={1};
    graph.groups={{1,"Existing outputs",0,0,2048,1536,0x40ffcd60u,{1,2,3,4,5,6,7}}};
    Node oldRender,oldCollect,newRender,newCollect;VisionFixture before,after;
    before.context.render=[&](int,const FrameStream&,const DetectionStream&,const MaskStream&,const FrameStream&,const Node& n){oldRender=n;return true;};
    before.context.collect=[&](int,const FrameStream&,const DetectionStream&,const Node& n){oldCollect=n;return true;};
    after.context.render=[&](int,const FrameStream&,const DetectionStream&,const MaskStream&,const FrameStream&,const Node& n){newRender=n;return true;};
    after.context.collect=[&](int,const FrameStream&,const DetectionStream&,const Node& n){newCollect=n;return true;};
    Service original;publish(original,workspace);original.evaluateVision(before.context);
    std::string text,error;require(serialize(workspace,text,error),error.c_str());text=legacyOutputFormat(text);
    Workspace loaded;require(deserialize(text,loaded,error),error.c_str());auto& migrated=loaded.graphs[3];
    require(node(migrated,2).kind==NodeKind::OverlayOutput&&node(migrated,3).kind==NodeKind::DatasetOutput&&migrated.nodes.size()==14,
        "A legacy output was not split into its compact parts");
    require(Layout::height(node(migrated,2))<250&&definition(NodeKind::OverlayOutput).inputs.size()==7,"The migrated render card still contains all layer options");
    require(migrated.links[2].ties==std::vector<int>{1}&&migrated.links[2].toNode!=2&&migrated.ties[0].angle==15,"Migration lost a wired setting's routing");
    Service converted;publish(converted,loaded);converted.evaluateVision(after.context);
    require(newRender.parameters==oldRender.parameters&&newRender.textParameters==oldRender.textParameters&&
        newCollect.parameters==oldCollect.parameters&&newCollect.textParameters==oldCollect.textParameters,
        "Splitting changed settings delivered to native rendering or dataset recording");
    require(serialize(loaded,text,error),error.c_str());Workspace reopened;require(deserialize(text,reopened,error),error.c_str());
    require(reopened.graphs[3].nodes.size()==migrated.nodes.size()&&reopened.graphs[3].links.size()==migrated.links.size(),"Loading split outputs split them again");
    for(const auto& item:migrated.nodes)require(node(reopened.graphs[3],item.id).x==item.x&&node(reopened.graphs[3],item.id).y==item.y,"Reopening changed the migrated layout");
    node(workspace.graphs[3],2).disabled=true;node(workspace.graphs[3],2).collapsed=true;node(workspace.graphs[3],2).bypassed=true;
    require(serialize(workspace,text,error),error.c_str());text=legacyOutputFormat(text);require(deserialize(text,loaded,error),error.c_str());
    require(node(loaded.graphs[3],2).disabled&&node(loaded.graphs[3],2).collapsed&&node(loaded.graphs[3],2).bypassed,"Splitting lost an output's disabled, collapsed or bypass state");
}

void testCompactDatasetLabelsAndCapacityFallback()
{
    auto workspace=emptyWorkspace();auto& graph=workspace.graphs[3];
    graph.nodes={defaultNode(NodeKind::CaptureSource,1),defaultNode(NodeKind::DatasetOutput,2),defaultNode(NodeKind::DatasetLabels,3)};
    node(graph,2).parameters["enabled"]=1;node(graph,3).parameters["auto_label_data"]=1;node(graph,3).parameters["auto_label_min_conf"]=0.65;
    node(graph,3).textParameters["auto_label_record_classes"]="0,2";graph.links={{1,0,2,1}};
    Service service;VisionFixture fixture;Node received;fixture.context.collect=[&](int,const FrameStream&,const DetectionStream&,const Node& n){received=n;return true;};
    publish(service,workspace);service.evaluateVision(fixture.context);require(parameter(received,"auto_label_data")==0,"Disconnected Labels enabled annotation writing");
    graph.links.push_back({3,0,2,3});publish(service,workspace);service.evaluateVision(fixture.context);
    require(parameter(received,"auto_label_data")==1&&parameter(received,"auto_label_min_conf")==0.65&&textParameter(received,"auto_label_record_classes")=="0,2","Connected label settings did not reach recording");
    workspace=emptyWorkspace();auto& full=workspace.graphs[3];full.nodes.push_back(defaultNode(NodeKind::RenderOverlay,1));
    for(int id=2;id<=static_cast<int>(MaxNodes);++id)full.nodes.push_back(defaultNode(NodeKind::Boolean,id));
    std::string text,error;require(serialize(workspace,text,error),error.c_str());text=legacyOutputFormat(text);Workspace loaded;
    require(deserialize(text,loaded,error)&&loaded.graphs[3].nodes.size()==MaxNodes&&node(loaded.graphs[3],1).kind==NodeKind::RenderOverlay,
        "A full legacy graph lost nodes or settings during automatic splitting");
    auto starter=defaults();require(node(starter.graphs[3],13).kind==NodeKind::OverlayOutput,"Starter graph still uses the oversized output");
    for(const auto& item:starter.graphs[3].nodes)require(item.kind!=NodeKind::RenderOverlay&&item.kind!=NodeKind::CollectDataset,"Starter graph contains a legacy monolithic output");
}
void testOutputLayers()
{
    testCompactStylesAreLiveSharedAndLazy();testOutputMigrationKeepsSettingsWiresAndState();testCompactDatasetLabelsAndCapacityFallback();
}
}
