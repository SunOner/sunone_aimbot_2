namespace
{
bool onLayoutGrid(float value){return std::abs(value-snap(value))<0.001f;}
bool hasSnapPreview(const EditorHarness& h)
{return std::any_of(h.vertices.begin(),h.vertices.end(),[](const ImDrawVert& vertex){return vertex.col==SnapPreviewFill;});}
void testEveryCardSizeFollowsTheGrid()
{
    gestureScenario="Automatic, manual and collapsed card bounds all use the visible grid";
    for(int kind=0;kind<static_cast<int>(NodeKind::Count);++kind)
    {
        auto node=defaultNode(static_cast<NodeKind>(kind),1);
        for(int size=0;size<3;++size)
        {
            if(size){node.width=size==1?197:611;node.height=Layout::minimumHeight(node)+(size==1?3:53);}
            require(onLayoutGrid(cardWidth(node))&&onLayoutGrid(cardHeight(node)),"A card's dimensions do not align with grid cells");
            require(cardHeight(node)>=Layout::minimumHeight(node),"Grid rounding clipped a node's required content");
            node.collapsed=true;require(onLayoutGrid(cardHeight(node)),"A collapsed card's bottom edge ignores the grid");node.collapsed=false;
        }
    }
    EditorHarness h;h.graph().nodes[0].x=67;h.graph().nodes[0].y=77;h.graph().nodes[0].width=197;h.graph().nodes[0].height=153;
    snapLayout(h.graph());edited();h.frame();const auto& node=h.graph().nodes[0];
    require(onLayoutGrid(node.x)&&onLayoutGrid(node.y)&&onLayoutGrid(node.width)&&onLayoutGrid(node.height),"Imported manual dimensions were not normalized to the grid");
}
void testFreeDragShowsItsGridDestinationAcrossZoom()
{
    gestureScenario="Nodes follow small pointer movements freely and snap only on release";
    for(const float zoom:GestureZooms)
    {
        EditorHarness h;arrangeCompactFixture(h,zoom);h.graph().links={{1,0,2,0}};edited();h.frame();
        selectOne(0,1);editor.selection[0].push_back(2);const auto revision=service().revision();
        const auto from=h.body(1);h.move(from);h.button(ImGuiMouseButton_Left,true);h.move(add(from,ImVec2(2,1)));
        require(std::abs(findNode(h.graph(),1)->x-64-2/zoom)<0.001f&&std::abs(findNode(h.graph(),1)->y-64-1/zoom)<0.001f,
            "The node sticks to its original grid cell or a mouse drag threshold");
        require(editor.pointerMove&&editor.undo.empty()&&hasSnapPreview(h),"Free dragging has no destination highlight or consumed Undo before release");
        require(distanceSquared(editor.canvasRoutes[0].routes[0].path.front(),h.port(1,true))<0.001f,"A moved node's wire lags behind its current position");
        h.move(add(from,ImVec2(43*zoom,47*zoom)));
        const float freeDx=(ImGui::GetIO().MousePos.x-editor.dragStartMouse.x)/zoom;
        require(std::abs(findNode(h.graph(),1)->x-64-freeDx)<0.001f&&std::abs(findNode(h.graph(),2)->x-320-freeDx)<0.001f,"Multi-selection does not move freely by one shared offset");
        const auto expected=screenPoint(h.graph(),h.origin,96,96);bool matching=false;
        for(const auto& vertex:h.vertices)if(vertex.col==SnapPreviewFill&&vertex.pos.x>=expected.x-0.1f&&vertex.pos.x<expected.x+8*zoom&&vertex.pos.y>=expected.y-0.1f&&vertex.pos.y<expected.y+8*zoom)matching=true;
        require(matching,"The background preview does not mark the actual snapped destination");
        h.button(ImGuiMouseButton_Left,false);
        require(findNode(h.graph(),1)->x==96&&findNode(h.graph(),1)->y==96&&findNode(h.graph(),2)->x==352,"Release did not place every selected node on the grid");
        require(!editor.pointerMove&&!hasSnapPreview(h)&&editor.undo.size()==1&&service().revision()==revision,"Move completion left its preview visible, restarted execution or failed one-step Undo");
        history(false);h.frame();require(findNode(h.graph(),1)->x==64&&findNode(h.graph(),2)->x==320,"Undo lost the original placement");
        history(true);h.frame();require(findNode(h.graph(),1)->x==96&&findNode(h.graph(),2)->x==352,"Redo lost the snapped placement");
    }
}
void testFreeMoveCancelsWithoutSavingOrNoopHistory()
{
    gestureScenario="Free movement remains a cancellable unsaved preview until release";
    for(int cancel=0;cancel<3;++cancel)
    {
        EditorHarness h;TemporaryWorkspace directory;arrangeCompactFixture(h,0.65f);edited();NodeEditor_Flush();
        std::ifstream beforeFile(GraphFile);const std::string saved((std::istreambuf_iterator<char>(beforeFile)),{});beforeFile.close();
        editor.redo.push_back(editor.draft);const auto from=h.body(1);
        h.move(from);h.button(ImGuiMouseButton_Left,true);h.move(add(from,ImVec2(27,19)));NodeEditor_Flush();
        std::ifstream duringFile(GraphFile);const std::string during((std::istreambuf_iterator<char>(duringFile)),{});duringFile.close();
        require(saved==during&&editor.undo.empty(),"Autosave or Undo committed an unfinished off-grid move");
        if(cancel==0){h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);}
        else if(cancel==1){h.button(ImGuiMouseButton_Right,true);h.button(ImGuiMouseButton_Right,false);}
        else NodeEditor_EndInteraction();
        h.button(ImGuiMouseButton_Left,false);h.frame();
        require(findNode(h.graph(),1)->x==64&&findNode(h.graph(),1)->y==64&&!editor.pointerMove&&!hasSnapPreview(h)&&editor.undo.empty()&&editor.redo.size()==1,"Cancellation did not restore placement and history");
        require(!editor.rightGesture&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId),"Cancelling also opened a menu or started selection");
    }
    EditorHarness h;const auto from=h.body(1);h.drag(from,add(from,ImVec2(2,1)));
    require(findNode(h.graph(),1)->x==64&&findNode(h.graph(),1)->y==64&&editor.undo.empty(),"A sub-cell move created an empty Undo entry");
    h.move(from);h.button(ImGuiMouseButton_Left,true);h.move(add(from,ImVec2(71,51)));h.releaseAt(from);
    require(findNode(h.graph(),1)->x==64&&findNode(h.graph(),1)->y==64&&editor.undo.empty(),"Returning to the start created an empty edit");
}
void testGroupsAndReroutesHaveFreePlacementPreviews()
{
    gestureScenario="Groups, reroutes and their straps use the same free placement preview";
    for(const float zoom:GestureZooms)
    {
        EditorHarness h;arrangeCompactFixture(h,zoom);h.graph().groups={{1,"Stage",32,32,512,320,0x40ffcd60u,{1,2}}};
        h.graph().ties={{1,288,128,0}};h.graph().links={{1,0,2,0,{1}}};edited();h.frame();
        const auto from=canvasAt(h,92,46);h.move(from);h.button(ImGuiMouseButton_Left,true);h.move(add(from,ImVec2(21*zoom,11*zoom)));
        const float dx=(ImGui::GetIO().MousePos.x-editor.dragStartMouse.x)/zoom;
        require(std::abs(h.graph().groups[0].x-32-dx)<0.001f&&std::abs(findNode(h.graph(),1)->x-64-dx)<0.001f&&std::abs(h.graph().ties[0].x-288-dx)<0.001f&&hasSnapPreview(h),"A group or its strap still snaps while held");
        h.button(ImGuiMouseButton_Left,false);
        require(h.graph().groups[0].x==64&&h.graph().groups[0].y==32&&findNode(h.graph(),1)->x==96&&h.graph().ties[0].x==320,"Group completion did not snap contents consistently");
        history(false);h.graph().groups.clear();h.graph().ties.clear();h.graph().links.clear();
        h.graph().nodes={defaultNode(NodeKind::RerouteBoolean,1,224,144)};h.frame();
        const auto dot=rerouteCenter(h.graph(),h.origin,h.graph().nodes[0]);h.move(dot);h.button(ImGuiMouseButton_Left,true);h.move(add(dot,ImVec2(21*zoom,11*zoom)));
        const float rerouteDx=(ImGui::GetIO().MousePos.x-editor.dragStartMouse.x)/zoom;
        require(std::abs(h.graph().nodes[0].x-224-rerouteDx)<0.001f&&hasSnapPreview(h),"Reroute points lack free movement or a snap preview");
        h.button(ImGuiMouseButton_Left,false);require(h.graph().nodes[0].x==256&&h.graph().nodes[0].y==144,"Reroute completion ignored the grid");
    }
}
void testReroutesAndTiesShareGridIntersections()
{
    for(float zoom:GestureZooms)for(ImVec2 position:{ImVec2(291,273),ImVec2(-17,-49),ImVec2(16,16)})
    {
        gestureScenario="Reroute/tie creation at zoom "+std::to_string(zoom);EditorHarness h;arrangeTieFixture(h,zoom);
        h.graph().panX=13;h.graph().panY=-7;
        std::vector<TieHit> hits;for(const auto& link:h.graph().links)hits.push_back({linkKey(link),0,{}});
        std::string error;require(createWireTie(h.graph(),0,hits,position,15,error),error.c_str());
        const auto tie=h.graph().ties[0];
        const auto check=[&](const Node& node)
        {
            require(node.x+32==tie.x&&node.y+16==tie.y&&onLayoutGrid(tie.x)&&onLayoutGrid(tie.y),
                "Reroute and tie created at the same position use different visible grid anchors");
            require(distanceSquared(rerouteCenter(h.graph(),h.origin,node),screenPoint(h.graph(),h.origin,tie.x,tie.y))<0.001f,
                "Pan or zoom shifted the reroute away from the tie's grid intersection");
        };
        addLooseReroute(h.graph(),0,position);check(*findNode(h.graph(),editor.selected[0]));
        insertReroute(h.graph(),0,position,0);check(*findNode(h.graph(),editor.selected[0]));
        editor.catalogPosition=position;require(addCatalogNode(h.graph(),0,NodeKind::RerouteInteger,error),error.c_str());
        check(*findNode(h.graph(),editor.selected[0]));
    }
}
void testMixedPointSelectionAndPreviewUseCenterSnap()
{
    for(float zoom:GestureZooms)
    {
        gestureScenario="Mixed reroute/tie movement and preview at zoom "+std::to_string(zoom);EditorHarness h;arrangeTieFixture(h,zoom);
        std::vector<TieHit> hits;for(const auto& link:h.graph().links)hits.push_back({linkKey(link),0,{}});
        std::string error;require(createWireTie(h.graph(),0,hits,ImVec2(320,192),0,error),error.c_str());
        h.graph().nodes.push_back(defaultNode(NodeKind::RerouteBoolean,7,416,304));edited();h.frame();
        selectOne(0,7);editor.tieSelection[0]={h.graph().ties[0].id};editor.selectedTie[0]=h.graph().ties[0].id;
        const auto from=rerouteCenter(h.graph(),h.origin,*findNode(h.graph(),7));
        h.move(from);h.button(ImGuiMouseButton_Left,true);
        require(editor.dragNode==7&&!editor.wireNode,"The point fixture started a wire instead of a move");
        h.move(add(from,ImVec2(21*zoom,11*zoom)));
        const auto freeCenter=rerouteCenter(h.graph(),h.origin,*findNode(h.graph(),7));
        const ImVec2 delta(ImGui::GetIO().MousePos.x-editor.dragStartMouse.x,ImGui::GetIO().MousePos.y-editor.dragStartMouse.y);
        require(distanceSquared(freeCenter,add(from,delta))<0.01f,"Center snapping introduced a magnet during free dragging");
        const auto destination=screenPoint(h.graph(),h.origin,480,320);
        ImVec2 minimum(1e9f,1e9f),maximum(-1e9f,-1e9f);int vertices=0;
        for(const auto& vertex:h.vertices)if(vertex.col==SnapPreviewFill&&distanceSquared(vertex.pos,destination)<81)
        {minimum.x=std::min(minimum.x,vertex.pos.x);minimum.y=std::min(minimum.y,vertex.pos.y);maximum.x=std::max(maximum.x,vertex.pos.x);maximum.y=std::max(maximum.y,vertex.pos.y);++vertices;}
        require(vertices>=8&&distanceSquared(multiply(add(minimum,maximum),0.5f),destination)<0.01f,"Reroute preview is half a grid cell away from its release position");
        h.button(ImGuiMouseButton_Left,false);
        const auto& node=*findNode(h.graph(),7);const auto tie=h.graph().ties[0];
        require(node.x+32==480&&node.y+16==320&&tie.x==352&&tie.y==192,"Mixed point selection did not snap both centers by the same offset");
        require(editor.undo.size()==2,"Mixed point movement did not create one Undo step");
        history(false);h.frame();require(findNode(h.graph(),7)->y+16==320&&h.graph().ties[0].x==320,"Undo changed the original point alignment");
        history(true);h.frame();require(findNode(h.graph(),7)->x+32==480&&h.graph().ties[0].x==352,"Redo lost the shared point placement");
        selectOne(0,7);editor.tieSelection[0]={tie.id};editor.selectedTie[0]=tie.id;
        h.key(ImGuiKey_RightArrow,true);h.key(ImGuiKey_RightArrow,false);
        require(findNode(h.graph(),7)->x+32==512&&findNode(h.graph(),7)->y+16==320&&h.graph().ties[0].x==384&&h.graph().ties[0].y==192,
            "Keyboard nudging used a different grid for reroutes and ties");
    }
}
void testPointGridSurvivesCopiesArrangeAndLegacyNormalization()
{
    gestureScenario="Point-grid normalization, group membership, copies and persistence";EditorHarness h;arrangeTieFixture(h,1);
    std::vector<TieHit> hits;for(const auto& link:h.graph().links)hits.push_back({linkKey(link),0,{}});
    std::string error;require(createWireTie(h.graph(),0,hits,ImVec2(320,192),30,error),error.c_str());
    h.graph().nodes.push_back(defaultNode(NodeKind::RerouteBoolean,7,288,416));
    h.graph().groups={{1,"Legacy point",256,352,128,96,0x40ffcd60u,{7}}};
    const auto links=h.graph().links;const auto revision=service().revision();
    require(snapLayout(h.graph())&&findNode(h.graph(),7)->y+16==448,"An old reroute kept its half-cell offset after normalization");
    syncGroupMembership(h.graph());
    require(h.graph().groups[0].nodes==std::vector<int>{7}&&!snapLayout(h.graph()),"Normalizing an old point detached it from its frame or keeps shifting it on reload");
    require(h.graph().links.size()==links.size()&&h.graph().ties[0].angle==30&&service().revision()==revision,"Snapping point geometry changed execution or tie routing");
    selectAll(h.graph(),0);std::string clipboard;require(selectionText(h.graph(),0,clipboard,error),error.c_str());
    require(pasteSelectionText(h.graph(),0,clipboard,ImVec2(32,512),error),error.c_str());
    duplicateSelection(h.graph(),0);
    const auto check=[&](const Graph& graph)
    {
        for(const auto& node:graph.nodes)if(reroute(node.kind))require(onLayoutGrid(node.x+32)&&onLayoutGrid(node.y+16),"A copied, imported or arranged reroute center ignores the grid");
        for(const auto& tie:graph.ties)require(onLayoutGrid(tie.x)&&onLayoutGrid(tie.y),"A copied, imported or arranged tie center ignores the grid");
    };
    check(h.graph());std::string document;Workspace restored;
    require(serialize(editor.draft,document,error)&&deserialize(document,restored,error),error.c_str());
    require(!snapLayout(restored.graphs[0]),"A saved point layout changes when imported again");check(restored.graphs[0]);
    Layout::arrange(restored.graphs[0]);check(restored.graphs[0]);require(!Layout::arrange(restored.graphs[0]),"Arranging point nodes is not stable");
    auto& point=*findNode(h.graph(),7);point.y=800;const auto frame=h.graph().groups[0];
    snapLayout(h.graph());syncGroupMembership(h.graph());
    require(h.graph().groups[0].nodes.empty()&&h.graph().groups[0].y==frame.y&&h.graph().groups[0].height==frame.height,
        "Normalizing an explicitly detached reroute expanded its old group to keep it attached");
}
}
