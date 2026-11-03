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
    require(!editor.loadFailed&&h.graph().nodes.size()==9&&h.graph().links.size()==7,"A valid old workspace was rejected during port migration");
    const auto* capture=findNode(h.graph(),1);const auto* render=findNode(h.graph(),2);const auto* value=findNode(h.graph(),3);
    require(capture->height>=Layout::minimumHeight(*capture)&&capture->height>320&&render->kind==NodeKind::OverlayOutput&&Layout::height(*render)<300,"Old outputs did not migrate to a compact card");
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
    gestureScenario="Model file picker from its input list";
    EditorHarness h;h.graphIndex=3;h.graph().nodes={defaultNode(NodeKind::DepthInference,1,32,32)};edited();h.frame();
    h.click(optionField(h,1,"model_path"));h.frame();h.frame();
    auto& popups=ImGui::GetCurrentContext()->OpenPopupStack;require(!popups.empty()&&popups.back().Window,"The model input did not open its list");
    const auto* popup=popups.back().Window;
    h.click(ImVec2(popup->WorkRect.Min.x+50,popup->DC.CursorPosPrevLine.y-ImGui::GetStyle().ItemSpacing.y-modelMenu.rowHeight*0.5f));requireNoFieldGesture();
    require(editor.fileAction==3&&editor.fileGraph==3&&editor.fileNode==1&&editor.fileParameter=="model_path","The file picker did not follow the path input");
    h.graph().nodes={defaultNode(NodeKind::SetFloat,1,32,32)};editor.fileAction=0;edited();h.frame();
    replaceInlineText(h,inlineField(h,1),"shared_gain");
    require(textParameter(*findNode(h.graph(),1),"name")=="shared_gain","Static Get/Set identifiers became inaccessible");
}
void testCompactLayerConnectionsAndInputs()
{
    for(float zoom:{0.65f,1.0f,1.75f})
    {
        gestureScenario="Compact overlay styles at zoom "+std::to_string(zoom);
        EditorHarness h;h.graphIndex=3;h.graph().zoom=zoom;editor.viewMode=1;
        h.graph().nodes={defaultNode(NodeKind::OverlayBoxes,1,32,32),defaultNode(NodeKind::OverlayOutput,2,448,32)};
        edited();h.frame();h.drag(h.port(1,true),h.port(2,false,5),ImGuiMouseButton_Left);
        require(h.graph().links.size()==1&&h.graph().links[0].toPort==5,"Compact styles could not connect to the renderer");
        const int pin=editorOptionPin(NodeKind::OverlayBoxes,"game_overlay_draw_boxes");
        h.click(defaultField(h,1,pin));requireNoFieldGesture();
        require(parameter(*findNode(h.graph(),1),"game_overlay_draw_boxes")==0&&parameter(service().workspace().graphs[3].nodes[0],"game_overlay_draw_boxes")==0,
            "A compact layer setting did not apply immediately");
        require(rerouteKind(ValueType::OverlayStyle)==NodeKind::RerouteOverlay&&rerouteKind(ValueType::LabelOptions)==NodeKind::RerouteLabels,
            "The new settings types lost reroute support");
        require(!kindAllowed(editor.draft.graphs[0],NodeKind::OverlayBoxes)&&!kindAllowed(editor.draft.graphs[0],NodeKind::DatasetOutput),"Vision settings appeared in control graphs");
    }
}
void testInputValuesUseAvailableWidthAndKeepMouseOwnership()
{
    for(float zoom:{0.65f,1.0f,1.75f})
    {
        gestureScenario="Adaptive input values at zoom "+std::to_string(zoom);
        EditorHarness h;h.graph().zoom=zoom;editor.viewMode=1;
        auto delta=defaultNode(NodeKind::MouseDelta,1,32,32);delta.width=416;
        delta.parameters["yaw"]=delta.parameters["pitch"]=0.0055555556;
        h.graph().nodes={delta};edited();h.frame();
        const int pin=editorOptionPin(NodeKind::MouseDelta,"yaw");
        auto& display=editor.canvasRoutes[0].displays[0];const auto& node=*findNode(h.graph(),1);const float font=ImGui::GetFontSize()*zoom;
        const float desired=inputValueWidth(display,node,pin,0,ImGui::GetFont(),font,zoom);
        const auto bounds=inputValueRect(h.graph(),node,pin,h.origin,font,desired);
        const auto& value=fitInputDefault(display,node,pin,0,ImGui::GetFont(),font,bounds.GetWidth()-6*zoom);
        require(value.shown==value.text&&value.shown.find("...")==std::string::npos&&bounds.GetWidth()>inputEditorWidth(definition(node.kind).inputs[pin],zoom),
            "Yaw is still abbreviated despite having room");
        const auto left=bounds.Min.x;
        replaceInlineText(h,ImVec2(bounds.Max.x-4*zoom,bounds.GetCenter().y),"0.0123456",true);
        require(std::abs(parameter(*findNode(h.graph(),1),"yaw")-0.0123456)<1e-12&&findNode(h.graph(),1)->x==32,
            "The expanded part of a value did not own the click or apply editing");
        require(left>h.port(1,false,pin).x,"An expanded value covered its input socket");
        auto text=defaultNode(NodeKind::ConcatString,1,32,32);text.width=1024;text.inputTexts[1]=std::string(129,'i');
        h.graph().nodes={text};h.graph().links.clear();edited();h.frame();
        auto& textDisplay=editor.canvasRoutes[0].displays[0];const auto& textNode=*findNode(h.graph(),1);
        const float textDesired=inputValueWidth(textDisplay,textNode,1,0,ImGui::GetFont(),font,zoom);
        const auto textBounds=inputValueRect(h.graph(),textNode,1,h.origin,font,textDesired);
        const auto& full=fitInputDefault(textDisplay,textNode,1,0,ImGui::GetFont(),font,textBounds.GetWidth()-6*zoom);
        require(full.shown==text.inputTexts[1],"A text value was cut at a fixed character count despite having room");
        findNode(h.graph(),1)->width=160;findNode(h.graph(),1)->inputTexts[1]=u8"Путь/модель/пример/long-file-name.onnx";edited();h.frame();
        const auto& narrowNode=*findNode(h.graph(),1);auto& narrowDisplay=editor.canvasRoutes[0].displays[0];
        const auto narrowBounds=inputValueRect(h.graph(),narrowNode,1,h.origin,font,inputValueWidth(narrowDisplay,narrowNode,1,0,ImGui::GetFont(),font,zoom));
        const auto& cropped=fitInputDefault(narrowDisplay,narrowNode,1,0,ImGui::GetFont(),font,std::max(0.0f,narrowBounds.GetWidth()-6*zoom));
        require(cropped.shown.find("...")!=std::string::npos&&cropped.width<=narrowBounds.GetWidth()-6*zoom+0.01f,
            "A genuinely narrow field does not clip its value safely");
    }
}
void testExistingWorkspaceInitializesModelInput()
{
    gestureScenario="Existing workspace restores its empty model input";
    EditorHarness h;TemporaryWorkspace files;std::filesystem::create_directory("models");
    {std::ofstream model("models/startup.onnx",std::ios::binary);model<<"model fixture";}
    struct RestoreModel{std::string value=config.ai_model;~RestoreModel(){config.ai_model=value;}} restore;
    config.ai_model="startup.onnx";h.graphIndex=3;
    h.graph().nodes={defaultNode(NodeKind::CaptureSource,1,32,64),defaultNode(NodeKind::Inference,2,352,64),defaultNode(NodeKind::VisionOutput,3,768,64)};
    findNode(h.graph(),2)->parameters["confidence_threshold"]=0.031;
    h.graph().links={{1,0,2,0},{2,0,3,1}};
    std::string error;if(!service().publish(editor.draft,error)||!service().save(GraphFile,error))throw std::runtime_error(error);
    NodeEditor_Load();h.frame();
    require(!editor.loadFailed&&textParameter(*findNode(h.graph(),2),"model_path")=="startup.onnx"&&editor.dirty,
        "Model selection still only runs for a newly created graph file");
    require(parameter(*findNode(h.graph(),2),"confidence_threshold")==0.031&&h.graph().links.size()==2,"Model initialization reset graph tuning or wires");
    NodeEditor_Flush();NodeEditor_Load();h.frame();
    require(textParameter(*findNode(h.graph(),2),"model_path")=="startup.onnx"&&!editor.dirty,"The selected model was not autosaved or was initialized again on every load");
}
ImGuiWindow* modelListWindow()
{
    const auto& popups=ImGui::GetCurrentContext()->OpenPopupStack;
    require(!popups.empty()&&popups.back().Window,"The model popup is not open");
    for(auto* window:ImGui::GetCurrentContext()->Windows)
        if(window->Active&&window->ParentWindow==popups.back().Window&&(window->Flags&ImGuiWindowFlags_ChildWindow))return window;
    throw std::runtime_error("The model list has no child window");
}
void testModelListsApplyUndoAndRefreshAcrossZoom()
{
    for(auto kind:{NodeKind::Inference,NodeKind::DepthInference})for(float zoom:{0.65f,1.0f,1.75f})
    {
        gestureScenario="Model list selection at zoom "+std::to_string(zoom);
        EditorHarness h;TemporaryWorkspace files;std::filesystem::create_directory("models");std::filesystem::create_directory("depth_models");
        for(const char* name:{"a.engine","b.onnx","c.ONNX","skip.txt"}){std::ofstream file(std::filesystem::path("models")/name);file<<"model fixture";}
        {std::ofstream file(std::filesystem::path("models")/std::filesystem::u8path(u8"глубина.onnx"));file<<"model fixture";}
        {std::ofstream file("depth_models/legacy.plan");file<<"model fixture";}
        std::filesystem::create_directory("models/not-a-file.onnx");
        h.graphIndex=3;h.graph().zoom=zoom;editor.viewMode=1;auto node=defaultNode(kind,1,32,0);node.width=416;
        if(kind==NodeKind::Inference)node.textParameters["backend"]="TRT";
        h.graph().nodes={node};edited();h.frame();h.click(optionField(h,1,"model_path"));h.frame();h.frame();
        require(modelMenu.choices.size()==(kind==NodeKind::DepthInference?5:4),"The model list lost supported files or included a directory/unsupported extension");
        const auto scans=modelMenu.scans;for(int i=0;i<8;++i)h.frame();
        require(modelMenu.scans==scans,"The open model menu scans the directory every frame");
        const auto found=std::find_if(modelMenu.choices.begin(),modelMenu.choices.end(),[](const ModelChoice& item){return item.name==u8"глубина.onnx";});
        require(found!=modelMenu.choices.end(),"Unicode model names are missing from the list");
        const int index=static_cast<int>(found-modelMenu.choices.begin());const auto* list=modelListWindow();
        h.click(ImVec2(list->WorkRect.Min.x+40,list->DC.CursorStartPos.y+(index+0.5f)*modelMenu.rowHeight));requireNoFieldGesture();
        require(textParameter(*findNode(h.graph(),1),"model_path")==u8"models/глубина.onnx"&&textParameter(service().workspace().graphs[3].nodes[0],"model_path")==u8"models/глубина.onnx",
            "Selecting a model did not immediately update its graph input");
        require(editor.undo.size()==1,"Selecting a model created more than one undo step");
        std::string encoded,error;Workspace loaded;
        require(serialize(editor.draft,encoded,error)&&deserialize(encoded,loaded,error)&&textParameter(loaded.graphs[3].nodes[0],"model_path")==u8"models/глубина.onnx","The selected relative model path did not export/import");
        history(false);h.frame();require(textParameter(*findNode(h.graph(),1),"model_path").empty(),"Undo did not restore the previous model");
        {std::ofstream file("models/new.onnx");file<<"model fixture";}
        h.click(optionField(h,1,"model_path"));h.frame();h.frame();
        require(modelMenu.scans==scans+1&&std::any_of(modelMenu.choices.begin(),modelMenu.choices.end(),[](const ModelChoice& item){return item.name=="new.onnx";}),"Reopening the model list did not discover new files");
        h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);h.frame();
        const auto closedScans=modelMenu.scans;h.move(canvasAt(h,900,650));for(int i=0;i<8;++i)h.frame();
        require(modelMenu.scans==closedScans,"A closed model selector keeps scanning the filesystem");
    }
}
void testModelMenuKeepsCustomPathsAndBackendFiltering()
{
    gestureScenario="Model menu supports custom paths and per-node backend filtering";
    EditorHarness h;TemporaryWorkspace files;std::filesystem::create_directory("models");
    {std::ofstream file("models/object.onnx");file<<"model fixture";}{std::ofstream file("models/compiled.engine");file<<"model fixture";}
    h.graphIndex=3;auto node=defaultNode(NodeKind::Inference,1,32,0);node.width=416;node.textParameters["backend"]="CPU";
    node.textParameters["model_path"]="outside/custom.onnx";h.graph().nodes={node};edited();h.frame();
    h.click(optionField(h,1,"model_path"));h.frame();h.frame();
    require(modelMenu.choices.size()==1&&modelMenu.choices[0].name=="object.onnx"&&modelMenu.selected==-1&&textParameter(*findNode(h.graph(),1),"model_path")=="outside/custom.onnx",
        "The CPU model list includes engines or replaces a custom selection");
    const auto* popup=ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
    replaceInlineText(h,ImVec2(popup->WorkRect.Min.x+60,popup->DC.CursorPosPrevLine.y+ImGui::GetFontSize()*0.5f),"elsewhere/model.onnx");
    require(textParameter(*findNode(h.graph(),1),"model_path")=="elsewhere/model.onnx","Custom paths are no longer editable from the model input");
    h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);h.frame();
    auto source=defaultNode(NodeKind::String,2,512,0);source.textParameters["value"]="wired.onnx";h.graph().nodes.push_back(source);
    h.graph().links.push_back({2,0,1,editorOptionPin(NodeKind::Inference,"model_path")});edited();h.frame();
    h.click(optionField(h,1,"model_path"));
    require(!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel)&&textParameter(*findNode(h.graph(),1),"model_path")=="elsewhere/model.onnx",
        "The model selector can override a connected input");
}
void testLocalSettingsRowsStayAlignedAcrossTypes()
{
    for(float zoom:{0.25f,0.65f,1.0f,1.75f})for(float width:{160.0f,192.0f,384.0f})
    {
        gestureScenario="Uniform local settings at zoom "+std::to_string(zoom)+" width "+std::to_string(width);EditorHarness h;editor.viewMode=1;h.graph().zoom=zoom;
        float content=-1,height=-1,valueX=-1;
        for(auto kind:{NodeKind::Boolean,NodeKind::Integer,NodeKind::Float,NodeKind::GetFloat})
        {
            auto node=defaultNode(kind,1,32,32);node.width=width;h.graph().nodes={node};h.graph().links.clear();h.graph().groups.clear();edited();h.frame();
            const auto row=localFieldRow(h,1);const auto panel=settingsRect(h.graph(),h.origin,*findNode(h.graph(),1));
            require(std::abs(row.label.GetCenter().y-row.value.GetCenter().y)<0.01f&&row.value.Min.x>=row.label.Max.x&&panel.Contains(row.value.Min)&&panel.Contains(row.value.Max),"A local label/value wraps, overlaps, or escapes the card");
            const auto& display=editor.canvasRoutes[0].displays[0];
            require(display.settingsContent<=nodeSettingsHeight(*findNode(h.graph(),1))+0.1f,"A one-row field requires hidden extra settings space");
            if(content>=0)require(std::abs(content-display.settingsContent)<0.1f&&std::abs(height-cardHeight(*findNode(h.graph(),1)))<0.1f&&std::abs(valueX-row.value.Min.x)<0.1f,"Bool, Int, Float and Name use different local row geometry");
            content=display.settingsContent;height=cardHeight(*findNode(h.graph(),1));valueX=row.value.Min.x;
        }
        h.graph().nodes={defaultNode(NodeKind::Boolean,1,32,32)};h.graph().nodes[0].width=width;edited();h.frame();
        const auto field=localFieldRow(h,1).value;const auto color=ImGui::GetColorU32(ImGuiCol_CheckboxSelectedBg)&0x00ffffff;
        require(std::any_of(h.vertices.begin(),h.vertices.end(),[&](const ImDrawVert& vertex){return (vertex.col&0x00ffffff)==color&&vertex.pos.x>=field.Min.x-1&&vertex.pos.x<=field.Min.x+4*zoom&&vertex.pos.y>=field.Min.y&&vertex.pos.y<=field.Max.y;}),"The Bool toggle is detached from the common value column");
        h.click(inlineField(h,1));requireNoFieldGesture();require(findNode(h.graph(),1)->value==0,"The scaled single-row switch missed its hit area");
    }
}
void testLocalSettingsPreviewUsesTheSameRow()
{
    gestureScenario="Local fields in previews share their label/value row";EditorHarness h;
    for(float width:{180.0f,380.0f})
    {
        float previous=-1;
        for(auto kind:{NodeKind::Boolean,NodeKind::Integer,NodeKind::Float})
        {
            CatalogPreviewState preview;preview.kind=kind;preview.node=defaultNode(kind,1);layoutCatalogCard(preview,width);
            const auto row=localSettingsRow(ImVec2(11,preview.settingsY+6),width-33,ImGui::GetFontSize(),1);
            require(row.value.Min.y==preview.settingsY+6&&row.value.Max.y<=preview.cardHeight-12,"A local preview editor is on a second row or clipped");
            if(previous>=0)require(std::abs(previous-preview.cardHeight)<0.01f,"Boolean and numeric previews reserve different local settings heights");
            previous=preview.cardHeight;
        }
    }
}
void testOptionEditorBindings()
{
    testLocalSettingsRowsStayAlignedAcrossTypes();testLocalSettingsPreviewUsesTheSameRow();
    testAllInputsConnectAndUndoAcrossZoom();testHighInputsStayStableAndPortable();testNamedCopiesKeepTheirOwnGetSetPairs();testVersion9PortsAndSpacingMigrateOnce();
    testEditableInputDefaultsOwnMouseAndUndo();
    testOptionsKeepChoicesAndFilePickingAtInputs();
    testCompactLayerConnectionsAndInputs();
    testInputValuesUseAvailableWidthAndKeepMouseOwnership();
    testExistingWorkspaceInitializesModelInput();
    testModelListsApplyUndoAndRefreshAcrossZoom();testModelMenuKeepsCustomPathsAndBackendFiltering();
}
}
