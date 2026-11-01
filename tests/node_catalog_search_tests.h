namespace
{
ImGuiWindow* catalogRoot()
{
    auto& stack=ImGui::GetCurrentContext()->OpenPopupStack;
    require(!stack.empty()&&stack[0].Window,"Node library popup did not open");return stack[0].Window;
}
void typeCatalogQuery(EditorHarness& h,const char* query)
{ImGui::GetIO().AddInputCharactersUTF8(query);h.frame();}
ImGuiWindow* catalogListWindow()
{
    for(auto* window:ImGui::GetCurrentContext()->Windows)
        if(window->Active&&std::strstr(window->Name,"catalog_results"))return window;
    return nullptr;
}
void testCatalogSearchAddsWithTabAndEnter()
{
    gestureScenario="Open, type immediately and confirm a searched node without refocusing";
    for(int opener=0;opener<3;++opener)for(auto accept:{ImGuiKey_Tab,ImGuiKey_Enter,ImGuiKey_KeypadEnter})
    {
        EditorHarness h;h.graphIndex=3;h.frame();const auto at=canvasAt(h,640,448);h.move(at);
        if(opener==0)h.click(at,ImGuiMouseButton_Right);
        else chord(h,opener==1?ImGuiMod_Shift:ImGuiMod_Ctrl,opener==1?ImGuiKey_A:ImGuiKey_Space);
        auto* root=catalogRoot();typeCatalogQuery(h,"nMs");
        require(std::strcmp(editor.search,"nMs")==0&&ImGui::GetActiveID()==root->GetID("##search"),
            ("Opening the library lost initial characters or required a search-field click; opener="+std::to_string(opener)+" query="+editor.search+" active="+std::to_string(ImGui::GetActiveID())+" search="+std::to_string(root->GetID("##search"))).c_str());
        require(!editor.catalogSearch.nodes.empty()&&editor.catalogSearch.nodes[0]==NodeKind::FilterNms,"An exact short name was not the first match");
        const auto position=editor.catalogPosition;h.key(accept,true);h.key(accept,false);h.frame();
        require(h.graph().nodes.size()==1&&h.graph().nodes[0].kind==NodeKind::FilterNms&&isSelected(3,h.graph().nodes[0].id),"Tab/Enter did not add the selected search result exactly once");
        require(h.graph().nodes[0].x==snap(position.x)&&h.graph().nodes[0].y==snap(position.y),"Search confirmation placed the node away from the requested location");
        require(ImGui::GetCurrentContext()->OpenPopupStack.empty()&&editor.undo.size()==1&&!editor.keyboardMove&&!editor.keyboardWire&&!ImGui::IsAnyItemActive(),"Search confirmation left a popup, active input or another gesture behind");
        require((ImGui::GetIO().ConfigFlags&ImGuiConfigFlags_NavEnableKeyboard)==0,"The search popup leaked keyboard navigation into the graph");
        history(false);h.frame();require(h.graph().nodes.empty(),"Undo did not remove the searched node");
        history(true);h.frame();require(h.graph().nodes.size()==1,"Redo did not restore the searched node");
    }
}
void testCatalogArrowsKeepSearchFocusAndScroll()
{
    gestureScenario="Result arrows wrap, scroll and preserve text-editing focus";
    EditorHarness h;selectOne(0,1);h.click(canvasAt(h,736,448),ImGuiMouseButton_Right);typeCatalogQuery(h,"target");h.frame();
    auto* root=catalogRoot();const auto searchId=root->GetID("##search");const auto count=editor.catalogSearch.nodes.size(),builds=editor.catalogSearch.builds;
    require(count>8&&std::strncmp(definition(editor.catalogSearch.nodes[0]).title,"Target",6)==0,"Search fixture lacks ranked scrollable results");
    h.key(ImGuiKey_DownArrow,true);h.key(ImGuiKey_DownArrow,false);
    require(editor.catalogSearch.selected==1&&ImGui::GetActiveID()==searchId,"Down changed focus instead of selecting the next result");
    h.key(ImGuiKey_UpArrow,true);h.key(ImGuiKey_UpArrow,false);h.key(ImGuiKey_UpArrow,true);h.key(ImGuiKey_UpArrow,false);h.frame();
    auto* list=catalogListWindow();
    require(editor.catalogSearch.selected==static_cast<int>(count)-1&&list&&list->Scroll.y>0,"Up did not wrap to the last result and reveal it");
    h.key(ImGuiKey_DownArrow,true);h.key(ImGuiKey_DownArrow,false);h.frame();
    require(editor.catalogSearch.selected==0&&list->Scroll.y<1,"Returning to the first result left it outside the visible list");
    for(int i=0;i<30;++i)h.frame();require(editor.catalogSearch.builds==builds,"Idle frames or selection arrows rebuilt filtered compatibility results");
    h.key(ImGuiKey_G,true);typeCatalogQuery(h,"g");h.key(ImGuiKey_G,false);h.key(ImGuiKey_X,true);typeCatalogQuery(h,"x");h.key(ImGuiKey_X,false);
    require(std::strcmp(editor.search,"targetgx")==0&&ImGui::GetActiveID()==searchId,"Typing after arrow navigation no longer edited the query");
    require(editor.catalogSearch.candidateBuilds==1,"Editing the query repeatedly recompiled compatible node candidates");
    require(h.graph().nodes.size()==3&&findNode(h.graph(),1)->x==64&&!editor.keyboardMove&&editor.undo.empty(),"Graph movement or deletion shortcuts consumed search typing");
}
void testCatalogNoResultsAndEscapeAreSafe()
{
    gestureScenario="No-match confirmation keeps search usable and Escape cancels it";
    EditorHarness h;h.click(canvasAt(h,736,448),ImGuiMouseButton_Right);typeCatalogQuery(h,"not_a_real_node_9817");
    auto* root=catalogRoot();const auto input=root->GetID("##search");
    for(auto key:{ImGuiKey_DownArrow,ImGuiKey_UpArrow,ImGuiKey_Tab,ImGuiKey_Enter,ImGuiKey_KeypadEnter})
    {
        h.key(key,true);h.key(key,false);h.frame();
        require(!ImGui::GetCurrentContext()->OpenPopupStack.empty()&&editor.catalogSearch.nodes.empty()&&h.graph().nodes.size()==3&&editor.undo.empty(),"Confirming an empty search created a node or closed the library");
        require(ImGui::GetActiveID()==input,"An unsuccessful confirmation lost the search field's focus");
    }
    h.key(ImGuiKey_Enter,true);typeCatalogQuery(h,"z");h.key(ImGuiKey_Enter,false);
    require(std::strcmp(editor.search,"not_a_real_node_9817z")==0,"Reactivating an unmatched search discarded immediately typed characters");
    chord(h,ImGuiMod_Ctrl,ImGuiKey_A);typeCatalogQuery(h,"Boolean");h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);
    require(h.graph().nodes.size()==4&&findNode(h.graph(),editor.selected[0])->kind==NodeKind::Boolean,"Refining an empty search did not recover normal creation");
    h.click(canvasAt(h,736,576),ImGuiMouseButton_Right);typeCatalogQuery(h,"target");h.key(ImGuiKey_DownArrow,true);h.key(ImGuiKey_DownArrow,false);
    const auto nodes=h.graph().nodes.size(),historySize=editor.undo.size();h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);h.frame();
    require(ImGui::GetCurrentContext()->OpenPopupStack.empty()&&h.graph().nodes.size()==nodes&&editor.undo.size()==historySize,"Escape did not cancel search without editing the graph");
}
void testCatalogSearchQuickAddPreservesExistingConnections()
{
    gestureScenario="Searched quick-add nodes connect in either direction in one Undo step";
    for(bool output:{true,false})for(auto accept:{ImGuiKey_Tab,ImGuiKey_Enter})
    {
        EditorHarness h;h.graph().links={{1,0,2,0}};edited();h.frame();
        h.drag(h.port(output?1:2,output),canvasAt(h,736,448));
        require(editor.catalogWireNode==(output?1:2)&&h.graph().links.size()==1,"Opening quick-add disconnected an existing wire");
        typeCatalogQuery(h,output?"NOT":"Boolean");h.key(accept,true);h.key(accept,false);h.frame();
        const int added=editor.selected[0];require(h.graph().nodes.size()==4&&editor.undo.size()==1,"Search quick-add did not create one complete transaction");
        require(std::any_of(h.graph().links.begin(),h.graph().links.end(),[&](const Link& link){return output?link.fromNode==1&&link.toNode==added:link.fromNode==added&&link.toNode==2;}),"Confirming a quick-add result did not connect it to the anchor");
        require(h.graph().links.size()==(output?2:1)&&ImGui::GetCurrentContext()->OpenPopupStack.empty(),"Quick-add left an obsolete connection or an open menu");
        history(false);h.frame();require(h.graph().nodes.size()==3&&h.graph().links.size()==1&&h.graph().links[0].fromNode==1&&h.graph().links[0].toNode==2,"Undo lost the original quick-add connection");
    }
}
void testEmptyCatalogCanBrowseWithArrows()
{
    gestureScenario="Down opens a searchable result list before a query is typed";
    EditorHarness h;h.click(canvasAt(h,736,448),ImGuiMouseButton_Right);h.frame();h.key(ImGuiKey_DownArrow,true);h.key(ImGuiKey_DownArrow,false);
    require(editor.catalogSearch.browse&&!editor.catalogSearch.nodes.empty()&&editor.catalogSearch.selected==0,"Down did not offer the first node from an empty search");
    const auto kind=editor.catalogSearch.nodes[0];h.key(ImGuiKey_Tab,true);h.key(ImGuiKey_Tab,false);h.frame();
    require(h.graph().nodes.size()==4&&findNode(h.graph(),editor.selected[0])->kind==kind,"Tab did not add a node from empty-query browsing");
}
void testCatalogSearchMouseSelectionAndCreationErrors()
{
    gestureScenario="Mouse selection remains usable in keyboard search, and failed creation keeps focus";
    {
        EditorHarness h;h.click(canvasAt(h,736,448),ImGuiMouseButton_Right);typeCatalogQuery(h,"target");h.frame();h.frame();
        auto* list=catalogListWindow();require(list&&editor.catalogSearch.nodes.size()>2,"Search results are unavailable for mouse selection");
        const auto kind=editor.catalogSearch.nodes[2];const float rowHeight=std::ceil(ImGui::GetFontSize()*1.8f+10);
        h.click(add(list->DC.CursorStartPos,ImVec2(40,rowHeight*2.5f)));
        require(h.graph().nodes.size()==4&&findNode(h.graph(),editor.selected[0])->kind==kind&&editor.undo.size()==1&&ImGui::GetCurrentContext()->OpenPopupStack.empty(),"Clicking a searched result failed to add the hovered node and close the library");
    }
    {
        EditorHarness h;h.graph().nodes.clear();for(int i=0;i<static_cast<int>(MaxNodes);++i)h.graph().nodes.push_back(defaultNode(NodeKind::Float,i+1,20000,i*192.0f));edited();h.frame();
        h.click(canvasAt(h,640,448),ImGuiMouseButton_Right);typeCatalogQuery(h,"Float");h.key(ImGuiKey_Tab,true);h.key(ImGuiKey_Tab,false);h.frame();
        require(h.graph().nodes.size()==MaxNodes&&editor.undo.empty()&&!editor.catalogSearch.error.empty()&&!ImGui::GetCurrentContext()->OpenPopupStack.empty(),"A failed search creation closed the library or partially changed the graph");
        require(ImGui::GetActiveID()==catalogRoot()->GetID("##search"),"A failed creation left search without keyboard focus");
    }
}
void testCatalogOpeningFrameInputIsPreserved()
{
    gestureScenario="Opening-frame text is accepted while the opening shortcut itself is excluded";
    {
        EditorHarness h;h.move(canvasAt(h,736,448));h.button(ImGuiMouseButton_Right,true);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right,false);ImGui::GetIO().AddInputCharactersUTF8("Boolean");h.frame();
        require(std::strcmp(editor.search,"Boolean")==0&&!editor.catalogSearch.nodes.empty(),"Typing delivered together with menu opening lost its initial characters");
        h.key(ImGuiKey_Tab,true);h.key(ImGuiKey_Tab,false);require(h.graph().nodes.size()==4,"A query typed in the opening frame could not be confirmed");
    }
    {
        EditorHarness h;h.key(ImGuiMod_Shift,true);ImGui::GetIO().AddInputCharactersUTF8("A");h.key(ImGuiKey_A,true);h.key(ImGuiKey_A,false);h.key(ImGuiMod_Shift,false);
        require(editor.search[0]==0,"The library-opening shortcut appeared as a search character");
        typeCatalogQuery(h,"Boolean");h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);
        require(h.graph().nodes.size()==4&&findNode(h.graph(),editor.selected[0])->kind==NodeKind::Boolean,"Typing after the opening shortcut lost a character");
    }
    {
        EditorHarness h;h.key(ImGuiMod_Shift,true);ImGui::GetIO().AddInputCharactersUTF8("A");h.key(ImGuiKey_A,true);h.key(ImGuiKey_A,false);h.key(ImGuiMod_Shift,false);
        h.key(ImGuiKey_Tab,true);h.key(ImGuiKey_Tab,false);
        for(int depth=0;depth<3;++depth){h.key(ImGuiKey_RightArrow,true);h.key(ImGuiKey_RightArrow,false);}
        require(ImGui::GetCurrentContext()->OpenPopupStack.Size>=4,"An opening shortcut's character stole Tab from empty-query category navigation");
    }
}
void testCatalogSearchPopupFitsTheViewport()
{
    gestureScenario="Growing search results stay inside the viewport without losing input focus";
    for(float height:{800.0f,540.0f,320.0f})
    {
        gestureScenario="Search popup fits viewport height "+std::to_string(height);
        EditorHarness h;ImGui::GetIO().DisplaySize=ImVec2(760,height);h.frame();
        h.click(ImVec2(h.origin.x+560,std::min(height-32,h.origin.y+editor.canvasSize[0].y-24)),ImGuiMouseButton_Right);typeCatalogQuery(h,"target");
        for(int frame=0;frame<5;++frame)h.frame();auto* root=catalogRoot();const auto* viewport=ImGui::GetMainViewport();
        require(root->Pos.x>=viewport->WorkPos.x&&root->Pos.y>=viewport->WorkPos.y&&
            root->Pos.x+root->Size.x<=viewport->WorkPos.x+viewport->WorkSize.x+1&&root->Pos.y+root->Size.y<=viewport->WorkPos.y+viewport->WorkSize.y+1,
            "The search popup extends outside the available viewport");
        auto* list=catalogListWindow();require(list&&list->Size.y<=height&&list->Pos.y+list->Size.y<=viewport->WorkPos.y+viewport->WorkSize.y+1,"Search results are clipped by the viewport edge");
        require(ImGui::GetActiveID()==root->GetID("##search"),"Repositioning search lost its keyboard focus");
    }
}
}
