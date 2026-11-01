namespace
{
void requireCatalogPreviewVisible(NodeKind kind)
{
    const auto& preview=editor.catalogPreview;auto* window=ImGui::FindWindowByName("Node preview###catalog_node_preview");
    require(preview.kind==kind&&preview.drawnFrame==ImGui::GetFrameCount()&&window&&window->Active,"The selected node has no visible catalog preview");
    require((window->Flags&ImGuiWindowFlags_NoNav)==ImGuiWindowFlags_NoNav&&!(window->Flags&ImGuiWindowFlags_NoMouseInputs),"The catalog preview cannot scroll or interferes with keyboard navigation");
    const auto* viewport=ImGui::GetMainViewport();const ImRect work(viewport->WorkPos,add(viewport->WorkPos,viewport->WorkSize));
    require(work.Contains(preview.bounds),"The catalog preview extends outside the viewport");
    for(const auto& popup:ImGui::GetCurrentContext()->OpenPopupStack)
        if(popup.Window&&popup.Window->Active)require(!preview.bounds.Overlaps(popup.Window->Rect()),"The catalog preview covers a menu or submenu");
}
void testCatalogPreviewFollowsEveryHoveredSubmenuRow()
{
    gestureScenario="Moving between submenu rows immediately previews the hovered node";
    EditorHarness h;chord(h,ImGuiMod_Shift,ImGuiKey_A);h.key(ImGuiKey_Tab,true);h.key(ImGuiKey_Tab,false);
    for(int depth=0;depth<3;++depth){h.key(ImGuiKey_RightArrow,true);h.key(ImGuiKey_RightArrow,false);h.frame();}
    auto& context=*ImGui::GetCurrentContext();require(context.OpenPopupStack.Size>=4,"The control-button submenu did not open");
    auto* submenu=context.OpenPopupStack[3].Window;const auto before=h.graph().nodes.size();
    const NodeKind kinds[]={NodeKind::AimHeld,NodeKind::ShootHeld,NodeKind::ZoomHeld};
    const auto point=[&](int row){return add(submenu->DC.CursorStartPos,ImVec2(35,row*ImGui::GetTextLineHeightWithSpacing()+ImGui::GetTextLineHeight()*0.5f));};
    for(int row:{0,1,2,1,0}){h.move(point(row));requireCatalogPreviewVisible(kinds[row]);}
    for(int row:{0,1,2,1,0,2})
    {
        h.move(point(row));requireCatalogPreviewVisible(kinds[row]);
        for(int frame=0;frame<20;++frame){h.frame();requireCatalogPreviewVisible(kinds[row]);}
        h.move(editor.catalogPreview.bounds.GetCenter());for(int frame=0;frame<20;++frame)h.frame();requireCatalogPreviewVisible(kinds[row]);
    }
    h.move(point(2));h.key(ImGuiKey_UpArrow,true);h.key(ImGuiKey_UpArrow,false);h.frame();requireCatalogPreviewVisible(NodeKind::ShootHeld);
    require(h.graph().nodes.size()==before&&editor.undo.empty(),"Switching catalog previews created a node or history entry");
}
void testCatalogSearchPreviewFollowsMouseRows()
{
    gestureScenario="Search preview follows each hovered result in both directions";
    EditorHarness h;h.click(canvasAt(h,736,448),ImGuiMouseButton_Right);typeCatalogQuery(h,"target");for(int frame=0;frame<4;++frame)h.frame();
    auto* list=catalogListWindow();require(list&&editor.catalogSearch.nodes.size()>4,"The search fixture has too few results");
    const float rowHeight=std::ceil(ImGui::GetFontSize()*1.8f+10);
    for(int row:{0,1,3,2,0})
    {
        h.move(add(list->DC.CursorStartPos,ImVec2(40,rowHeight*(row+0.5f))));
        require(editor.catalogSearch.selected==row,"Hovering a search result did not change its selection");requireCatalogPreviewVisible(editor.catalogSearch.nodes[row]);
    }
}
void testCatalogPreviewTracksSelectionWithoutExecuting()
{
    gestureScenario="Catalog previews follow search and keyboard selection without running or changing the graph";
    EditorHarness h;h.graphIndex=3;h.frame();const auto revision=service().revision(),scans=modelMenu.scans;
    h.click(canvasAt(h,736,448),ImGuiMouseButton_Right);typeCatalogQuery(h,"Preview");h.frame();
    requireCatalogPreviewVisible(NodeKind::ImagePreview);const auto builds=editor.catalogPreview.builds;
    for(int frame=0;frame<30;++frame)h.frame();
    require(editor.catalogPreview.builds==builds,"Idle catalog frames rebuilt the preview's prototype and defaults");
    require(ImGui::GetActiveID()==catalogRoot()->GetID("##search"),"The catalog preview stole search focus");
    chord(h,ImGuiMod_Ctrl,ImGuiKey_A);typeCatalogQuery(h,"target");h.key(ImGuiKey_DownArrow,true);h.key(ImGuiKey_DownArrow,false);h.frame();
    require(editor.catalogSearch.selected==1,"Preview rendering interfered with result navigation");requireCatalogPreviewVisible(editor.catalogSearch.nodes[1]);
    require(editor.catalogPreview.inputRows==static_cast<int>(definition(editor.catalogPreview.kind).inputs.size())&&editor.catalogPreview.outputRows==static_cast<int>(definition(editor.catalogPreview.kind).outputs.size()),"A small node's ports are hidden in its catalog preview");
    require(h.graph().nodes.empty()&&editor.undo.empty()&&!editor.dirty&&service().revision()==revision,"Previewing a catalog item edited, published or saved the graph");
    const auto requests=service().previewNodes();
    require(modelMenu.scans==scans&&(!requests||requests->empty())&&service().providerNodes(NodeKind::CaptureSource).empty(),"Catalog preview scanned models or requested runtime images/providers");
    h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);h.frame();
    require(!ImGui::FindWindowByName("Node preview###catalog_node_preview")->Active,"Closing the catalog left a stale preview on screen");
}
void testCatalogPreviewShowsDefaultsAndBoundsLargeNodes()
{
    gestureScenario="Catalog prototypes show creation defaults and bound large schemas";
    EditorHarness h;h.graphIndex=3;h.frame();h.click(canvasAt(h,736,448),ImGuiMouseButton_Right);typeCatalogQuery(h,"NMS");h.frame();
    requireCatalogPreviewVisible(NodeKind::FilterNms);const auto& defaults=editor.catalogPreview.display.defaults;
    require(editor.catalogPreview.inputRows==static_cast<int>(definition(NodeKind::FilterNms).inputs.size())&&editor.catalogPreview.outputRows==1,"The NMS preview unnecessarily hides ports");
    require(defaults.size()==definition(NodeKind::FilterNms).inputs.size()&&defaults[1].text=="0.5","The NMS preview omits or misrepresents its default threshold");
    chord(h,ImGuiMod_Ctrl,ImGuiKey_A);typeCatalogQuery(h,"Boolean");h.frame();requireCatalogPreviewVisible(NodeKind::Boolean);
    require(editor.catalogPreview.node.value==1,"The Boolean preview differs from a newly created node");
    chord(h,ImGuiMod_Ctrl,ImGuiKey_A);typeCatalogQuery(h,"Object inference");h.frame();requireCatalogPreviewVisible(NodeKind::Inference);
    const auto& preview=editor.catalogPreview;
    require(preview.inputRows==static_cast<int>(definition(NodeKind::Inference).inputs.size())&&preview.outputRows==static_cast<int>(definition(NodeKind::Inference).outputs.size())&&preview.bounds.GetHeight()<=600,"The large preview omits ports or grows beyond its bounded window");
    require(preview.display.defaults.size()==definition(NodeKind::Inference).inputs.size(),"The cached prototype loses input defaults");
    h.key(ImGuiKey_Tab,true);h.key(ImGuiKey_Tab,false);h.frame();
    require(h.graph().nodes.size()==1&&h.graph().nodes[0].kind==NodeKind::Inference&&ImGui::GetCurrentContext()->OpenPopupStack.empty(),"The preview blocked keyboard node creation");
}
void testCatalogCardWrapsFullValuesAndReusesLayout()
{
    gestureScenario="The node card wraps full names and values without overlapping ports";
    EditorHarness h;CatalogPreviewState preview;preview.kind=NodeKind::Inference;preview.node=defaultNode(preview.kind,1);
    const std::string path="models/"+std::string(250,'W')+".onnx";preview.node.textParameters["model_path"]=path;
    const auto& def=definition(preview.kind);
    for(std::size_t pin=0;pin<def.inputs.size();++pin)inputDefault(preview.display,preview.node,pin);
    for(float width:{180.0f,380.0f,560.0f})
    {
        layoutCatalogCard(preview,width);const auto layouts=preview.layouts;
        require(preview.rows.size()==std::max(def.inputs.size(),def.outputs.size()),"The node card drops rows to fit its height");
        float previous=preview.headerHeight;
        for(std::size_t pin=0;pin<preview.rows.size();++pin)
        {
            const auto& row=preview.rows[pin];require(row.y>=previous&&row.y+row.height<=preview.cardHeight,"Node preview rows overlap or extend outside the card");previous=row.y+row.height;
            if(pin>=def.inputs.size())continue;
            const bool model=modelPathInput(preview.kind,def.inputs[pin].defaultParameter);
            const float padding=model?26.0f:8.0f;
            const auto& value=catalogDefaultValue(preview.display.defaults[pin]);
            const float labelHeight=ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(),FLT_MAX,row.labelWidth,def.inputs[pin].name).y;
            const float valueHeight=ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(),FLT_MAX,std::max(1.0f,row.valueWidth-padding),value.c_str()).y;
            require(labelHeight<=row.height&&row.valueY+valueHeight<=row.height&&row.valueX+row.valueWidth<=width-12-(pin<def.outputs.size()?row.outputWidth+16:0)+0.1f,"A full label/default overlaps another row or output port");
            if(model)require(value=='"'+path+'"'&&valueHeight>ImGui::GetFontSize(),"The model path is shortened instead of wrapping inside the node");
        }
        layoutCatalogCard(preview,width);require(preview.layouts==layouts,"An unchanged node preview recomputes text wrapping each frame");
    }
}
void testCatalogPreviewScrollDoesNotDismissMenusOrMoveTheGraph()
{
    gestureScenario="Large catalog previews scroll all ports without stealing search or canvas gestures";
    EditorHarness h;h.graphIndex=3;ImGui::GetIO().DisplaySize.y=360;h.frame();h.click(canvasAt(h,736,220),ImGuiMouseButton_Right);typeCatalogQuery(h,"Object inference");
    for(int frame=0;frame<5;++frame)h.frame();requireCatalogPreviewVisible(NodeKind::Inference);
    auto* window=ImGui::FindWindowByName("Node preview###catalog_node_preview");
    require(window->ScrollbarY&&window->ScrollMax.y>0&&window->ScrollMax.x==0,("The large preview has no vertical scrollbar or overflows horizontally: size="+std::to_string(window->Size.x)+","+std::to_string(window->Size.y)+" scroll="+std::to_string(window->ScrollMax.x)+","+std::to_string(window->ScrollMax.y)+" content="+std::to_string(window->ContentSize.x)+","+std::to_string(window->ContentSize.y)).c_str());
    require(editor.catalogPreview.rows.size()==definition(NodeKind::Inference).inputs.size(),"The preview card omits ports below the viewport");
    const auto revision=service().revision(),builds=editor.catalogPreview.builds;const auto zoom=h.graph().zoom,panX=h.graph().panX,panY=h.graph().panY;
    const auto searchId=catalogRoot()->GetID("##search");h.move(editor.catalogPreview.bounds.GetCenter());
    ImGui::GetIO().AddMouseWheelEvent(0,-4);h.frame();h.frame();
    require(window->Scroll.y>0&&ImGui::GetActiveID()==searchId,("Wheel scrolling the preview failed or lost search focus: scroll="+std::to_string(window->Scroll.y)+" active="+std::to_string(ImGui::GetActiveID())+" search="+std::to_string(searchId)+" hovered="+(ImGui::GetCurrentContext()->HoveredWindow?ImGui::GetCurrentContext()->HoveredWindow->Name:"none")+" nav="+(ImGui::GetCurrentContext()->NavWindow?ImGui::GetCurrentContext()->NavWindow->Name:"none")).c_str());
    const auto bar=ImGui::GetWindowScrollbarRect(window,ImGuiAxis_Y);
    const float grab=std::max(ImGui::GetStyle().GrabMinSize,bar.GetHeight()*window->InnerRect.GetHeight()/(window->ContentSize.y+2*window->WindowPadding.y));
    const float grabY=bar.Min.y+(bar.GetHeight()-grab)*window->Scroll.y/window->ScrollMax.y+grab*0.5f;
    h.drag(ImVec2(bar.GetCenter().x,grabY),ImVec2(bar.GetCenter().x,bar.Max.y-4));for(int frame=0;frame<4;++frame)h.frame();
    require(window->Scroll.y>window->ScrollMax.y-2&&ImGui::GetActiveID()==searchId,("Dragging the preview scrollbar cannot reach the last port or restore typing focus: scroll="+std::to_string(window->Scroll.y)+" max="+std::to_string(window->ScrollMax.y)+" active="+std::to_string(ImGui::GetActiveID())+" search="+std::to_string(searchId)).c_str());
    requireCatalogPreviewVisible(NodeKind::Inference);
    require(h.graph().nodes.empty()&&editor.undo.empty()&&!editor.dirty&&service().revision()==revision&&h.graph().zoom==zoom&&h.graph().panX==panX&&h.graph().panY==panY,"Preview scrolling edited or moved the graph");
    require(editor.catalogPreview.builds==builds,"Preview scrolling rebuilt its node prototype");
    chord(h,ImGuiMod_Ctrl,ImGuiKey_A);typeCatalogQuery(h,"Boolean");h.frame();requireCatalogPreviewVisible(NodeKind::Boolean);
    require(window->Scroll.y==0&&window->ScrollMax.y==0,"Changing the selected node retains a stale preview scroll position");
    h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);h.frame();
    require(h.graph().nodes.size()==1&&h.graph().nodes[0].kind==NodeKind::Boolean,"Typing and Enter stopped working after preview scrolling");
}
void testCatalogPreviewPlacementAvoidsMenusAtViewportEdges()
{
    gestureScenario="Catalog previews use free viewport space at every menu edge";
    const ImRect viewport(ImVec2(8,8),ImVec2(1192,792));
    const ImRect menus[]={{{8,8},{388,538}},{{812,8},{1192,538}},{{8,262},{388,792}},{{812,262},{1192,792}},{{300,400},{1000,740}},{{200,8},{1000,320}}};
    for(const auto& menu:menus)
    {
        const auto preview=catalogPreviewPlacement(menu,viewport,ImVec2(380,480));
        require(preview.GetWidth()>=180&&preview.GetHeight()>=140&&viewport.Contains(preview)&&!preview.Overlaps(menu),"Preview placement overlaps menus or falls outside the viewport");
    }
    const auto unavailable=catalogPreviewPlacement(viewport,viewport,ImVec2(380,480));
    require(unavailable.GetWidth()==0&&unavailable.GetHeight()==0,"A preview covers menus when there is no available space");
}
}
