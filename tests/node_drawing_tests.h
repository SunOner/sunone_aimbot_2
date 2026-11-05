#pragma once
#include "overlay/drawing_renderer.h"

namespace
{
void testPredictionDrawingExampleAndRetainedMotion()
{
    auto workspace=emptyWorkspace();workspace.graphs[0]=predictionDrawingExample();
    Fixture fixture;Service service;Drawing drawing;int renders=0;
    fixture.context.draw=[&](int,const Drawing& value,Point,const Node&){drawing=value;++renders;return true;};
    publish(service,workspace);auto actions=service.evaluate(fixture.context);
    require(renders==1&&drawing&&drawing->commands.size()==6&&!actions.aim&&!actions.autoShoot&&!actions.recoil,"Prediction example did not produce a visual-only drawing");
    const auto& predicted=drawing->commands[0];
    require(predicted.points&&predicted.points->size()==16&&predicted.points->front().x==20&&predicted.points->back().x==22.5&&predicted.points->back().y==40,"Example prediction did not use calculated target-center velocity");
    require(drawing->commands[2].from.x==22.5&&drawing->commands[4].to.x==20&&drawing->commands[5].from.y==22,"Example marker, bounding box or label used the wrong geometry");
    const int predictions=fixture.predictions;
    const auto cached=service.evaluate(fixture.context);
    require(cached.activePredictionKeys==actions.activePredictionKeys&&cached.activeSelectionKeys==actions.activeSelectionKeys&&fixture.predictions==predictions&&renders==1,"Throttling discarded upstream motion history or recalculated geometry");
    fixture.setDetections({});require(service.evaluate(fixture.context).activeRenderKeys.empty(),"A lost target kept its prediction drawing alive");
    for(std::size_t a=0;a<workspace.graphs[0].nodes.size();++a)for(std::size_t b=a+1;b<workspace.graphs[0].nodes.size();++b)
    {
        const auto& x=workspace.graphs[0].nodes[a];const auto& y=workspace.graphs[0].nodes[b];
        require(x.x+Layout::width(x)<=y.x||y.x+Layout::width(y)<=x.x||x.y+Layout::height(x,true)<=y.y||y.y+Layout::height(y,true)<=x.y,"Prediction example contains overlapping cards");
    }
    // Upgrade only the untouched previous layout, preserving local tuning.
    auto legacy=workspace;
    auto& old=legacy.graphs[0];for(auto& n:old.nodes)n.width=n.height=0;for(auto& link:old.links)link.ties.clear();old.ties.clear();
    old.groups={{1,"01 Target geometry",0,0,0,0,0x405ac8ffu,{1,2,3,4,5,6,7,17,28}},
        {2,"02 Motion and prediction",0,0,0,0,0x40b099ffu,{8,9,10,11,12,13,14,15,16}},
        {3,"03 Drawing",0,0,0,0,0x4070d9a0u,{18,19,20,21,22,23,24,25,26,27,29}}};
    Layout::arrange(old,4);node(old,13).value=0.75;node(old,18).parameters["draw_r"]=200;old.workflowName="Custom prediction";
    std::string document,error;Workspace restored;
    require(serialize(legacy,document,error)&&deserialize(document,restored,error),error.c_str());
    require(restored.graphs[0].groups.size()==7&&restored.graphs[0].ties.size()==22&&node(restored.graphs[0],13).value==0.75&&
        parameter(node(restored.graphs[0],18),"draw_r")==200&&restored.graphs[0].workflowName==old.workflowName,"Layout migration changed prediction tuning or left the shipped arrangement behind");
    node(old,1).x+=32;
    require(serialize(legacy,document,error)&&deserialize(document,restored,error),error.c_str());
    require(restored.graphs[0].groups.size()==3&&restored.graphs[0].ties.empty()&&node(restored.graphs[0],1).x==node(old,1).x,"Layout migration overwrote a user's arrangement");
}
int drawingOption(NodeKind kind,const char* key)
{
    const auto& inputs=definition(kind).inputs;
    for(std::size_t i=0;i<inputs.size();++i)if(inputs[i].defaultParameter&&std::string(inputs[i].defaultParameter)==key)return static_cast<int>(i);
    throw std::runtime_error("Missing drawing option");
}
Workspace drawingWorkspace()
{
    using K=NodeKind;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[0];
    graph.nodes={defaultNode(K::MakePoint,1),defaultNode(K::MakePoint,2),defaultNode(K::PredictPath,3),defaultNode(K::DrawingStyle,4),
        defaultNode(K::DrawPolyline,5),defaultNode(K::ProjectPoint,6),defaultNode(K::DrawPoint,7),defaultNode(K::DrawLine,8),defaultNode(K::DrawBox,9),defaultNode(K::DrawTextPrimitive,10),defaultNode(K::RenderDrawing,11)};
    node(graph,1).inputValues={{0,10},{1,20}};node(graph,2).inputValues={{0,30},{1,-10}};
    node(graph,3).parameters["seconds"]=0.5;node(graph,3).parameters["samples"]=3;node(graph,6).parameters["seconds"]=0.5;
    node(graph,4).parameters["draw_r"]=230;node(graph,4).parameters["draw_g"]=100;node(graph,4).parameters["draw_b"]=40;node(graph,4).parameters["draw_width"]=2;
    node(graph,10).textParameters["text"]=u8"Прогноз";node(graph,11).parameters["game_overlay_max_fps"]=240;
    graph.links={{1,0,3,0},{2,0,3,1},{3,0,5,1},{4,0,5,2},{1,0,6,0},{2,0,6,1},{5,0,7,0},{6,0,7,1},{4,0,7,2},
        {7,0,8,0},{1,0,8,1},{6,0,8,2},{4,0,8,3},{8,0,9,0},{1,0,9,1},{2,0,9,2},{4,0,9,3},
        {9,0,10,0},{6,0,10,1},{4,0,10,3},{10,0,11,1}};
    return workspace;
}
void testDrawingPredictionGeometryCachingAndGates()
{
    Service service;auto workspace=drawingWorkspace();Context context;context.center={160,160};Drawing drawing;int renders=0;
    context.draw=[&](int key,const Drawing& value,Point size,const Node&){require(key==11&&size.x==320&&size.y==320,"Drawing used the wrong output identity or capture dimensions");drawing=value;++renders;return true;};
    publish(service,workspace);auto actions=service.evaluate(context);
    require(renders==1&&actions.activeRenderKeys==std::vector<int>{11}&&drawing&&drawing->commands.size()==5,"Drawing primitives did not compose into one control-workflow output");
    const auto& path=drawing->commands[0];require(path.points&&path.points->size()==3&&(*path.points)[0].x==10&&(*path.points)[1].x==17.5&&(*path.points)[2].x==25&&(*path.points)[2].y==15,"Predicted samples do not follow position + velocity * time");
    require(drawing->commands[1].from.x==25&&drawing->commands[1].from.y==15&&drawing->commands[4].text&&*drawing->commands[4].text==u8"Прогноз","Computed point or UTF-8 text did not reach the drawing commands");
    const auto first=drawing;service.evaluate(context);require(renders==1,"Drawing ignored its output FPS limit");
    std::this_thread::sleep_for(std::chrono::milliseconds(6));service.evaluate(context);
    require(renders==2&&drawing==first,"Unchanged drawing geometry was rebuilt rather than reused");
    node(workspace.graphs[0],7).parameters["enabled"]=0;
    workspace.graphs[0].links.erase(std::remove_if(workspace.graphs[0].links.begin(),workspace.graphs[0].links.end(),[](const Link& link){return link.toNode==7&&link.toPort==1;}),workspace.graphs[0].links.end());
    publish(service,workspace);service.evaluate(context);require(drawing->commands.size()==4,"A disabled primitive evaluated missing geometry or erased the preceding drawing");
    node(workspace.graphs[0],11).parameters["enabled"]=0;publish(service,workspace);const int before=renders;
    require(service.evaluate(context).activeRenderKeys.empty()&&renders==before,"A disabled render output executed its drawing branch");
    node(workspace.graphs[0],11).parameters["enabled"]=1;publish(service,workspace);context.inputCaptured=true;
    require(service.evaluate(context).activeRenderKeys==std::vector<int>{11}&&renders==before+1,"Opening the editor stopped a visual-only drawing output");
    context.inputCaptured=false;workspace.graphs[0].kind=GraphKind::Vision;publish(service,workspace);
    require(service.evaluateVision(context).activeRenderKeys==std::vector<int>{11},"Drawing is unavailable in a Vision workflow");
}
void testDrawingListsLimitsNamedValuesAndPersistence()
{
    using K=NodeKind;auto workspace=drawingWorkspace();auto& graph=workspace.graphs[0];
    graph.nodes.push_back(defaultNode(K::SetDrawing,12));graph.nodes.push_back(defaultNode(K::GetDrawing,13));
    graph.nodes.push_back(defaultNode(K::RerouteDrawing,14));
    graph.links.erase(std::remove_if(graph.links.begin(),graph.links.end(),[](const Link& link){return link.toNode==11;}),graph.links.end());
    graph.links.insert(graph.links.end(),{{10,0,12,0},{13,0,14,0},{14,0,11,1}});
    Context context;context.center={160,160};int renders=0;context.draw=[&](int,const Drawing& drawing,Point,const Node&){++renders;return drawing&&drawing->commands.size()==5;};
    Service service;publish(service,workspace);require(service.evaluate(context).activeRenderKeys.size()==1,"Drawing Get/Set or reroute lost its value");
    std::string text,error;Workspace restored;require(serialize(workspace,text,error)&&deserialize(text,restored,error),error.c_str());
    Service imported;publish(imported,restored);require(imported.evaluate(context).activeRenderKeys.size()==1,"Drawing graph changed after export/import");
    node(graph,3).parameters["samples"]=256;graph.nodes.push_back(defaultNode(K::AppendPoint,15));
    for(auto& link:graph.links)if(link.toNode==5&&link.toPort==1)link.fromNode=15;
    graph.links.push_back({3,0,15,0});graph.links.push_back({1,0,15,1});publish(service,workspace);const auto before=renders;
    require(service.evaluate(context).activeRenderKeys.empty()&&renders==before,"An overflowing point list still rendered");
    const auto telemetry=service.telemetry();const auto failed=std::find_if(telemetry.graphs[0].begin(),telemetry.graphs[0].end(),[](const NodeValue& value){return value.nodeId==15;});
    require(failed!=telemetry.graphs[0].end()&&failed->value.error.find("256")!=std::string::npos,"Point-list overflow did not expose a useful node error");
    auto invalid=workspace;for(auto& link:invalid.graphs[0].links)if(link.toNode==5&&link.toPort==1)link.fromNode=1;
    require(!validate(invalid,error),"A Point was connected directly to a Points input");
}
void testPointTrailSamplingResetAndKey()
{
    using K=NodeKind;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[0];
    graph.nodes={defaultNode(K::Center,1),defaultNode(K::PointTrail,2),defaultNode(K::DrawPolyline,3),defaultNode(K::RenderDrawing,4),defaultNode(K::ShootHeld,5),defaultNode(K::CaptureFps,6),defaultNode(K::ToInteger,7)};
    graph.links={{1,0,2,0},{5,0,2,1},{6,0,7,0},{7,0,2,2},{2,0,3,1},{3,0,4,1}};
    node(graph,2).parameters["samples"]=3;node(graph,2).parameters["sample_fps"]=30;node(graph,2).parameters["lifetime"]=0.2;node(graph,4).parameters["game_overlay_max_fps"]=240;
    Context context;context.center={10,10};context.captureFps=1;PointList trail;
    context.draw=[&](int,const Drawing& drawing,Point,const Node&){trail=drawing->commands[0].points;return true;};Service service;publish(service,workspace);
    const auto tick=[&](double time,double x){context.elapsedSeconds=time;context.center.x=x;std::this_thread::sleep_for(std::chrono::milliseconds(6));service.evaluate(context);};
    tick(0,10);const auto first=trail;tick(0.01,20);require(trail==first&&trail->size()==1,"Trail sampled at the control tick rate");
    tick(0.04,20);tick(0.08,30);tick(0.12,40);require(trail->size()==3&&trail->front().x==20&&trail->back().x==40,"Trail did not retain the bounded recent history");
    context.captureFps=2;tick(0.16,50);require(trail->size()==1&&trail->front().x==50,"A new target key kept the previous target's trail");
    tick(0.20,60);context.shootHeld=true;tick(0.24,70);require(trail->size()==1&&trail->front().x==70,"Reset did not clear trail history");
    context.shootHeld=false;tick(0.50,80);require(trail->size()==1&&trail->front().x==80,"Expired trail samples remained visible");
}
struct DrawingCanvas
{
    struct Call{std::string kind;std::vector<double> coordinates;OverlayColor color;std::wstring text;};
    std::vector<Call> calls;
    void AddLine(OverlayLine v,OverlayColor c,float width){calls.push_back({"line",{v.x1,v.y1,v.x2,v.y2,width},c,{}});}
    void AddRect(OverlayRect v,OverlayColor c,float width){calls.push_back({"box",{v.x,v.y,v.w,v.h,width},c,{}});}
    void FillRect(OverlayRect v,OverlayColor c){calls.push_back({"fill_box",{v.x,v.y,v.w,v.h},c,{}});}
    void AddCircle(OverlayCircle v,OverlayColor c,float width){calls.push_back({"circle",{v.cx,v.cy,v.r,width},c,{}});}
    void FillCircle(OverlayCircle v,OverlayColor c){calls.push_back({"point",{v.cx,v.cy,v.r},c,{}});}
    void AddText(float x,float y,const std::wstring& text,float size,OverlayColor c){calls.push_back({"text",{x,y,size},c,text});}
};
void testDrawingNativeProjectionStyleAndBudget()
{
    auto workspace=drawingWorkspace();Context context;context.center={160,160};Drawing drawing;
    context.draw=[&](int,const Drawing& value,Point,const Node&){drawing=value;return true;};Service service;publish(service,workspace);service.evaluate(context);
    DrawingCanvas canvas;std::size_t budget=100;
    GraphDrawing::render(canvas,*drawing,GraphDrawing::viewport({320,320},1920,1080,0),budget);
    require(canvas.calls.size()==6&&canvas.calls[0].coordinates[0]==810&&canvas.calls[0].coordinates[1]==400,"Capture coordinates were not centered on the selected monitor");
    require(canvas.calls[2].kind=="point"&&canvas.calls[2].coordinates[0]==825&&canvas.calls[2].coordinates[1]==395&&canvas.calls[2].color==ARGB(255,230,100,40),"Point projection or wired drawing style changed");
    require(canvas.calls.back().text==L"Прогноз"&&canvas.calls[4].coordinates[1]==390&&canvas.calls[4].coordinates[3]==10,"Text decoding or negative box dimensions are incorrect");
    DrawingCanvas limited;budget=2;GraphDrawing::render(limited,*drawing,{},budget);require(limited.calls.size()==2&&budget==0,"Overlay primitive budget was ignored");
    const auto normalized=GraphDrawing::viewport({1,1},1920,1080,2).project({0.5,0.5});require(normalized.x==960&&normalized.y==540,"Normalized screen coordinates are incorrect");
    const auto screen=GraphDrawing::viewport({320,320},1920,1080,1).project({100,200});require(screen.x==100&&screen.y==200,"Screen-pixel drawing was offset into the capture frame");
}
}
