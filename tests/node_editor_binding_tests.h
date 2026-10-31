#pragma once

namespace
{
int editorOptionPin(NodeKind kind,const char* key)
{
    const auto& inputs=definition(kind).inputs;
    for(std::size_t p=0;p<inputs.size();++p)if(inputs[p].defaultParameter&&std::string(inputs[p].defaultParameter)==key)return static_cast<int>(p);
    throw std::runtime_error("Option input is missing");
}
void testAllInputsConnectAndUndoAcrossZoom()
{
    for(const float zoom:GestureZooms)
    {
        gestureScenario="All parameter ports at zoom "+std::to_string(zoom);
        EditorHarness h;h.graph().nodes={defaultNode(NodeKind::Float,1,64,64),defaultNode(NodeKind::Prediction,2,416,64)};
        h.graph().links.clear();findNode(h.graph(),1)->value=0.75;h.graph().zoom=zoom;editor.viewMode=1;edited();h.frame();
        const int pin=editorOptionPin(NodeKind::Prediction,"kalman_velocity_damping");
        require(Layout::inputVisible(*findNode(h.graph(),2),pin),"A setting input is hidden on a new node");
        const auto ports=nodePorts(h.graph(),2);
        require(std::any_of(ports.begin(),ports.end(),[&](const Editor::PortFocus& p){return !p.output&&p.port==pin;}),"A setting input is absent from keyboard navigation");
        h.drag(h.port(1,true),h.port(2,false,pin),ImGuiMouseButton_Left);
        require(h.graph().links.size()==1&&h.graph().links[0].toPort==pin&&!editor.dragNode&&!editor.resizeNode,"A setting input could not be connected directly");
        require(drivenInput(h.graph(),*findNode(h.graph(),2),"kalman_velocity_damping")==pin,"Wired setting remained editable as an independent value");
        history(false);h.frame();require(h.graph().links.empty()&&Layout::inputVisible(*findNode(h.graph(),2),pin),"Undo of a connection hid its input");
    }
}
void testHighInputsStayStableAndPortable()
{
    gestureScenario="Always visible high inputs and portable geometry";
    EditorHarness h;h.graphIndex=3;h.graph().nodes={defaultNode(NodeKind::Float,1,64,64),defaultNode(NodeKind::RenderOverlay,2,416,64),defaultNode(NodeKind::Integer,3,64,320)};
    h.graph().links.clear();h.graph().zoom=0.45f;editor.viewMode=1;
    findNode(h.graph(),2)->height=Layout::up(Layout::minimumHeight(*findNode(h.graph(),2))+64);edited();h.frame();
    const int late=editorOptionPin(NodeKind::RenderOverlay,"snap_boost"),early=editorOptionPin(NodeKind::RenderOverlay,"game_overlay_box_r");
    require(late>31,"High-port regression no longer reaches a 64-bit input");
    h.drag(h.port(1,true),h.port(2,false,late),ImGuiMouseButton_Left);h.frame();
    auto& cache=editor.canvasRoutes[3];require((cache.connections[1][0]&(std::uint64_t{1}<<late))!=0,"Connection cache truncated an input above 31");
    const auto endpoint=cache.routes[0].path.back();h.drag(h.port(3,true),h.port(2,false,early),ImGuiMouseButton_Left);h.frame();
    require(h.graph().links.size()==2&&std::abs(cache.routes[0].path.back().y-endpoint.y)<0.01f,"Connecting an earlier input moved an existing socket");
    clearSelection(3);selectOne(3,2);std::string text,error;require(selectionText(h.graph(),3,text,error),error.c_str());
    require(pasteSelectionText(h.graph(),3,text,ImVec2(704,384),error),error.c_str());
    const auto* pasted=findNode(h.graph(),editor.selected[3]);
    require(pasted&&Layout::inputCount(*pasted)==definition(pasted->kind).inputs.size()&&pasted->height==findNode(h.graph(),2)->height,"Clipboard lost ports or manual dimensions");
    NodeValue source,target;const auto now=std::chrono::steady_clock::now();source.signalSequence=1;source.signaledAt=now;source.signalActive=true;target.readInputs=std::uint64_t{1}<<late;
    require(wireSignal(&source,&target,late,now).strength==1,"A consumed high input cannot animate its wire");
}
void testNamedCopiesKeepTheirOwnGetSetPairs()
{
    gestureScenario="Named values duplicate and paste without aliasing old writers";
    EditorHarness h;h.graph().nodes={defaultNode(NodeKind::Float,1,64,64),defaultNode(NodeKind::SetFloat,2,352,64),defaultNode(NodeKind::GetFloat,3,640,64)};
    findNode(h.graph(),2)->textParameters["name"]="gain";findNode(h.graph(),3)->textParameters["name"]="gain";h.graph().links={{1,0,2,0}};edited();selectAll(h.graph(),0);
    duplicateSelection(h.graph(),0);h.frame();
    const auto* get=findNode(h.graph(),editor.selected[0]);require(get&&variableGet(get->kind)&&textParameter(*get,"name")!="gain","Duplicated Get still points at the original Set");
    const auto name=textParameter(*get,"name");require(std::count_if(h.graph().nodes.begin(),h.graph().nodes.end(),[&](const Node& n){return variableSet(n.kind)&&textParameter(n,"name")==name;})==1,"Duplicating a named pair did not create one independent writer");
    std::string text,error;require(selectionText(h.graph(),0,text,error),error.c_str());require(pasteSelectionText(h.graph(),0,text,ImVec2(64,384),error),error.c_str());
    const auto pasted=textParameter(*findNode(h.graph(),editor.selected[0]),"name");require(pasted!=name&&pasted!="gain","Pasted Get aliases an existing writer");
    require(std::count_if(h.graph().nodes.begin(),h.graph().nodes.end(),[&](const Node& n){return variableSet(n.kind)&&textParameter(n,"name")==pasted;})==1,"Paste did not remap Get/Set names together");
}
void testVersion9PortsAndSpacingMigrateOnce()
{
    gestureScenario="Version 9 hidden ports and short cards migrate without losing data";
    EditorHarness h;TemporaryWorkspace directory;h.graphIndex=3;
    std::ostringstream legacy;legacy<<"SUNONE_NODE_GRAPH 9\n";
    for(int graph=0;graph<3;++graph)legacy<<"GRAPH "<<graph<<" 40 40 1 0 0 0 0\n";
    legacy<<"GRAPH 3 40 40 1 3 1 1 0\n"
        <<"NODE 1 "<<static_cast<int>(NodeKind::CaptureSource)<<" 32 64 0 0 0 1 0 0 0 320 0\nPARAM \"capture_fps\" 144\n"
        <<"NODE 2 "<<static_cast<int>(NodeKind::RenderOverlay)<<" 32 320 0 0 0 1 0 1 0 640 0\nPARAM \"game_overlay_max_fps\" 72\n"
        <<"NODE 3 "<<static_cast<int>(NodeKind::Float)<<" 32 128 7 0 1 0 0 0 0 0 0\n"
        <<"LINK 1 0 2 1 0\nGROUP 1 \"Vision controls\" 0 0 352 512 "<<0x40ffcd60u<<" 3 1 2 3\nEND\n";
    {std::ofstream file(GraphFile);file<<legacy.str();}
    NodeEditor_Load();h.frame();
    require(!editor.loadFailed&&h.graph().nodes.size()==3&&h.graph().links.size()==1,"A valid old workspace was rejected during port migration");
    const auto* capture=findNode(h.graph(),1);const auto* render=findNode(h.graph(),2);const auto* value=findNode(h.graph(),3);
    require(capture->height>=Layout::minimumHeight(*capture)&&capture->height>320&&render->height>=Layout::minimumHeight(*render)&&render->height>640,"Old manual heights still cover some inputs");
    require(parameter(*capture,"capture_fps")==144&&parameter(*render,"game_overlay_max_fps")==72&&render->disabled&&value->value==7&&value->collapsed,"Port migration changed settings or node state");
    for(const auto& first:h.graph().nodes)for(const auto& second:h.graph().nodes)if(first.id<second.id)
        require(first.x+cardWidth(first)+32<=second.x||second.x+cardWidth(second)+32<=first.x||
            first.y+Layout::height(first,true)+32<=second.y||second.y+Layout::height(second,true)+32<=first.y,"Expanding all old ports left overlapping cards");
    for(const auto& node:h.graph().nodes)
        require(Layout::inputCount(node)==definition(node.kind).inputs.size(),"An imported node still hides option inputs");
    const auto arranged=h.graph();NodeEditor_Flush();NodeEditor_Load();h.frame();
    for(const auto& node:arranged.nodes)
    {
        const auto* restored=findNode(h.graph(),node.id);
        require(restored&&restored->x==node.x&&restored->y==node.y&&restored->height==node.height,"Reopening a migrated graph arranged it again");
    }
}
void testEditableInputDefaultsOwnMouseAndUndo()
{
    for(float zoom:{0.65f,1.0f,1.75f})
    {
        gestureScenario="Adjacent input editor at zoom "+std::to_string(zoom);
        EditorHarness h;h.graph().zoom=zoom;h.graph().nodes={defaultNode(NodeKind::MakePoint,1,32,32),defaultNode(NodeKind::Float,2,32,224)};
        h.graph().links.clear();edited();h.frame();
        const auto port=h.port(1,false);const auto field=defaultField(h,1,0);
        const float label=ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize()*zoom,10000,0,"X").x;
        require(std::abs(field.x-inputEditorWidth(definition(NodeKind::MakePoint).inputs[0],zoom)*0.5f-port.x-12*zoom-label-6*zoom)<0.1f,"Default field is detached from the input label");
        replaceInlineText(h,field,"42.5",true);
        require(findNode(h.graph(),1)->inputValues.at(0)==42.5&&findNode(service().workspace().graphs[0],1)->inputValues.at(0)==42.5,"Input literal was not editable or immediately published");
        require(findNode(h.graph(),1)->x==32&&findNode(h.graph(),1)->y==32&&editor.undo.size()==1,"Editing an input moved its card or split the undo transaction");
        history(false);h.frame();require(findNode(h.graph(),1)->inputValues.empty(),"Undo did not restore an implicit zero default");
        history(true);h.frame();require(findNode(h.graph(),1)->inputValues.at(0)==42.5,"Redo lost the input literal");
        h.drag(h.port(2,true),h.port(1,false));require(h.graph().links.size()==1,"A nearby default editor stole a socket drag");
        h.key(ImGuiMod_Alt,true);h.click(h.port(1,false));h.key(ImGuiMod_Alt,false);
        require(h.graph().links.empty()&&findNode(h.graph(),1)->inputValues.at(0)==42.5,"Disconnecting a wire lost its fallback");

        h.graph().nodes={defaultNode(NodeKind::And,1,32,32)};h.graph().links.clear();editor.undo.clear();editor.redo.clear();edited();h.frame();
        h.click(defaultField(h,1,1));requireNoFieldGesture();
        require(findNode(h.graph(),1)->inputValues.at(1)==1&&editor.undo.size()==1,"Boolean input did not toggle in one undo transaction");
        history(false);h.frame();require(findNode(h.graph(),1)->inputValues.empty(),"Boolean input undo did not restore false");
    }
    gestureScenario="Typed local defaults and named options";
    EditorHarness h;h.graph().nodes={defaultNode(NodeKind::ConcatString,1,32,32)};edited();h.frame();
    replaceInlineText(h,defaultField(h,1,1),u8"folder/пример");
    require(findNode(h.graph(),1)->inputTexts.at(1)==u8"folder/пример","String input lost text while editing");
    std::string text,error;selectOne(0,1);require(selectionText(h.graph(),0,text,error)&&pasteSelectionText(h.graph(),0,text,ImVec2(416,32),error),error.c_str());
    require(findNode(h.graph(),editor.selected[0])->inputTexts.at(1)==u8"folder/пример","Clipboard lost local input defaults");
    h.graph().nodes={defaultNode(NodeKind::IntegerAdd,1,32,32)};h.graph().links.clear();edited();h.frame();
    replaceInlineText(h,defaultField(h,1,0),"2147483647",true);
    require(findNode(h.graph(),1)->inputValues.at(0)==MaxInteger,"Int default field lost precision");
    h.graph().nodes={defaultNode(NodeKind::FilterConfidence,1,32,32)};edited();h.frame();
    const int pin=editorOptionPin(NodeKind::FilterConfidence,"threshold");
    replaceInlineText(h,defaultField(h,1,pin),"0.75",true);
    require(parameter(*findNode(h.graph(),1),"threshold")==0.75&&findNode(h.graph(),1)->inputValues.empty(),"An option input did not update its named local setting");
}
void selectInputChoice(EditorHarness& h,int id,const char* key,int choice,int count=4)
{
    h.click(optionField(h,id,key));h.frame();h.frame();
    auto& popups=ImGui::GetCurrentContext()->OpenPopupStack;
    require(!popups.empty()&&popups.back().Window&&editor.inputPopupNode==id,"An input did not open its choice menu");
    const auto* popup=popups.back().Window;
    const float row=(popup->DC.CursorPos.y-popup->DC.CursorStartPos.y)/count;
    const ImVec2 target(popup->Pos.x+popup->Size.x*0.5f,popup->DC.CursorStartPos.y+row*(choice+0.5f));
    h.move(target);
    require(ImGui::IsPopupOpen(editor.inputPopup,ImGuiPopupFlags_AnyPopupLevel),"A choice menu closed when the mouse left its input");
    h.click(target);h.move(canvasAt(h,900,650));requireNoFieldGesture();
}
void testOptionsKeepChoicesAndFilePickingAtInputs()
{
    for(float zoom:{0.65f,1.0f})
    {
        gestureScenario="Input-only choice menus at zoom "+std::to_string(zoom);
        EditorHarness h;h.graph().zoom=zoom;h.graph().nodes={defaultNode(NodeKind::ToInteger,1,32,32)};edited();h.frame();
        selectInputChoice(h,1,"rounding",2);
        require(parameter(*findNode(h.graph(),1),"rounding")==2&&parameter(service().workspace().graphs[0].nodes[0],"rounding")==2,"A rounding choice at its input did not apply immediately");
        require(editor.undo.size()==1,"Choosing an input option created multiple undo entries");history(false);h.frame();
        require(parameter(*findNode(h.graph(),1),"rounding")==0,"Undo did not restore a choice made at its input");
        h.graph().nodes={defaultNode(NodeKind::KeyHeld,1,32,32)};edited();h.frame();
        selectInputChoice(h,1,"virtual_key",1,3);
        require(parameter(*findNode(h.graph(),1),"virtual_key")==KeyCodes::getKeyCode("F2"),"The key picker was lost when the duplicate settings panel was removed");
        h.graphIndex=3;h.graph().zoom=zoom;h.graph().nodes={defaultNode(NodeKind::CaptureSource,1,32,32)};edited();h.frame();
        selectInputChoice(h,1,"capture_method",1);
        require(textParameter(*findNode(h.graph(),1),"capture_method")=="winrt","A String choice is not editable at its input");
    }
    gestureScenario="Model file picker beside its input";
    EditorHarness h;h.graphIndex=3;h.graph().nodes={defaultNode(NodeKind::DepthInference,1,32,32)};edited();h.frame();
    const int pin=editorOptionPin(NodeKind::DepthInference,"model_path");const auto& node=*findNode(h.graph(),1);
    const auto bounds=inputValueRect(h.graph(),node,pin,h.origin,ImGui::GetFontSize(),inputEditorWidth(definition(node.kind).inputs[pin],1));
    h.click(ImVec2(bounds.Max.x-9,bounds.GetCenter().y));requireNoFieldGesture();
    require(editor.fileAction==3&&editor.fileGraph==3&&editor.fileNode==1&&editor.fileParameter=="model_path","The file picker did not follow the path input");
    h.graph().nodes={defaultNode(NodeKind::SetFloat,1,32,32)};editor.fileAction=0;edited();h.frame();
    replaceInlineText(h,inlineField(h,1,17),"shared_gain");
    require(textParameter(*findNode(h.graph(),1),"name")=="shared_gain","Static Get/Set identifiers became inaccessible");
}
void testOptionEditorBindings()
{
    testAllInputsConnectAndUndoAcrossZoom();testHighInputsStayStableAndPortable();testNamedCopiesKeepTheirOwnGetSetPairs();testVersion9PortsAndSpacingMigrateOnce();
    testEditableInputDefaultsOwnMouseAndUndo();
    testOptionsKeepChoicesAndFilePickingAtInputs();
}
}
