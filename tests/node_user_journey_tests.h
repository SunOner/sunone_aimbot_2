#pragma once
namespace
{
int journeyAdd(EditorHarness& h,NodeKind kind,float x,float y)
{
    h.move(canvasAt(h,x,y));chord(h,ImGuiMod_Ctrl,ImGuiKey_Space);typeCatalogQuery(h,definition(kind).title);
    require(!editor.catalogSearch.nodes.empty()&&editor.catalogSearch.nodes[0]==kind,"Repeated library searches kept an old selection/query");
    h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);h.frame();
    const int id=editor.selected[0];require(findNode(h.graph(),id)&&findNode(h.graph(),id)->kind==kind,"Library confirmation created the wrong node");return id;
}
void journeyNumber(EditorHarness& h,int id,const char* text,double expected)
{
    h.click(inlineField(h,id));chord(h,ImGuiMod_Ctrl,ImGuiKey_A);ImGui::GetIO().AddInputCharactersUTF8(text);h.frame();
    h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);h.frame();
    require(findNode(h.graph(),id)->value==expected,"Entering a literal value did not publish the committed value");
}
void testUserBuildsCopiesAndRestoresAnArithmeticWorkflow()
{
    for(float zoom:{1.0f,1.3f})
    {
        gestureScenario="Build, wire, group, copy, edit and Undo a complete workflow at zoom "+std::to_string(zoom);
        TemporaryWorkspace files;EditorHarness h;h.windowWidth=1960;ImGui::GetIO().DisplaySize=ImVec2(2000,1000);
        auto& graph=h.graph();graph.nodes.clear();graph.links.clear();graph.groups.clear();graph.ties.clear();graph.zoom=zoom;edited();h.frame();
        const int a=journeyAdd(h,NodeKind::Number,32,32);journeyNumber(h,a,"5",5);
        const int b=journeyAdd(h,NodeKind::Number,32,288);journeyNumber(h,b,"7",7);
        const int sum=journeyAdd(h,NodeKind::Add,448,32),preview=journeyAdd(h,NodeKind::ImagePreview,768,32);
        h.fastDrag(h.port(a,true),h.port(sum,false,0));h.fastDrag(h.port(b,true),h.port(sum,false,1));h.fastDrag(h.port(sum,true),h.port(preview,false,1));
        require(graph.links.size()==3&&!editor.wireNode,"Building a graph left a dangling connection or dropped a wire");
        const auto check=[&](int id,const char* expected){service().setPreviewNodes({previewKey(0,id)});std::this_thread::sleep_for(std::chrono::milliseconds(35));service().evaluate(Context{});const auto t=service().telemetry();
            const auto found=std::find_if(t.graphs[0].begin(),t.graphs[0].end(),[&](const NodeValue& n){return n.nodeId==id;});
            require(found!=t.graphs[0].end()&&found->value.preview&&found->value.preview->text==expected,(std::string("Preview ")+std::to_string(id)+" expected "+expected+", got "+(found!=t.graphs[0].end()&&found->value.preview?found->value.preview->text:"no data")).c_str());};
        check(preview,"12");h.click(canvasAt(h,640,448));chord(h,ImGuiMod_Ctrl,ImGuiKey_A);chord(h,ImGuiMod_Ctrl,ImGuiKey_G);
        require(graph.groups.size()==1&&graph.groups[0].nodes.size()==4,"Grouping the built workflow lost its members");
        chord(h,ImGuiMod_Ctrl,ImGuiKey_C);h.move(canvasAt(h,896,416));chord(h,ImGuiMod_Ctrl,ImGuiKey_V);h.frame();
        require(graph.nodes.size()==8&&graph.links.size()==6&&graph.groups.size()==2,"Copy/paste did not preserve the complete grouped workflow");
        int copy=0;for(const auto& n:graph.nodes)if(n.kind==NodeKind::ImagePreview&&n.id!=preview)copy=n.id;require(copy>0,"Copied graph lost Preview");check(copy,"12");
        history(false);h.frame();require(h.graph().nodes.size()==4&&h.graph().links.size()==3,"Undo of paste also removed earlier edits");
        history(true);h.frame();require(h.graph().nodes.size()==8&&h.graph().links.size()==6,"Redo of paste lost its wires");
        journeyNumber(h,a,"9",9);check(preview,"16");check(copy,"12");
        history(false);h.frame();check(preview,"12");history(true);h.frame();check(preview,"16");
        std::string document,error;Workspace restored;require(serialize(service().workspace(),document,error)&&deserialize(document,restored,error),error.c_str());
        require(restored.graphs[0].nodes.size()==8&&restored.graphs[0].links.size()==6&&restored.graphs[0].groups.size()==2,"Export/import lost part of the edited graph");
    }
}
void testRoutingEditsStayUndoableForEveryDataType()
{
    const std::vector<NodeKind> sources={NodeKind::Boolean,NodeKind::Number,NodeKind::Integer,NodeKind::MakePoint,NodeKind::Detections,NodeKind::ChooseTarget,
        NodeKind::PixelsToCounts,NodeKind::CurrentFrame,NodeKind::DepthInference,NodeKind::DepthToMask,NodeKind::String,NodeKind::PrimaryMonitor,NodeKind::GetMonitors,
        NodeKind::OverlayBoxes,NodeKind::DatasetLabels,NodeKind::MotionEstimate,NodeKind::MouseProfile,NodeKind::PredictPath,NodeKind::DrawPoint,NodeKind::DrawingStyle};
    for(auto source:sources)for(float zoom:{0.35f,0.75f,1.25f})
    {
        gestureScenario=std::string("Route edit/copy/bypass/delete/history: ")+definition(source).title+" zoom "+std::to_string(zoom);
        TemporaryWorkspace files;EditorHarness h;h.graphIndex=3;h.windowWidth=1960;ImGui::GetIO().DisplaySize=ImVec2(2000,1000);auto& graph=h.graph();
        graph.nodes={defaultNode(source,1,64,64),defaultNode(rerouteKind(definition(source).outputs[0].type),2,384,96),defaultNode(NodeKind::ImagePreview,3,640,64)};
        graph.links={{1,0,2,0},{2,0,3,1}};graph.groups={{1,"Route",352,32,128,160,0x40ffcd60u,{2}}};graph.ties.clear();graph.zoom=zoom;edited();h.frame();
        h.click(canvasAt(h,32,400));selectOne(3,2);chord(h,ImGuiMod_Ctrl,ImGuiKey_B);require(findNode(graph,2)->bypassed,"Routing bypass shortcut failed for this type");
        chord(h,ImGuiMod_Ctrl,ImGuiKey_C);h.move(canvasAt(h,512,416));chord(h,ImGuiMod_Ctrl,ImGuiKey_V);require(graph.nodes.size()==4,"Copying a typed reroute lost its node");
        history(false);h.frame();require(h.graph().nodes.size()==3,"Undo of reroute paste failed");selectOne(3,2);
        h.key(ImGuiKey_Delete,true);h.key(ImGuiKey_Delete,false);
        require(h.graph().nodes.size()==2&&h.graph().links.size()==1&&h.graph().links[0].fromNode==1&&h.graph().links[0].toNode==3&&h.graph().links[0].toPort==1,"Deleting a typed routing point broke the existing signal path");
        for(const auto& group:h.graph().groups)require(std::find(group.nodes.begin(),group.nodes.end(),2)==group.nodes.end(),"A group retained a deleted routing point");
        history(false);h.frame();require(findNode(h.graph(),2)&&findNode(h.graph(),2)->bypassed&&h.graph().links.size()==2,"Undo did not restore routing state and both wires");
        history(true);h.frame();std::string error;require(h.graph().nodes.size()==2&&validate(service().workspace(),error),error.c_str());
    }
}
void testMixedEditingSequencesRoundTripHistoryAndFiles()
{
    for(float zoom:{0.4f,0.8f,1.2f})
    {
        TemporaryWorkspace files;EditorHarness h;h.windowWidth=1960;ImGui::GetIO().DisplaySize=ImVec2(2000,1000);
        h.graph().nodes={defaultNode(NodeKind::Number,1,64,64),defaultNode(NodeKind::Add,2,384,64),defaultNode(NodeKind::RerouteNumber,3,672,96),defaultNode(NodeKind::ImagePreview,4,800,64)};
        h.graph().links={{1,0,2,0},{2,0,3,0},{3,0,4,1}};h.graph().groups.clear();h.graph().ties.clear();h.graph().zoom=zoom;edited();h.frame();
        std::uint32_t seed=0x2b9713u;
        const auto random=[&](){seed=seed*1664525u+1013904223u;return seed;};
        const auto document=[&](){auto workspace=service().workspace();for(auto& graph:workspace.graphs){graph.panX=graph.panY=0;graph.zoom=1;}
            std::string text,error;require(serialize(workspace,text,error),error.c_str());return text;};
        for(int step=0;step<120;++step)
        {
            const auto choice=random()%7;gestureScenario="Mixed edit sequence zoom "+std::to_string(zoom)+" step "+std::to_string(step)+" action "+std::to_string(choice);
            h.move(canvasAt(h,32,400));const auto index=random()%h.graph().nodes.size();const int selected=h.graph().nodes[index].id;selectOne(0,selected);
            const auto before=document();const auto historySize=editor.undo.size();
            switch(choice)
            {
            case 0:h.key(ImGuiKey_RightArrow,true);h.key(ImGuiKey_RightArrow,false);break;
            case 1:chord(h,ImGuiMod_Ctrl,ImGuiKey_M);break;
            case 2:if(canBypass(findNode(h.graph(),selected)->kind))chord(h,ImGuiMod_Ctrl,ImGuiKey_B);break;
            case 3:if(!reroute(findNode(h.graph(),selected)->kind)){h.key(ImGuiKey_Space,true);h.key(ImGuiKey_Space,false);}break;
            case 4:chord(h,ImGuiMod_Ctrl,ImGuiKey_C);h.move(canvasAt(h,640,448));chord(h,ImGuiMod_Ctrl,ImGuiKey_V);break;
            case 5:if(h.graph().nodes.size()>2){h.key(ImGuiKey_Delete,true);h.key(ImGuiKey_Delete,false);}break;
            case 6:
                editor.selection[0]={selected,h.graph().nodes[(index+1)%h.graph().nodes.size()].id};chord(h,ImGuiMod_Ctrl,ImGuiKey_G);break;
            }
            h.frame();std::string error;require(validate(service().workspace(),error),error.c_str());
            const auto after=document();
            if(after!=before)
            {
                require(!editor.undo.empty()&&editor.undo.size()>=historySize&&editor.undo.size()<=historySize+1,"One user action created multiple undo transactions or cleared history");
                chord(h,ImGuiMod_Ctrl,ImGuiKey_Z);require(document()==before,"Undo did not restore the complete previous graph");
                chord(h,ImGuiMod_Ctrl,ImGuiKey_Y);require(document()==after,"Redo did not restore the complete edited graph");
            }
            else require(editor.undo.size()==historySize,"A no-op polluted the history");
            Workspace restored;require(deserialize(after,restored,error)&&validate(restored,error),error.c_str());
            service().evaluate(Context{});
            require(!editor.wireNode&&!editor.keyboardMove&&editor.panButton<0,"Mixed shortcuts left a mouse/keyboard gesture active");
        }
    }
}
}
