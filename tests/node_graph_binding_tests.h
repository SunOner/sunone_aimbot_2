#pragma once
#include <cstring>

namespace
{
int optionPin(NodeKind kind,const char* key)
{
    const auto& pins=definition(kind).inputs;
    for(std::size_t i=0;i<pins.size();++i)if(pins[i].defaultParameter&&std::string(pins[i].defaultParameter)==key)return static_cast<int>(i);
    throw std::runtime_error(std::string("Missing option input: ")+definition(kind).title+" / "+key);
}
Node stringNode(int id,const std::string& value)
{
    auto result=defaultNode(NodeKind::String,id);result.textParameters["value"]=value;return result;
}
void testEveryOptionHasATypedInput()
{
    for(std::size_t i=0;i<definitions().size();++i)
    {
        const auto& def=definitions()[i];require(static_cast<std::size_t>(def.kind)==i,"Definition order no longer matches persisted node IDs");
        require(def.inputs.size()<=MaxPins&&def.dataInputs<=MaxDataPins,"Option inputs exceeded their bounded storage");
        for(const auto& spec:def.parameters)
        {
            const auto pin=optionPin(def.kind,spec.key);const auto type=def.inputs[pin].type;
            require(std::count_if(def.inputs.begin(),def.inputs.end(),[&](const Pin& input){return input.defaultParameter&&std::strcmp(input.defaultParameter,spec.key)==0;})==1,"A numeric setting does not have exactly one input");
            require(type==(spec.kind==ParameterKind::Boolean?ValueType::Boolean:spec.kind==ParameterKind::Integer?ValueType::Integer:ValueType::Float)||
                (!def.inputs[pin].option&&type==ValueType::Float&&spec.kind==ParameterKind::Integer),"An option has the wrong wire type");
        }
        for(const auto& spec:def.textParameters)if(!((variableGet(def.kind)||variableSet(def.kind))&&std::string(spec.key)=="name"))
        {
            require(def.inputs[optionPin(def.kind,spec.key)].type==ValueType::String,"A text option has no String input");
            require(std::count_if(def.inputs.begin(),def.inputs.end(),[&](const Pin& input){return input.defaultParameter&&std::strcmp(input.defaultParameter,spec.key)==0;})==1,"A text setting does not have exactly one input");
        }
    }
}
void testBoundOptionsReachNativeCallbacks()
{
    Service service;Fixture fixture;auto workspace=defaults();auto& graph=workspace.graphs[0];
    constant(graph,90,0.7);constant(graph,91,1,true);graph.nodes.push_back(stringNode(92,"wired sensitivity profile"));
    graph.links.push_back({90,0,5,optionPin(NodeKind::Prediction,"kalman_velocity_damping")});
    graph.links.push_back({91,0,5,optionPin(NodeKind::Prediction,"kalman_enabled")});
    graph.links.push_back({92,0,10,optionPin(NodeKind::MouseDelta,"profile_name")});
    bool predicted=false,converted=false;
    const auto predict=fixture.context.predict;const auto delta=fixture.context.mouseDelta;
    fixture.context.predict=[&](int key,Point point,const Node& settings,double lookahead,bool reset){predicted=parameter(settings,"kalman_velocity_damping")==0.7&&parameter(settings,"kalman_enabled")==1;return predict(key,point,settings,lookahead,reset);};
    fixture.context.mouseDelta=[&](Point point,double speed,Point center,const Node& settings){converted=textParameter(settings,"profile_name")=="wired sensitivity profile";return delta(point,speed,center,settings);};
    publish(service,workspace);require(service.evaluate(fixture.context).aim&&predicted&&converted,"A callback received stored options instead of its wired values");
    require(textParameter(node(service.workspace().graphs[0],10),"profile_name")!="wired sensitivity profile","Runtime bindings overwrote an editable fallback");
}
void testWideOptionMasksAndTextOutputs()
{
    Service service;VisionFixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[3];
    graph.nodes={defaultNode(NodeKind::CaptureSource,1),defaultNode(NodeKind::RenderOverlay,2),defaultNode(NodeKind::Boolean,3),
        defaultNode(NodeKind::Integer,4),defaultNode(NodeKind::Float,5),stringNode(6,u8"icons/пример.png"),defaultNode(NodeKind::Boolean,7)};
    node(graph,4).value=191;node(graph,5).value=1.75;node(graph,7).value=0;
    const int late=optionPin(NodeKind::RenderOverlay,"snap_boost");require(late>31,"Wide-mask regression no longer exercises a high input");
    graph.links={{3,0,2,0},{1,0,2,1},{4,0,2,optionPin(NodeKind::RenderOverlay,"game_overlay_box_r")},{5,0,2,late},
        {6,0,2,optionPin(NodeKind::RenderOverlay,"game_overlay_icon_path")},{7,0,2,optionPin(NodeKind::RenderOverlay,"game_overlay_draw_boxes")}};
    bool received=false;int renders=0;
    fixture.context.render=[&](int,const FrameStream&,const DetectionStream&,const MaskStream&,const FrameStream&,const Node& settings){
        ++renders;received=parameter(settings,"game_overlay_box_r")==191&&parameter(settings,"snap_boost")==1.75&&parameter(settings,"game_overlay_draw_boxes")==0&&textParameter(settings,"game_overlay_icon_path")==u8"icons/пример.png";return true;};
    publish(service,workspace);service.evaluateVision(fixture.context);
    require(received&&renders==1&&(sampleNode(service.telemetry(),3,2).readInputs&(std::uint64_t{1}<<late)),"A setting above input 31 was lost or its callback saw stale values");
    node(graph,4).value=999;publish(service,workspace);service.evaluateVision(fixture.context);
    require(renders==1&&!sampleNode(service.telemetry(),3,2).value.error.empty(),"An out-of-range wired option reached a side effect");
}
void testMonitorsConfigureCaptureBeforeAFrameExists()
{
    Service service;VisionFixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[3];
    graph.nodes={defaultNode(NodeKind::CaptureSource,1),defaultNode(NodeKind::Inference,2),defaultNode(NodeKind::VisionOutput,3),defaultNode(NodeKind::Boolean,4),
        defaultNode(NodeKind::GetMonitors,5),defaultNode(NodeKind::MonitorAt,6),defaultNode(NodeKind::Integer,7),defaultNode(NodeKind::Integer,8),
        stringNode(9,"winrt"),stringNode(10,u8"models/модель.onnx"),defaultNode(NodeKind::GetMonitor,11),defaultNode(NodeKind::SetMonitor,12),defaultNode(NodeKind::Boolean,13)};
    node(graph,1).parameters["enabled"]=0;node(graph,1).textParameters["capture_target"]="window";
    node(graph,7).value=1;node(graph,8).value=144;node(graph,11).textParameters["name"]="screen";node(graph,12).textParameters["name"]="screen";
    graph.links={{1,0,2,0},{2,0,3,1},{13,0,3,0},{5,0,6,0},{7,0,6,1},{6,0,12,0},{11,0,1,0},
        {4,0,1,optionPin(NodeKind::CaptureSource,"enabled")},{8,0,1,optionPin(NodeKind::CaptureSource,"capture_fps")},
        {9,0,1,optionPin(NodeKind::CaptureSource,"capture_method")},{10,0,2,optionPin(NodeKind::Inference,"model_path")}};
    auto inventory=std::make_shared<std::vector<MonitorInfo>>();
    inventory->push_back({3,0,0,0,1920,1080,"DISPLAY-A","Primary",true});inventory->push_back({9,1,-2560,0,2560,1440,"DISPLAY-B","Secondary",false});
    int enumerations=0,invalidations=0;fixture.context.monitors=[&]()->MonitorList{++enumerations;return inventory;};fixture.context.captureConfigurationChanged=[&]{++invalidations;};
    publish(service,workspace);require(service.hasDynamicProviders(),"Capture did not discover its option dependencies");
    auto configured=service.evaluateConfiguration(fixture.context);
    const auto source=std::find_if(configured.begin(),configured.end(),[](const Node& n){return n.kind==NodeKind::CaptureSource;});
    require(source!=configured.end()&&parameter(*source,"monitor_idx")==9&&parameter(*source,"capture_fps")==144&&textParameter(*source,"monitor_device")=="DISPLAY-B"&&textParameter(*source,"capture_target")=="monitor", "Monitor/Get/Set did not configure capture before the first frame");
    require(fixture.captures==0&&fixture.inferences==0&&enumerations==1&&invalidations==1,"Configuration evaluation started a provider or skipped source invalidation");
    const auto capture=fixture.context.captureFrame;bool sameOptions=false;
    fixture.context.captureFrame=[&](int key,const Node& settings){sameOptions=parameter(settings,"monitor_idx")==9&&textParameter(settings,"capture_method")=="winrt";return capture(key,settings);};
    fixture.context.useConfigurationSnapshot=true;service.evaluateVision(fixture.context);
    require(sameOptions&&enumerations==1&&fixture.captures==1&&fixture.inferences==1,"Frame evaluation recomputed configuration or used stale capture options");
    require(sampleNode(service.telemetry(),3,2).value.detections->modelName==u8"models/модель.onnx","A model path was not driven by a String node");
    graph.nodes.push_back(defaultNode(NodeKind::KeyHeld,14));node(graph,14).parameters["virtual_key"]=65;
    graph.nodes.push_back(defaultNode(NodeKind::SelectInteger,15));graph.nodes.push_back(defaultNode(NodeKind::Integer,16));
    for(auto& link:graph.links)if(link.toNode==6&&link.toPort==1)link.fromNode=15;
    graph.links.insert(graph.links.end(),{{14,0,15,0},{7,0,15,1},{16,0,15,2}});bool held=false;fixture.context.keyDown=[&](int key){return key==65&&held;};publish(service,workspace);
    const auto revision=service.revision();configured=service.evaluateConfiguration(fixture.context);
    const auto configuredIndex=[](const std::vector<Node>& nodes){const auto n=std::find_if(nodes.begin(),nodes.end(),[](const Node& n){return n.kind==NodeKind::CaptureSource;});require(n!=nodes.end(),"Selected monitor has no capture configuration");return parameter(*n,"monitor_idx");};
    require(configuredIndex(configured)==3,"Select did not choose its false monitor branch");
    held=true;configured=service.evaluateConfiguration(fixture.context);
    require(service.revision()==revision&&configuredIndex(configured)==9,"Live inputs could not select another monitor without publishing the graph");
    const int stable=invalidations;service.evaluateConfiguration(fixture.context);require(invalidations==stable,"Unchanged source settings repeatedly invalidate frames");
    node(graph,8).value=120;publish(service,workspace);service.evaluateConfiguration(fixture.context);require(invalidations==stable,"Frame-rate feedback restarted source identity");
    node(graph,7).value=255;publish(service,workspace);configured=service.evaluateConfiguration(fixture.context);
    require(std::none_of(configured.begin(),configured.end(),[](const Node& n){return n.kind==NodeKind::CaptureSource;})&&!sampleNode(service.telemetry(),3,1).value.error.empty(),"An unavailable monitor silently fell back to another screen");
    node(graph,4).value=0;publish(service,workspace);const int before=enumerations;configured=service.evaluateConfiguration(fixture.context);
    require(std::none_of(configured.begin(),configured.end(),[](const Node& n){return n.kind==NodeKind::CaptureSource;})&&enumerations==before,"Disabled capture evaluated its monitor/options branch");
}
void testNamedValuesAndLazySelection()
{
    Service service;Fixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
    graph.nodes={defaultNode(NodeKind::Float,1),defaultNode(NodeKind::SetFloat,2),defaultNode(NodeKind::GetFloat,3),defaultNode(NodeKind::Boolean,4),defaultNode(NodeKind::RecoilOutput,5)};
    node(graph,1).value=2.5;node(graph,2).textParameters["name"]="gain";node(graph,3).textParameters["name"]="gain";graph.links={{1,0,2,0},{3,0,5,1},{4,0,5,0}};
    publish(service,workspace);require(service.evaluate(fixture.context).recoilStrength==2.5&&sampleNode(service.telemetry(),2,2).active,"Get did not evaluate its current Set dependency");
    std::string text,error;Workspace loaded;require(serialize(workspace,text,error)&&deserialize(text,loaded,error)&&textParameter(node(loaded.graphs[2],3),"name")=="gain","Named values were not portable");
    graph.links[0].fromNode=3;require(!validate(workspace,error)&&error.find("cycle")!=std::string::npos,"A cycle hidden behind Get/Set was accepted");graph.links[0].fromNode=1;
    auto duplicate=defaultNode(NodeKind::SetFloat,9);duplicate.textParameters["name"]="gain";graph.nodes.push_back(duplicate);require(!validate(workspace,error),"Two writers for the same variable were accepted");graph.nodes.pop_back();
    node(graph,3).textParameters["name"]="missing";publish(service,workspace);require(!service.evaluate(fixture.context).recoil,"An unresolved Get used stale variable data");
    workspace=emptyWorkspace();auto& selected=workspace.graphs[2];
    selected.nodes={defaultNode(NodeKind::Boolean,1),defaultNode(NodeKind::Float,2),defaultNode(NodeKind::Divide,3),defaultNode(NodeKind::SelectFloat,4),defaultNode(NodeKind::RecoilOutput,5)};
    node(selected,2).value=7;selected.links={{1,0,4,0},{2,0,4,1},{3,0,4,2},{1,0,5,0},{4,0,5,1}};publish(service,workspace);
    require(service.evaluate(fixture.context).recoilStrength==7&&!sampleNode(service.telemetry(),2,3).active,"Select evaluated an invalid unchosen branch");
}
void testAllInputGeometryAndDeepGraphs()
{
    auto workspace=defaults();const auto& render=node(workspace.graphs[3],13);const int pin=optionPin(NodeKind::OverlayOutput,"game_overlay_max_fps");
    require(Layout::inputVisible(render,pin)&&Layout::inputCount(render)==definition(render.kind).inputs.size(),"A default node hides some option inputs");
    Service service;publish(service,workspace);std::string text,error;Workspace loaded;
    require(serialize(workspace,text,error)&&deserialize(text,loaded,error)&&Layout::inputCount(node(loaded.graphs[3],13))==Layout::inputCount(render),"Export/import lost some visible inputs");
    workspace=emptyWorkspace();auto& deep=workspace.graphs[2];constant(deep,1,1);constant(deep,2,1,true);
    for(int id=3;id<=127;++id){deep.nodes.push_back(defaultNode(NodeKind::Add,id));deep.links.push_back({id==3?1:id-1,0,id,0});}
    deep.nodes.push_back(defaultNode(NodeKind::RecoilOutput,128));deep.links.push_back({2,0,128,0});deep.links.push_back({127,0,128,1});
    Fixture fixture;publish(service,workspace);require(service.evaluate(fixture.context).recoilStrength==1,"A deep graph exhausted evaluator stack storage after expanding the port limit");
}
void testWiredKeyOverridesNoneFallback()
{
    auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
    graph.nodes={defaultNode(NodeKind::Integer,1),defaultNode(NodeKind::KeyHeld,2),defaultNode(NodeKind::Float,3),defaultNode(NodeKind::RecoilOutput,4)};
    node(graph,1).value=65;node(graph,2).parameters["virtual_key"]=0;node(graph,3).value=2;
    graph.links={{1,0,2,optionPin(NodeKind::KeyHeld,"virtual_key")},{2,0,4,0},{3,0,4,1}};
    Service service;Fixture fixture;fixture.context.keyDown=[](int key){return key==65;};publish(service,workspace);
    require(service.evaluate(fixture.context).recoilStrength==2&&sampleNode(service.telemetry(),2,1).active,"A wired key setting was pruned using its None fallback");
}
void testLocalScalarInputsExecuteAndFollowWires()
{
    Service service;Fixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[2];
    graph.nodes={defaultNode(NodeKind::Add,1),defaultNode(NodeKind::RecoilOutput,2),defaultNode(NodeKind::Float,3)};
    node(graph,1).inputValues={{0,2.25},{1,1.5}};node(graph,2).inputValues={{0,1}};node(graph,3).value=8;
    graph.links={{1,0,2,1}};publish(service,workspace);
    require(service.evaluate(fixture.context).recoilStrength==3.75,"Unconnected scalar literals did not execute");
    const auto revision=service.revision();node(graph,1).inputValues[0]=3;publish(service,workspace);
    require(service.revision()>revision&&service.evaluate(fixture.context).recoilStrength==4.5,"Editing a local input did not immediately change execution");
    graph.links.push_back({3,0,1,0});publish(service,workspace);
    require(service.evaluate(fixture.context).recoilStrength==9.5,"A local default overrode a connected wire");
    graph.links.pop_back();node(graph,1).bypassed=true;publish(service,workspace);
    require(service.evaluate(fixture.context).recoilStrength==3,"Bypass ignored the configured input literal");
    node(graph,1).bypassed=false;node(graph,2).inputValues[0]=0;publish(service,workspace);
    require(!service.evaluate(fixture.context).recoil&&!sampleNode(service.telemetry(),2,1).active,"A false local Enable still evaluated its data branch");

    node(graph,1)=defaultNode(NodeKind::SelectFloat,1);node(graph,1).inputValues={{0,1},{1,6.5}};
    node(graph,2).inputValues[0]=1;node(graph,3)=defaultNode(NodeKind::Divide,3);graph.links.push_back({3,0,1,2});publish(service,workspace);
    require(service.evaluate(fixture.context).recoilStrength==6.5&&!sampleNode(service.telemetry(),2,3).active,"Select lost its local condition/value or evaluated an unchosen branch");
}
void testLocalInputTypesAndPersistence()
{
    Service service;VisionFixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[3];
    graph.nodes={defaultNode(NodeKind::ConcatString,1),defaultNode(NodeKind::CaptureSource,2),defaultNode(NodeKind::CollectDataset,3)};
    node(graph,1).inputTexts={{0,u8"datasets/пример"},{1,"/session"}};node(graph,3).parameters["enabled"]=1;
    graph.links={{2,0,3,1},{1,0,3,optionPin(NodeKind::CollectDataset,"collect_output_dir")}};
    std::string received;fixture.context.collect=[&](int,const FrameStream&,const DetectionStream&,const Node& settings){received=textParameter(settings,"collect_output_dir");return true;};
    publish(service,workspace);service.evaluateVision(fixture.context);
    require(received==u8"datasets/пример/session","Local text inputs did not reach a bound native setting");
    auto& ints=workspace.graphs[2];ints.nodes={defaultNode(NodeKind::IntegerAdd,1),defaultNode(NodeKind::RecoilOutput,2)};
    node(ints,1).inputValues={{0,7},{1,-2}};node(ints,2).inputValues={{0,1}};ints.links={{1,0,2,1}};
    publish(service,workspace);Fixture control;require(service.evaluate(control.context).recoilStrength==5,"Int input literals lost numeric promotion");
    std::string text,error;Workspace restored;require(serialize(workspace,text,error)&&deserialize(text,restored,error),"Literal input defaults did not roundtrip");
    require(node(restored.graphs[3],1).inputTexts==node(graph,1).inputTexts&&node(restored.graphs[2],1).inputValues==node(ints,1).inputValues,"Export lost local input values");
    node(ints,1).inputValues[0]=0.5;require(!validate(workspace,error),"A fractional Int input default was accepted");node(ints,1).inputValues[0]=7;
    node(ints,2).inputValues[0]=2;require(!validate(workspace,error),"A non-boolean Bool input default was accepted");node(ints,2).inputValues[0]=1;
    node(ints,1).inputValues[MaxPins]=1;require(!validate(workspace,error),"An out-of-range input default was accepted");node(ints,1).inputValues.erase(MaxPins);
    node(graph,2).inputValues[0]=1;require(!validate(workspace,error),"A scalar literal was accepted for a Monitor input");node(graph,2).inputValues.clear();
    node(graph,1).inputTexts[0]=std::string(4097,'x');require(!validate(workspace,error),"An oversized local text input was accepted");
}
void testUnavailableEnableDoesNotInventAnError()
{
    const NodeKind outputs[]={NodeKind::AimOutput,NodeKind::AutoShootOutput,NodeKind::RecoilOutput,
        NodeKind::VisionOutput,NodeKind::CollectDataset,NodeKind::RenderOverlay,NodeKind::ScreenshotOutput};
    for(const auto kind:outputs)
    {
        const int index=kind==NodeKind::AimOutput?0:kind==NodeKind::AutoShootOutput?1:kind==NodeKind::RecoilOutput?2:3;
        Service service;Fixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[index];
        graph.nodes={defaultNode(NodeKind::ScopeCheck,1),defaultNode(kind,2)};graph.links={{1,0,2,0}};publish(service,workspace);
        const auto evaluate=[&]{return index==3?service.evaluateVision(fixture.context):service.evaluate(fixture.context);};
        const auto actions=evaluate();const auto telemetry=service.telemetry();const auto& output=sampleNode(telemetry,index,2);
        require(!output.value.valid&&output.value.error.empty()&&!output.active,"An unavailable Enable without a target was mislabeled as an error");
        require(output.readInputs==1&&!actions.aim&&!actions.autoShoot&&!actions.recoil&&!actions.visionDetections&&
            actions.activeCollectorKeys.empty()&&actions.activeRenderKeys.empty()&&actions.activeScreenshotKeys.empty(),"An unavailable Enable executed an output or its data branch");
        node(graph,1)=defaultNode(NodeKind::GetBoolean,1);node(graph,1).textParameters["name"]="missing_enable";publish(service,workspace);evaluate();
        const auto failed=service.telemetry();
        require(sampleNode(failed,index,2).value.error=="No matching Set for missing_enable","A real Enable error was hidden or replaced");
        node(graph,1)=defaultNode(NodeKind::Boolean,1);node(graph,1).value=0;publish(service,workspace);evaluate();
        const auto recovered=service.telemetry();const auto& inactive=sampleNode(recovered,index,2);
        require(inactive.value.valid&&!inactive.value.boolean&&inactive.value.error.empty(),"An inactive output retained the prior Enable error");
    }
    Service service;VisionFixture fixture;auto workspace=emptyWorkspace();auto& vision=workspace.graphs[3];
    vision.nodes={defaultNode(NodeKind::ScopeCheck,1),defaultNode(NodeKind::CaptureSource,2),defaultNode(NodeKind::CollectDataset,3)};
    node(vision,3).parameters["enabled"]=1;vision.links={{1,0,2,optionPin(NodeKind::CaptureSource,"enabled")},{2,0,3,1}};
    publish(service,workspace);service.evaluateVision(fixture.context);const auto waiting=service.telemetry();
    require(sampleNode(waiting,3,2).value.error.empty()&&!sampleNode(waiting,3,2).value.valid&&fixture.captures==0&&fixture.collections==0,
        "A provider with unavailable activation reported a false error or called its native service");
}
void testPreviewIsVisibleOnlyLazyAndThrottled()
{
    Service service;VisionFixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[3];
    graph.nodes={defaultNode(NodeKind::CaptureSource,1),defaultNode(NodeKind::DepthInference,2),defaultNode(NodeKind::DepthToMask,3),defaultNode(NodeKind::ImagePreview,4)};
    node(graph,2).parameters["enabled"]=1;node(graph,3).parameters["enabled"]=1;node(graph,4).parameters["draw_boxes"]=0;
    graph.links={{1,0,2,0},{2,0,3,0},{1,0,4,1},{2,0,4,2},{3,0,4,3}};
    int conversions=0;ValueType seen=ValueType::Boolean;
    fixture.context.preview=[&](const Value& value,const DetectionStream&,const Node&){
        ++conversions;seen=value.type;auto image=std::make_shared<PreviewImage>();image->width=4;image->height=2;image->rgba.resize(32,255);
        image->generation=value.mask?value.mask->generation:value.frame->generation;image->version=value.mask?value.mask->version:value.frame->version;return Preview(image);
    };
    publish(service,workspace);service.evaluateVision(fixture.context);
    require(fixture.captures==0&&conversions==0&&service.providerNodes(NodeKind::CaptureSource).empty(),"An invisible preview activated capture");
    require(service.evaluateConfiguration(fixture.context).empty(),"An invisible preview configured native providers");
    const auto revision=service.revision();service.setPreviewNodes({4});
    const auto configured=service.evaluateConfiguration(fixture.context);
    require(configured.size()==1&&configured[0].kind==NodeKind::CaptureSource&&conversions==0&&fixture.captures==0,"Preview preflight did not configure its selected source without reading pixels");
    service.evaluateVision(fixture.context);
    require(conversions==1&&fixture.captures==1&&fixture.depths==0&&fixture.masks==0&&seen==ValueType::Frame,"Auto preview executed an unselected depth/mask branch");
    for(int i=0;i<20;++i)service.evaluateVision(fixture.context);
    require(conversions==1&&fixture.captures==1,"Preview FPS limiting ran conversion or upstream providers on every frame");
    std::this_thread::sleep_for(std::chrono::milliseconds(75));service.evaluateVision(fixture.context);
    require(conversions==1,"An unchanged image was converted again");
    for(int mode:{2,3})
    {
        node(graph,4).parameters["mode"]=mode;publish(service,workspace);
        const int before=conversions;const auto providers=service.evaluateConfiguration(fixture.context);
        require(std::any_of(providers.begin(),providers.end(),[](const Node& n){return n.kind==NodeKind::CaptureSource;})&&conversions==before,"A standalone depth/mask preview did not configure its upstream capture without converting pixels");
        service.evaluateVision(fixture.context);
        require(seen==(mode==2?ValueType::Depth:ValueType::Mask),"Changing preview mode did not select its typed input immediately");
    }
    auto telemetry=service.telemetry();require(sampleNode(telemetry,3,4).value.preview!=nullptr,"Preview pixels did not reach editor telemetry");
    const int captures=fixture.captures;service.setPreviewNodes({});service.evaluateVision(fixture.context);
    telemetry=service.telemetry();require(!sampleNode(telemetry,3,4).value.preview&&fixture.captures==captures,"A hidden preview retained pixels or ran its provider");
    require(service.revision()>revision,"Mode settings did not change execution");
    const auto stable=service.revision();service.setPreviewNodes({4});service.setTelemetryEnabled(false);service.evaluateVision(fixture.context);
    require(!service.previewNodes()&&fixture.captures==captures&&service.revision()==stable,"Hiding monitoring restarted execution or left previews active");
    std::string text,error;Workspace loaded;require(serialize(workspace,text,error)&&deserialize(text,loaded,error)&&node(loaded.graphs[3],4).kind==NodeKind::ImagePreview,"Preview nodes did not survive export/import");
}
void testPreviewCountAndCancellationAreBounded()
{
    Service service;VisionFixture fixture;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[3];
    graph.nodes={defaultNode(NodeKind::CaptureSource,1)};std::vector<int> requests;
    for(int id=2;id<=6;++id){graph.nodes.push_back(defaultNode(NodeKind::ImagePreview,id));graph.links.push_back({1,0,id,1});requests.push_back(id);}
    int converted=0;fixture.context.preview=[&](const Value&,const DetectionStream&,const Node&){++converted;auto image=std::make_shared<PreviewImage>();image->width=image->height=1;image->rgba.resize(4,255);return Preview(image);};
    publish(service,workspace);service.setPreviewNodes(requests);service.evaluateVision(fixture.context);
    require(converted==MaxLivePreviews,"Visible previews exceeded the conversion budget");
    service.setPreviewNodes({2});const auto capture=fixture.context.captureFrame;
    fixture.context.captureFrame=[&](int key,const Node& node){service.setPreviewNodes({});return capture(key,node);};
    std::this_thread::sleep_for(std::chrono::milliseconds(75));service.evaluateVision(fixture.context);
    require(converted==MaxLivePreviews,"A preview converted pixels after it was hidden during provider evaluation");
}
void testNodeBindings()
{
    testEveryOptionHasATypedInput();testBoundOptionsReachNativeCallbacks();testWideOptionMasksAndTextOutputs();
    testMonitorsConfigureCaptureBeforeAFrameExists();testNamedValuesAndLazySelection();testAllInputGeometryAndDeepGraphs();testWiredKeyOverridesNoneFallback();
    testLocalScalarInputsExecuteAndFollowWires();testLocalInputTypesAndPersistence();
    testUnavailableEnableDoesNotInventAnError();
    testPreviewIsVisibleOnlyLazyAndThrottled();testPreviewCountAndCancellationAreBounded();
}
}
