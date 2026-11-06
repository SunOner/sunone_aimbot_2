#pragma once
namespace
{
void testPreviewDiagnosticsKeepValuesBetweenRefreshes()
{
    using K=NodeKind;Service service;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[0];
    graph.nodes={defaultNode(K::Float,1),defaultNode(K::Float,2),defaultNode(K::Add,3),defaultNode(K::ImagePreview,4)};
    node(graph,1).value=5;node(graph,2).value=7;node(graph,4).parameters["max_fps"]=1;
    graph.links={{1,0,3,0},{2,0,3,1},{3,0,4,1}};publish(service,workspace);service.setPreviewNodes({previewKey(0,4)});
    service.evaluate(Context{});const auto first=sampleNode(service.telemetry(),0,3);
    require(first.value.valid&&first.value.number==12&&first.evaluated,"Initial Add diagnostics are incorrect");
    std::this_thread::sleep_for(std::chrono::milliseconds(35));service.evaluate(Context{});
    const auto cached=sampleNode(service.telemetry(),0,3);
    require(cached.value.valid&&cached.value.number==12&&!cached.evaluated&&cached.evaluatedAt==first.evaluatedAt,
        "A throttled Preview replaced the last upstream value with an artificial invalid zero or recomputed it");
    node(graph,1).value=9;publish(service,workspace);service.setPreviewNodes({previewKey(0,4)});service.evaluate(Context{});
    const auto changed=sampleNode(service.telemetry(),0,3);
    require(changed.value.valid&&changed.value.number==16&&changed.evaluatedAt>first.evaluatedAt,"An edit reused diagnostics from an earlier program revision");
    service.setPreviewNodes({});std::this_thread::sleep_for(std::chrono::milliseconds(1035));service.evaluate(Context{});
    const auto expired=sampleNode(service.telemetry(),0,3);
    require(!expired.value.valid&&expired.evaluatedAt.time_since_epoch().count()==0,"Unused diagnostic values outlived the bounded cache");
}
void testPreviewSamplingKeepsUpstreamRuntimeState()
{
    using K=NodeKind;Fixture fixture;Service service;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[0];
    graph.nodes={defaultNode(K::Detections,1),defaultNode(K::ChooseTarget,2),defaultNode(K::TargetPosition,3),defaultNode(K::MotionEstimate,4),defaultNode(K::MotionVelocity,5),defaultNode(K::ImagePreview,6)};
    graph.links={{1,0,2,0},{2,0,3,0},{3,0,4,0},{4,0,5,0},{5,0,6,1}};publish(service,workspace);service.setPreviewNodes({previewKey(0,6)});
    const auto first=service.evaluate(fixture.context);const int updates=fixture.predictions;
    const auto cached=service.evaluate(fixture.context);
    require(first.activePredictionKeys==std::vector<int>{4}&&first.activeSelectionKeys==std::vector<int>{2},"Preview did not evaluate its motion/target branch");
    require(cached.activePredictionKeys==first.activePredictionKeys&&cached.activeSelectionKeys==first.activeSelectionKeys&&fixture.predictions==updates,"Preview FPS throttling discarded tracker/filter state between samples");
    service.setPreviewNodes({});const auto hidden=service.evaluate(fixture.context);
    require(hidden.activePredictionKeys.empty()&&hidden.activeSelectionKeys.empty(),"Hidden preview retained an unused motion branch");

    workspace=emptyWorkspace();auto& vision=workspace.graphs[3];
    vision.nodes={defaultNode(K::CurrentFrame,1),defaultNode(K::DepthInference,2),defaultNode(K::ImagePreview,3)};node(vision,2).parameters["enabled"]=1;
    vision.links={{1,0,2,0},{2,0,3,2}};publish(service,workspace);service.setPreviewNodes({previewKey(3,3)});
    VisionFixture image;image.context.preview=[](const Value& value,const DetectionStream&,const Node&){auto p=std::make_shared<PreviewImage>();p->sourceType=value.type;p->width=p->height=1;p->generation=value.frame->generation;p->rgba.resize(4,255);return Preview(p);};
    const auto depth=service.evaluateVision(image.context);const int calls=image.depths;const auto cachedDepth=service.evaluateVision(image.context);
    require(!depth.activeDepthKeys.empty()&&depth.activeDepthKeys==cachedDepth.activeDepthKeys&&image.depths==calls,"Cached depth preview dropped its provider state or ran inference again");
    node(vision,3).disabled=true;publish(service,workspace);require(service.evaluateVision(image.context).activeDepthKeys.empty(),"Disabled preview kept its depth provider alive");
}
void testPointPreviewWaitsForTargetAndShowsErrorCause()
{
    using K=NodeKind;Fixture fixture;Service service;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[0];
    graph.nodes={defaultNode(K::Detections,1),defaultNode(K::ChooseTarget,2),defaultNode(K::TargetPosition,3),defaultNode(K::MotionEstimate,4),
        defaultNode(K::MotionPosition,5),defaultNode(K::MotionVelocity,6),defaultNode(K::MotionDamping,7),defaultNode(K::MotionReady,8),defaultNode(K::ProjectPoint,9),defaultNode(K::ImagePreview,10)};
    node(graph,9).parameters["seconds"]=0.25;
    graph.links={{1,0,2,0},{2,0,3,0},{3,0,4,0},{4,0,5,0},{4,0,6,0},{4,0,7,0},{4,0,8,0},
        {5,0,9,0},{6,0,9,1},{7,0,9,3},{8,0,9,4},{9,0,10,1}};
    publish(service,workspace);service.setPreviewNodes({previewKey(0,10)});fixture.context.inputCaptured=true;fixture.setDetections({});service.evaluate(fixture.context);
    for(int id:{7,8,9,10})require(!sampleNode(service.telemetry(),0,id).value.valid&&sampleNode(service.telemetry(),0,id).value.error.empty(),"An absent target became a setting or preview error");
    fixture.setDetections({{10,20,20,40,0,0.9}});std::this_thread::sleep_for(std::chrono::milliseconds(35));service.evaluate(fixture.context);
    const auto point=sampleNode(service.telemetry(),0,10).value.preview;
    require(point&&point->width==0&&point->text.find("12.5")!=std::string::npos&&point->text.find("20")!=std::string::npos,"Project point did not reach the text preview while the editor held input");
    const auto estimate=fixture.context.estimateMotion;fixture.context.estimateMotion=[](int,Point,const Node&,bool)->MotionData{throw std::runtime_error("Velocity provider lost");};
    std::this_thread::sleep_for(std::chrono::milliseconds(75));service.evaluate(fixture.context);
    const auto failed=sampleNode(service.telemetry(),0,10).value;
    require(!failed.valid&&failed.error=="Velocity provider lost"&&failed.preview&&failed.preview->text.find(failed.error)!=std::string::npos&&failed.preview->rgba.empty(),"Preview hid the upstream error cause or downloaded pixels for it");
    const auto errorMessage=failed.preview;std::this_thread::sleep_for(std::chrono::milliseconds(35));service.evaluate(fixture.context);
    const auto cached=sampleNode(service.telemetry(),0,10).value;
    require(!cached.valid&&cached.error==failed.error&&cached.preview==errorMessage,"A throttled error preview became valid or rebuilt its text");
    fixture.context.estimateMotion=estimate;std::this_thread::sleep_for(std::chrono::milliseconds(75));service.evaluate(fixture.context);
    const auto recovered=sampleNode(service.telemetry(),0,10).value;
    require(recovered.valid&&recovered.error.empty()&&recovered.preview&&recovered.preview->text==point->text,"Unchanged point data retained a stale error preview after recovery");
    fixture.setDetections({});std::this_thread::sleep_for(std::chrono::milliseconds(75));service.evaluate(fixture.context);
    const auto waiting=sampleNode(service.telemetry(),0,10).value;
    require(!waiting.valid&&waiting.error.empty()&&!waiting.preview,"A lost target retained an error or the last valid coordinates");
}
void testUniversalPreviewAcceptsValuesInEveryWorkflow()
{
    using K=NodeKind;using T=ValueType;
    for(const auto& def:definitions())for(const auto& output:def.outputs)require(canConnectTypes(output.type,T::Any),"Universal preview rejected a typed output");
    require(!canConnectTypes(T::Any,T::Float)&&!canConnectTypes(T::Float,T::Integer),"Universal input changed ordinary type-safety rules");
    for(int g=0;g<4;++g)
    {
        Service service;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[g];
        auto source=defaultNode(K::String,1);source.textParameters["value"]="Полный текст\n0.005555555555555556";
        graph.nodes={source,defaultNode(K::ImagePreview,2)};graph.links={{1,0,2,1}};publish(service,workspace);
        const auto evaluate=[&]{return g==3?service.evaluateVision(Context{}):service.evaluate(Context{});};
        evaluate();require(!sampleNode(service.telemetry(),g,2).value.preview,"Hidden universal preview evaluated its text");
        service.setPreviewNodes({previewKey(g,2)});std::this_thread::sleep_for(std::chrono::milliseconds(35));evaluate();
        const auto shown=sampleNode(service.telemetry(),g,2).value.preview;
        require(shown&&shown->sourceType==T::String&&shown->text==source.textParameters["value"]&&shown->rgba.empty(),"Universal preview lost full text or allocated image pixels for it");
        std::this_thread::sleep_for(std::chrono::milliseconds(35));evaluate();require(sampleNode(service.telemetry(),g,2).value.preview==shown,"Preview recreated unchanged text inside its update budget");
        source.textParameters["value"]="Changed";graph.nodes[0]=source;publish(service,workspace);evaluate();
        require(sampleNode(service.telemetry(),g,2).value.preview->text=="Changed","Editing preview data was not applied immediately");
        service.setPreviewNodes({});evaluate();require(!sampleNode(service.telemetry(),g,2).value.preview,"Hidden text preview retained its display payload");
    }
}
void testUniversalPreviewNumbersStructuresAndImageCompatibility()
{
    using K=NodeKind;Service service;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[0];
    auto source=defaultNode(K::Float,1);source.value=0.005555555555555556;graph.nodes={source,defaultNode(K::ImagePreview,2)};graph.links={{1,0,2,1}};publish(service,workspace);service.setPreviewNodes({previewKey(0,2)});service.evaluate(Context{});
    const auto numeric=sampleNode(service.telemetry(),0,2).value.preview;
    require(numeric&&std::stod(numeric->text)==source.value,"Numeric preview rounded away stored precision");
    graph.nodes[0]=defaultNode(K::Center,1);publish(service,workspace);Context context;context.center={123.25,999.5};service.evaluate(context);
    const auto point=sampleNode(service.telemetry(),0,2).value.preview;require(point&&point->text.find("123.25")!=std::string::npos&&point->text.find("999.5")!=std::string::npos,"Point preview omitted one component");
    auto& vision=workspace.graphs[3];vision.nodes={defaultNode(K::CurrentFrame,1),defaultNode(K::ImagePreview,2)};vision.links={{1,0,2,1}};publish(service,workspace);service.setPreviewNodes({2});
    VisionFixture fixture;int conversions=0;fixture.context.preview=[&](const Value& value,const DetectionStream&,const Node&){++conversions;auto image=std::make_shared<PreviewImage>();image->width=image->height=1;image->rgba.resize(4,255);image->sourceType=value.type;return Preview(image);};
    service.evaluateVision(fixture.context);require(conversions==1&&sampleNode(service.telemetry(),3,2).value.preview->width==1,"Legacy Frame to Preview connection stopped rendering images");
    const auto convert=fixture.context.preview;
    fixture.context.preview=[](const Value&,const DetectionStream&,const Node&)->Preview{throw std::runtime_error("Frame conversion failed");};
    auto next=std::make_shared<FrameData>(*fixture.context.frame);++next->version;fixture.context.frame=std::move(next);
    std::this_thread::sleep_for(std::chrono::milliseconds(75));service.evaluateVision(fixture.context);
    const auto failed=sampleNode(service.telemetry(),3,2).value;
    require(!failed.valid&&failed.preview&&failed.preview->width==0&&failed.preview->text.find("Frame conversion failed")!=std::string::npos,"Preview conversion errors still hid behind an empty image panel");
    fixture.context.preview=convert;std::this_thread::sleep_for(std::chrono::milliseconds(75));service.evaluateVision(fixture.context);
    require(sampleNode(service.telemetry(),3,2).value.valid&&sampleNode(service.telemetry(),3,2).value.preview->width==1,"A recovered image converter retained an error message");
    std::string document,error;Workspace restored;require(serialize(workspace,document,error)&&deserialize(document,restored,error),"Universal preview did not preserve old port indexes through export/import");
}
}
