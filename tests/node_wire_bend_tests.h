namespace
{
void prepareBendGraph(EditorHarness& h,float zoom)
{
    h.graph().nodes={defaultNode(NodeKind::Boolean,1,32,96),defaultNode(NodeKind::Not,2,672,224)};
    h.graph().links={{1,0,2,0}};h.graph().zoom=zoom;edited();h.frame();
    editor.undo.clear();editor.redo.clear();
}
void testManualBendGesturesAcrossZoom()
{
    gestureScenario="Manual two-axis wire bends use one reversible gesture at every scale";
    for(const float zoom:GestureZooms)
    {
        EditorHarness h;prepareBendGraph(h,zoom);const auto revision=service().revision();
        const auto center=editor.canvasRoutes[0].routes[0].bendPoint;
        h.move(ImVec2(5,5));for(int i=0;i<15;++i)h.frame();
        require(editor.bendHandleFade==0,"Wire handles stay visible away from the pointer");
        h.move(center);h.button(ImGuiMouseButton_Left,true);
        require(editor.bendGraph==0&&!editor.wireNode&&!editor.dragNode&&editor.panButton<0,"The wire handle started another mouse gesture");
        const auto pan=ImVec2(h.graph().panX,h.graph().panY);
        ImGui::GetIO().AddMouseWheelEvent(0,1);h.frame();
        h.button(ImGuiMouseButton_Middle,true);h.button(ImGuiMouseButton_Middle,false);
        require(h.graph().zoom==zoom&&h.graph().panX==pan.x&&h.graph().panY==pan.y,"Navigation moved the canvas during a bend drag");
        h.releaseAt(add(center,ImVec2(54,72)));h.frame();
        require(editor.bendGraph<0&&std::abs(h.graph().links[0].bend.roundness-0.88f)<0.001f&&
            std::abs(h.graph().links[0].bend.twist-0.4f)<0.001f,"A coalesced release lost a bend axis or depended on zoom");
        require(editor.undo.size()==1&&service().revision()==revision&&sameBend(service().workspace().graphs[0].links[0].bend,h.graph().links[0].bend),
            "Bending reset execution, failed immediate publication or used more than one Undo entry");
        require(distanceSquared(center,editor.canvasRoutes[0].routes[0].bendPoint)<0.01f,"The center handle drifted while changing curvature");
        history(false);h.frame();require(sameBend(h.graph().links[0].bend,WireBend{}),"Undo did not restore the original wire");
        history(true);h.frame();require(std::abs(h.graph().links[0].bend.twist-0.4f)<0.001f,"Redo did not restore the manual bend");
        h.key(ImGuiMod_Alt,true);h.click(center);h.key(ImGuiMod_Alt,false);
        require(sameBend(h.graph().links[0].bend,WireBend{}),"Alt-click did not reset the center handle");
        history(false);h.frame();require(std::abs(h.graph().links[0].bend.twist-0.4f)<0.001f,"Reset bend was not undoable");
    }
}
void testBendCancellationAndFineControl()
{
    gestureScenario="Bends cancel without saving or disturbing Undo and support fine control";
    for(int cancel=0;cancel<3;++cancel)
    {
        EditorHarness h;TemporaryWorkspace directory;prepareBendGraph(h,0.65f);
        h.graph().links[0].bend={0.4f,-0.2f};edited();h.frame();NodeEditor_Flush();
        std::ifstream beforeFile(GraphFile);const std::string saved((std::istreambuf_iterator<char>(beforeFile)),{});beforeFile.close();
        const auto before=h.graph().links[0].bend;const auto center=editor.canvasRoutes[0].routes[0].bendPoint;
        editor.redo.push_back(editor.draft);
        h.move(center);h.button(ImGuiMouseButton_Left,true);h.move(add(center,ImVec2(90,90)));
        require(!sameBend(h.graph().links[0].bend,before),"Bend fixture never changed its wire");
        NodeEditor_Flush();std::ifstream duringFile(GraphFile);const std::string during((std::istreambuf_iterator<char>(duringFile)),{});
        require(saved==during,"Autosave persisted an unfinished bend drag");duringFile.close();
        if(cancel==0){h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);}
        else if(cancel==1){h.button(ImGuiMouseButton_Right,true);h.button(ImGuiMouseButton_Right,false);}
        else NodeEditor_EndInteraction();
        h.button(ImGuiMouseButton_Left,false);h.frame();
        require(sameBend(h.graph().links[0].bend,before)&&editor.bendGraph<0&&editor.undo.empty()&&editor.redo.size()==1,
            "Cancelling a bend failed to restore its shape or disturbed history");
        require(!editor.rightGesture&&!editor.marquee&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId),"Cancelling a bend also opened a menu or selected nodes");
    }
    EditorHarness h;prepareBendGraph(h,1);const auto center=editor.canvasRoutes[0].routes[0].bendPoint;
    h.click(center);require(editor.undo.empty(),"Clicking a bend handle created a no-op history entry");
    h.key(ImGuiMod_Shift,true);h.drag(center,add(center,ImVec2(36,72)));h.key(ImGuiMod_Shift,false);
    require(std::abs(h.graph().links[0].bend.roundness-0.63f)<0.001f&&std::abs(h.graph().links[0].bend.twist-0.1f)<0.001f,"Shift-drag did not provide fine control");
    editor.undo.clear();const auto before=h.graph().links[0].bend;
    h.move(center);h.button(ImGuiMouseButton_Left,true);h.move(add(center,ImVec2(36,36)));h.releaseAt(center);
    require(sameBend(h.graph().links[0].bend,before)&&editor.undo.empty(),"Returning a bend to its start created a no-op edit");
}
void testManualBendsKeepTiesClipboardAndCache()
{
    gestureScenario="Manual bends preserve straps, portable copies and cached animation paths";
    EditorHarness h;prepareBendGraph(h,0.65f);auto& graph=h.graph();
    graph.ties={{1,480,160,30}};graph.links[0].ties={1};graph.links[0].bend={0.8f,-0.5f};edited();h.frame();
    auto& cache=editor.canvasRoutes[0];const auto before=cache.routes[0].path;
    const auto tie=graph.ties[0];const auto oldBuilds=cache.rebuilds;
    graph.links[0].bend.twist=0.7f;edited(false);h.frame();
    require(cache.rebuilds==oldBuilds+1&&cache.routes[0].path.size()==before.size(),"Manual bend did not invalidate cached geometry");
    bool changed=false;for(std::size_t i=0;i<before.size();++i)changed|=distanceSquared(before[i],cache.routes[0].path[i])>0.01f;
    require(changed&&graph.ties[0].x==tie.x&&graph.ties[0].y==tie.y&&graph.ties[0].angle==tie.angle,"Bending moved a strap or failed to change the tied curve");
    const auto lane=tieLane(graph,graph.links[0],graph.ties[0],h.origin);
    require(distanceSquared(routedPosition(cache.routes[0],cache.routes[0].tieDistances[0]/cache.routes[0].length),lane)<0.01f,"Bending detached the wire from its strap");
    const auto rebuilt=cache.rebuilds;h.move(ImVec2(5,5));for(int frame=0;frame<30;++frame)h.frame();
    require(cache.rebuilds==rebuilt,"An idle manual wire rebuilt its spline every frame");
    selectAll(graph,0);std::string text,error;require(selectionText(graph,0,text,error),error.c_str());
    require(pasteSelectionText(graph,0,text,ImVec2(128,384),error),error.c_str());h.frame();
    require(graph.links.size()==2&&sameBend(graph.links[0].bend,graph.links[1].bend)&&graph.links[0].ties!=graph.links[1].ties,
        "Copy/paste lost the manual shape or aliased its original tie");
    insertReroute(graph,0,ImVec2(384,128),0,0);h.frame();
    require(sameBend(graph.links[0].bend,graph.links.back().bend)&&graph.links[0].bend.twist==0.7f,"Adding a reroute discarded the manual bend");
}
void testBendHandlesRespectOtherControls()
{
    gestureScenario="Wire handles cannot steal node, group, port or strap gestures";
    for(const float zoom:GestureZooms)
    {
        EditorHarness h;prepareBendGraph(h,zoom);
        const auto center=editor.canvasRoutes[0].routes[0].localBendPoint;
        h.graph().nodes.push_back(defaultNode(NodeKind::Boolean,3,center.x-64,center.y-12));h.frame();
        const auto covered=screenPoint(h.graph(),h.origin,center.x,center.y);
        h.move(covered);h.button(ImGuiMouseButton_Left,true);
        require(editor.dragNode==3&&editor.bendGraph<0,"A handle stole an overlaid node's header");
        h.button(ImGuiMouseButton_Left,false);h.graph().nodes.pop_back();h.frame();
        h.graph().groups={{1,"Stage",center.x-64,center.y-12,256,256,0,{1}}};h.frame();
        h.move(covered);h.button(ImGuiMouseButton_Left,true);
        require(editor.dragGroup==1&&editor.bendGraph<0,"A handle stole a group header");
        h.button(ImGuiMouseButton_Left,false);h.graph().groups.clear();h.frame();
        h.move(h.port(1,true));h.button(ImGuiMouseButton_Left,true);
        require(editor.wireNode==1&&editor.bendGraph<0,"A handle swallowed a port gesture");
        h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);h.button(ImGuiMouseButton_Left,false);
        h.graph().ties={{1,center.x,center.y,0}};h.graph().links[0].ties={1};h.frame();
        const auto strap=screenPoint(h.graph(),h.origin,center.x,center.y);
        h.move(strap);h.button(ImGuiMouseButton_Left,true);
        require(editor.dragTie==1&&editor.bendGraph<0,"A bend handle stole a strap gesture");h.button(ImGuiMouseButton_Left,false);
    }
}
}
