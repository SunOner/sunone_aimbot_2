namespace
{
void doubleClickCanvas(EditorHarness& h,ImVec2 point)
{ImGui::GetIO().MouseDoubleClickTime=0.4f;h.click(point);h.click(point);h.frame();}
void testEmptyDoubleClickCreatesOneRerouteAcrossZoomAndTabs()
{
    gestureScenario="Double-click empty canvas creates a selected, snapped and undoable reroute";
    for(int tab=0;tab<static_cast<int>(GraphCount);++tab)for(float zoom:GestureZooms)
    {
        EditorHarness h;h.graphIndex=tab;h.graph().nodes.clear();h.graph().links.clear();h.graph().groups.clear();h.graph().ties.clear();
        h.graph().zoom=zoom;h.graph().panX=13;h.graph().panY=-7;edited();h.frame();
        const auto point=canvasAt(h,288,288);const auto pan=ImVec2(h.graph().panX,h.graph().panY);
        doubleClickCanvas(h,point);
        require(h.graph().nodes.size()==1&&reroute(h.graph().nodes[0].kind)&&isSelected(tab,h.graph().nodes[0].id),"Empty double-click did not create and select exactly one reroute");
        const auto& node=h.graph().nodes[0];
        require(node.x==snap(node.x)&&node.y==snap(node.y)&&distanceSquared(rerouteCenter(h.graph(),h.origin,node),point)<std::pow(32*zoom+2,2),"Reroute placement ignored the cursor or grid");
        require(editor.undo.size()==1&&editor.panButton<0&&!editor.pointerMove&&!editor.wireNode&&h.graph().panX==pan.x&&h.graph().panY==pan.y,"Double-click also panned, connected or moved a node");
        history(false);h.frame();require(h.graph().nodes.empty(),"Undo did not remove the new reroute");history(true);h.frame();
        require(h.graph().nodes.size()==1&&reroute(h.graph().nodes[0].kind),"Redo did not restore the reroute");
    }
}
void testEmptyDoubleClickRespectsGroupsToolsAndLimits()
{
    gestureScenario="Empty double-click respects group ownership, modifiers, panning and node limits";
    {
        EditorHarness h;h.graph().groups={{1,"Stage",32,32,704,576,0x40ffcd60u,{1,2}}};edited();h.frame();
        doubleClickCanvas(h,canvasAt(h,640,480));
        require(h.graph().nodes.size()==4&&std::find(h.graph().groups[0].nodes.begin(),h.graph().groups[0].nodes.end(),editor.selected[0])!=h.graph().groups[0].nodes.end(),"A reroute created inside a group did not move with its frame");
    }
    for(auto modifier:{ImGuiMod_Ctrl,ImGuiMod_Shift,ImGuiMod_Alt})
    {
        EditorHarness h;const auto count=h.graph().nodes.size();h.key(modifier,true);doubleClickCanvas(h,canvasAt(h,736,448));h.key(modifier,false);
        require(h.graph().nodes.size()==count,"A modified double-click created an unintended reroute");
    }
    {
        EditorHarness h;const auto count=h.graph().nodes.size();const auto start=canvasAt(h,736,448);ImGui::GetIO().MouseDoubleClickTime=0.4f;
        h.move(start);h.button(ImGuiMouseButton_Left,true);h.move(add(start,ImVec2(30,20)));h.releaseAt(start);h.click(start);
        require(h.graph().nodes.size()==count,"A completed pan was mistaken for the first half of a double-click");
    }
    {
        EditorHarness h;h.graph().nodes.clear();for(int i=0;i<static_cast<int>(MaxNodes);++i)h.graph().nodes.push_back(defaultNode(NodeKind::Float,i+1,20000,i*192.0f));edited();h.frame();
        doubleClickCanvas(h,canvasAt(h,640,448));require(h.graph().nodes.size()==MaxNodes&&editor.undo.empty()&&editor.error,"Double-click exceeded the node limit or created an empty Undo entry");
    }
}
void testLooseReroutesAdaptToTheFirstConnection()
{
    gestureScenario="Loose reroutes adopt their first wire's type and keep connected types stable";
    const NodeKind sources[]={NodeKind::Boolean,NodeKind::Float,NodeKind::Integer,NodeKind::MakePoint,NodeKind::Detections,NodeKind::Target,
        NodeKind::MouseDelta,NodeKind::CaptureSource,NodeKind::DepthInference,NodeKind::DepthToMask,NodeKind::String,NodeKind::PrimaryMonitor,
        NodeKind::GetMonitors,NodeKind::OverlayBoxes,NodeKind::DatasetLabels};
    for(auto kind:sources)
    {
        EditorHarness h;h.graphIndex=3;h.graph().nodes={defaultNode(kind,1,64,64),defaultNode(NodeKind::RerouteFloat,2,384,160)};h.graph().links.clear();edited();h.frame();
        const auto type=definition(kind).outputs[0].type;const Link link{1,0,2,0};std::string error;
        require(canConnectReroutes(h.graph(),link,error),"A loose reroute rejected a supported wire type");
        require(findNode(h.graph(),2)->kind==NodeKind::RerouteFloat,"Hover validation mutated an unconnected point");
        h.drag(h.port(1,true),rerouteCenter(h.graph(),h.origin,*findNode(h.graph(),2)));h.frame();
        require(h.graph().links.size()==1&&definition(findNode(h.graph(),2)->kind).outputs[0].type==type,"Connecting a loose point did not apply its inferred type");
        require(editor.undo.size()==1,"Type adaptation used a second Undo entry");history(false);h.frame();
        require(h.graph().links.empty()&&findNode(h.graph(),2)->kind==NodeKind::RerouteFloat,"Undo did not restore the original point and wire together");
    }
    {
        EditorHarness h;h.graph().nodes={defaultNode(NodeKind::Not,1,416,64),defaultNode(NodeKind::RerouteFloat,2,64,160)};h.graph().links.clear();edited();h.frame();
        h.drag(h.port(2,true),h.port(1,false));h.frame();require(findNode(h.graph(),2)->kind==NodeKind::RerouteBoolean&&h.graph().links.size()==1,"Dragging from a loose output failed to infer the destination type");
        h.graph().nodes.push_back(defaultNode(NodeKind::Float,3,64,320));h.frame();std::string error;
        require(!canConnectReroutes(h.graph(),Link{3,0,2,0},error)&&findNode(h.graph(),2)->kind==NodeKind::RerouteBoolean,"A connected reroute silently changed its established type");
    }
    {
        EditorHarness h;h.graph().nodes[1]=defaultNode(NodeKind::RerouteFloat,2,416,64);edited();h.frame();
        focusPort(h.graph(),0,{1,0,true});activatePort(h.graph(),0);focusPort(h.graph(),0,{2,0,false});activatePort(h.graph(),0);h.frame();
        require(h.graph().links.size()==1&&findNode(h.graph(),2)->kind==NodeKind::RerouteBoolean,"Keyboard wiring could not adapt a loose point");
    }
}
void testMouseMenuHoverDoesNotHighlightAnUnrelatedFirstItem()
{
    gestureScenario="Mouse category transitions and keyboard navigation own separate menu highlights";
    EditorHarness h;h.click(add(h.origin,ImVec2(750,430)),ImGuiMouseButton_Right);h.frame();h.frame();
    auto& context=*ImGui::GetCurrentContext();require(!context.OpenPopupStack.empty()&&context.OpenPopupStack[0].Window,"Context menu did not open");const auto* root=context.OpenPopupStack[0].Window;
    const float firstRow=root->DC.CursorStartPos.y+ImGui::GetTextLineHeightWithSpacing()+ImGui::GetFrameHeightWithSpacing()+1+ImGui::GetStyle().ItemSpacing.y+ImGui::GetTextLineHeight()*0.5f;
    h.move(ImVec2(root->Pos.x+70,firstRow));for(int frame=0;frame<20;++frame)h.frame();
    require(context.OpenPopupStack.Size>=2&&context.OpenPopupStack[1].Window,"Category menu did not open");
    auto* categories=context.OpenPopupStack[1].Window;const auto tree=catalogTree(editor.catalogSearch.available);
    const auto categoryPoint=[&](int row){return add(categories->DC.CursorStartPos,ImVec2(55,row*ImGui::GetTextLineHeightWithSpacing()+ImGui::GetTextLineHeight()*0.5f));};
    for(int row:{0,3,1,4,2,0})
    {
        require(row<static_cast<int>(tree.children.size()),"Category fixture no longer has enough branches");h.move(categoryPoint(row));
        for(int frame=0;frame<20;++frame)
        {
            h.frame();require(!context.NavCursorVisible&&!context.NavHighlightItemUnderNav&&!editor.popupUsesKeyboard,"Mouse-driven menus highlighted a keyboard-selected item");
        }
        require(context.OpenPopupStack.Size>=3&&context.OpenPopupStack[2].PopupId==categories->GetID(tree.children[row].label.c_str()),"Moving between categories opened the wrong submenu");
    }
    h.key(ImGuiKey_DownArrow,true);h.key(ImGuiKey_DownArrow,false);h.frame();
    require(editor.popupUsesKeyboard&&context.NavCursorVisible,"Suppressing mouse artifacts also disabled keyboard navigation");
    h.move(categoryPoint(2));for(int frame=0;frame<20;++frame)h.frame();
    require(!editor.popupUsesKeyboard&&!context.NavCursorVisible&&!context.NavHighlightItemUnderNav,"Mouse input did not take menu highlighting back from the keyboard");
}
}
