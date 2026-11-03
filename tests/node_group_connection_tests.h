#pragma once
namespace
{
bool grouped(const Graph& graph,int group,int id)
{const auto* frame=findGroup(graph,group);return frame&&std::find(frame->nodes.begin(),frame->nodes.end(),id)!=frame->nodes.end();}
void groupMembershipFixture(EditorHarness& h,float zoom)
{
    h.graph().nodes={defaultNode(NodeKind::Boolean,1,64,96),defaultNode(NodeKind::Not,2,736,96)};
    h.graph().groups={{1,"Frame",32,32,320,256,0x40ffcd60u,{1}}};h.graph().links.clear();h.graph().zoom=zoom;edited(false);h.frame();
}
void testNodesLeaveAndEnterGroupsAcrossZoom()
{
    for(float zoom:GestureZooms)
    {
        gestureScenario="Group boundary membership at zoom "+std::to_string(zoom);TemporaryWorkspace files;EditorHarness h;groupMembershipFixture(h,zoom);
        const auto begin=h.body(1);h.fastDrag(begin,add(begin,ImVec2(352*zoom,0)));
        require(!grouped(h.graph(),1,1)&&findGroup(h.graph(),1)->x==32,"Moving a member out retained its group or moved the frame with it");
        const auto outside=findNode(h.graph(),1)->x;const auto header=canvasAt(h,120,46);h.fastDrag(header,add(header,ImVec2(32*zoom,0)));
        require(findNode(h.graph(),1)->x==outside,"Dragging the former group still moved its detached node");
        const auto back=h.body(1);h.fastDrag(back,add(back,ImVec2(-320*zoom,0)));
        require(grouped(h.graph(),1,1)&&findNode(h.graph(),1)->x==96,"Dropping a node inside a group did not attach it");
        history(false);h.frame();require(!grouped(h.graph(),1,1)&&findNode(h.graph(),1)->x==outside,"Undo did not restore movement and membership together");
        history(true);h.frame();require(grouped(h.graph(),1,1),"Redo did not restore group membership");
        std::string text,error;Workspace restored;require(serialize(service().workspace(),text,error)&&deserialize(text,restored,error)&&grouped(restored.graphs[0],1,1),"Group membership did not survive autosave/export");
    }
}
void testKeyboardNodeMoveDoesNotCarryItsFrame()
{
    gestureScenario="Keyboard node move leaves its frame in place";EditorHarness h;groupMembershipFixture(h,1);
    h.click(h.body(1));h.key(ImGuiKey_G,true);h.key(ImGuiKey_G,false);h.move(add(ImGui::GetIO().MousePos,ImVec2(32,0)));h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);
    require(findNode(h.graph(),1)->x==96&&findGroup(h.graph(),1)->x==32,"G implicitly moved a one-member group frame");
    h.key(ImGuiKey_G,true);h.key(ImGuiKey_G,false);h.move(add(ImGui::GetIO().MousePos,ImVec2(352,0)));h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);
    require(!grouped(h.graph(),1,1)&&findGroup(h.graph(),1)->x==32,"Keyboard movement retained membership outside the frame");
}
void testGroupBoundaryCancellationAndMultipleNodes()
{
    for(int cancel=0;cancel<2;++cancel)
    {
        gestureScenario="Cancelled group boundary drag "+std::to_string(cancel);EditorHarness h;groupMembershipFixture(h,1);
        const auto begin=h.body(1);h.move(begin);h.button(ImGuiMouseButton_Left,true);h.move(add(begin,ImVec2(352,0)));
        if(cancel==0){h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);}else{h.button(ImGuiMouseButton_Right,true);h.button(ImGuiMouseButton_Right,false);}
        h.button(ImGuiMouseButton_Left,false);
        require(grouped(h.graph(),1,1)&&findNode(h.graph(),1)->x==64&&editor.undo.empty(),"Cancelling a move changed membership, position or history");
    }
    gestureScenario="Multiple nodes detach independently";EditorHarness h;groupMembershipFixture(h,1);
    h.graph().nodes.push_back(defaultNode(NodeKind::Not,3,64,320));h.graph().groups[0].height=448;h.graph().groups[0].nodes.push_back(3);edited(false);h.frame();
    editor.selection[0]={1,3};editor.selected[0]=1;
    const auto begin=h.body(1);h.fastDrag(begin,add(begin,ImVec2(352,0)));
    require(!grouped(h.graph(),1,1)&&!grouped(h.graph(),1,3)&&findGroup(h.graph(),1)->x==32,"A multi-node drag retained its group bindings");
}
void testFrameAndNodeResizingUpdateMembership()
{
    gestureScenario="Resize changes membership without a permanent frame binding";EditorHarness h;groupMembershipFixture(h,1);selectOne(0,1);h.frame();
    const auto edge=canvasAt(h,254,176);h.fastDrag(edge,add(edge,ImVec2(128,0)));
    require(findNode(h.graph(),1)->width==320&&findGroup(h.graph(),1)->width==320&&!grouped(h.graph(),1,1)&&!grouped(service().workspace().graphs[0],1,1),"A resized node stretched its frame or retained membership beyond it");
    history(false);h.frame();require(grouped(h.graph(),1,1)&&findNode(h.graph(),1)->width==0,"Undo did not restore node dimensions and membership together");
    const auto corner=canvasAt(h,346,282);h.fastDrag(corner,add(corner,ImVec2(-160,-128)));
    require(findGroup(h.graph(),1)->width==160&&findGroup(h.graph(),1)->height==128&&!grouped(h.graph(),1,1),"A frame could not shrink past its old members or kept them bound afterwards");
    history(false);h.frame();require(grouped(h.graph(),1,1)&&findGroup(h.graph(),1)->width==320,"Undo did not restore the resized frame and its membership");
}
void testNestedGroupMovesAndDetachment()
{
    gestureScenario="Nested group movement and parent detachment";EditorHarness h;
    h.graph().nodes={defaultNode(NodeKind::Boolean,1,96,160)};h.graph().links.clear();
    h.graph().groups={{1,"Outer",32,32,416,384,0x40ffcd60u,{}},{2,"Inner",64,96,288,256,0x40ff93adu,{1}}};edited(false);h.frame();
    const auto outer=canvasAt(h,120,46);h.fastDrag(outer,add(outer,ImVec2(32,0)));
    require(findGroup(h.graph(),2)->x==96&&findNode(h.graph(),1)->x==128,"Dragging an outer frame left its nested frame behind");
    const auto inner=canvasAt(h,160,110);h.fastDrag(inner,add(inner,ImVec2(448,0)));
    require(!grouped(h.graph(),1,1)&&grouped(h.graph(),2,1),"Moving a nested group out retained its old parent binding");
    const auto x=findNode(h.graph(),1)->x,innerX=findGroup(h.graph(),2)->x;
    const auto again=canvasAt(h,152,46);h.fastDrag(again,add(again,ImVec2(32,0)));
    require(findNode(h.graph(),1)->x==x&&findGroup(h.graph(),2)->x==innerX,"The old parent still moved the detached child group");
}
void testMagneticWireTargetsAcrossZoomAndDirections()
{
    for(float zoom:GestureZooms)for(bool reverse:{false,true})
    {
        gestureScenario="Magnetic wire at zoom "+std::to_string(zoom)+(reverse?" reverse":" forward");EditorHarness h;h.graph().zoom=zoom;h.frame();
        const auto from=h.port(reverse?2:1,!reverse),to=h.port(reverse?1:2,reverse);
        const auto attractionPoint=add(to,ImVec2(reverse?16.0f:-16.0f,0));
        h.move(from);h.button(ImGuiMouseButton_Left,true);h.move(attractionPoint);
        require(editor.wireFeedback.target.node==(reverse?1:2)&&editor.wireFeedback.amount>0,"A compatible port did not capture the wire outside its tiny socket");
        const auto first=editor.wireFeedback.amount;const auto checks=editor.wireFeedback.checks;
        for(int frame=0;frame<20;++frame)h.frame();
        require(editor.wireFeedback.amount>first&&distanceSquared(editor.wireFeedback.endpoint,to)<0.1f,"Wire attraction did not animate towards the socket");
        require(editor.wireFeedback.checks==checks,"A stationary target revalidated/compiled the graph every frame");
        h.button(ImGuiMouseButton_Left,false);
        require(h.graph().links.size()==1&&h.graph().links[0].fromNode==1&&h.graph().links[0].toNode==2,"Dropping in the magnetic area connected the wrong ports");
        require(!editor.wireFeedback.target.node&&!editor.wireNode,"The magnetic target survived a completed gesture");
    }
}
void testMagneticTargetChoosesNearestPortAndRevalidates()
{
    gestureScenario="Nearest compatible socket and coalesced drop";EditorHarness h;arrangeCompactFixture(h,0.25f);
    const auto from=h.port(1,true),second=h.port(2,false,1);h.fastDrag(from,add(second,ImVec2(-16,0)));
    require(h.graph().links.size()==1&&h.graph().links[0].toPort==1,"Magnet chose the first socket instead of the closest, or lost a fast drop");
    gestureScenario="Magnetic cycles and cache invalidation";h.graph().nodes={defaultNode(NodeKind::Not,1,64,64),defaultNode(NodeKind::Not,2,416,64)};h.graph().links.clear();h.graph().zoom=1;edited();h.frame();
    h.move(h.port(1,true));h.button(ImGuiMouseButton_Left,true);h.move(add(h.port(2,false),ImVec2(-16,0)));
    require(editor.wireFeedback.valid,"A valid initial magnetic connection was rejected");const auto checked=editor.wireFeedback.checks;
    h.graph().links.push_back({2,0,1,0});edited();h.frame();
    require(!editor.wireFeedback.valid&&!editor.wireFeedback.target.node&&editor.wireFeedback.checks>checked,"A graph change reused a stale valid target or animated an invalid cycle");
    h.button(ImGuiMouseButton_Left,false);require(h.graph().links.size()==1,"An invalid magnetic cycle was connected");
}
void testMagneticWireCancellationAndTypeSafety()
{
    gestureScenario="Magnet cancellation and incompatible types";EditorHarness h;
    h.move(h.port(1,true));h.button(ImGuiMouseButton_Left,true);h.move(add(h.port(2,false),ImVec2(-16,0)));
    h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);h.button(ImGuiMouseButton_Left,false);
    require(!editor.wireNode&&!editor.wireFeedback.target.node&&h.graph().links.empty(),"Cancelling a magnetic wire retained a target or connection");
    h.move(h.port(3,true));h.button(ImGuiMouseButton_Left,true);h.move(add(h.port(2,false),ImVec2(-16,0)));
    require(!editor.wireFeedback.target.node,"A Float output magnetized to a Bool input");
    h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);h.button(ImGuiMouseButton_Left,false);
}
void testDistantGroupTitlesAreReadableAndCached()
{
    gestureScenario="Distant group titles remain readable without per-frame text layout";EditorHarness h;
    h.graph().nodes={defaultNode(NodeKind::Boolean,1,96,160)};h.graph().links.clear();
    h.graph().groups={{1,"01 Activation and input conditions",32,64,512,320,0x40ffcd60u,{1}}};h.graph().panX=h.graph().panY=128;edited(false);
    const auto revision=service().revision();
    for(float zoom:{0.25f,0.45f,0.64f})
    {
        h.graph().zoom=zoom;h.frame();require(editor.groupCaptions[0].size()==1,"A distant group has no overview title");
        const auto& cached=editor.groupCaptions[0][0];const auto& caption=cached.layout;
        require(caption.fontSize>=12&&caption.fontSize>ImGui::GetFontSize()*zoom&&cached.bounds.GetHeight()>28*zoom,"A group retained a tiny scaled header at overview zoom");
        require(caption.lines.size()<=2&&caption.extent.x<=caption.available.x+0.1f,"A group title is unbounded or overflows its label");
        const auto frameTop=screenPoint(h.graph(),h.origin,32,64).y;
        require(cached.bounds.Max.y<frameTop,"A large title covers the nodes inside its group");
        const auto layouts=caption.layouts;const auto previousX=cached.bounds.Min.x;
        for(int repeat=0;repeat<10;++repeat)h.frame();
        h.graph().panX+=32;h.frame();
        require(editor.groupCaptions[0][0].layout.layouts==layouts&&std::abs(editor.groupCaptions[0][0].bounds.Min.x-previousX-32)<0.01f,("Idle frames or panning repeatedly measured group text: old="+std::to_string(layouts)+" new="+std::to_string(editor.groupCaptions[0][0].layout.layouts)+" x="+std::to_string(previousX)+" -> "+std::to_string(editor.groupCaptions[0][0].bounds.Min.x)).c_str());
    }
    editor.viewMode=2;h.graph().zoom=1;h.frame();
    require(editor.groupCaptions[0][0].layout.fontSize>=12&&editor.groupCaptions[0][0].layout.zoomBucket==64,"Forced compact view did not apply to group titles");
    require(service().revision()==revision&&h.graph().nodes[0].x==96&&h.graph().groups[0].width==512,"Overview titles changed execution, node positions or group bounds");
}
void testLongGroupTitlesAndFitStayWithinBounds()
{
    gestureScenario="Long and Unicode group titles fit and update after edits";EditorHarness h;GroupCaption cached;
    Group group;group.id=1;group.width=256;group.title=u8"Long title with repeated sections and Unicode: \u041f\u0440\u0438\u0446\u0435\u043b\u0438\u0432\u0430\u043d\u0438\u0435 \u0438 \u043a\u0430\u043b\u0438\u0431\u0440\u043e\u0432\u043a\u0430";
    const auto& caption=groupOverviewCaption(cached,group,0.25f,ImGui::GetFont(),ImGui::GetFontSize());
    require(caption.lines.size()<=2&&caption.extent.x<=caption.available.x+0.1f&&caption.extent.y<=caption.available.y+0.1f&&cached.source==group.title,"A long group title spills out or loses its full source name");
    for(const char* cursor=caption.text.c_str();*cursor;)
    {unsigned int codepoint=0;const int bytes=ImTextCharFromUtf8(&codepoint,cursor,nullptr);require(bytes>0&&codepoint!=0xfffd,"A shortened group title splits a UTF-8 character");cursor+=bytes;}
    const auto layouts=caption.layouts;group.title="Recoil";group.width=512;groupOverviewCaption(cached,group,0.25f,ImGui::GetFont(),ImGui::GetFontSize());
    require(cached.layout.layouts==layouts+1&&cached.layout.text=="RECOIL","Editing a group title or size retained a stale caption");
    h.graph().nodes={defaultNode(NodeKind::Boolean,1,96,160)};h.graph().links.clear();h.graph().groups={{1,"Activation and input conditions",32,64,512,320,0x40ffcd60u,{1}}};
    const auto before=h.graph().nodes[0];editor.viewMode=2;fitGraph(h.graph(),editor.canvasSize[0]);h.frame();
    const auto& bounds=editor.groupCaptions[0][0].bounds;
    require(bounds.Min.y>=h.origin.y&&bounds.Max.y<=h.origin.y+editor.canvasSize[0].y,"Frame graph clips the enlarged group title above the canvas");
    require(h.graph().nodes[0].x==before.x&&h.graph().nodes[0].y==before.y,"Framing overview titles rearranged nodes");
}
void testOverviewGroupTitleIsADragHandle()
{
    for(float zoom:{0.25f,0.45f,0.64f})
    {
        gestureScenario="Overview group title drag at zoom "+std::to_string(zoom);EditorHarness h;
        h.graph().nodes={defaultNode(NodeKind::Boolean,1,96,160)};h.graph().links.clear();
        h.graph().groups={{1,"Activation",32,64,512,320,0x40ffcd60u,{1}}};h.graph().zoom=zoom;h.graph().panX=h.graph().panY=128;edited(false);h.frame();
        const auto label=editor.groupCaptions[0][0].bounds.GetCenter();
        require(label.y<screenPoint(h.graph(),h.origin,32,64).y,"The overview title is not outside the small header");
        h.fastDrag(label,add(label,ImVec2(64*zoom,32*zoom)));
        require(findGroup(h.graph(),1)->x==96&&findGroup(h.graph(),1)->y==96&&findNode(h.graph(),1)->x==160&&findNode(h.graph(),1)->y==192,"Dragging the large title panned the graph or failed to carry its contents");
        require(!editor.wireNode&&!editor.resizeGroup&&editor.panButton<0,"Title drag collided with another mouse tool");
    }
}

}
