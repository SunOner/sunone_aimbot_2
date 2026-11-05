#pragma once
namespace
{
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
    std::string document,error;Workspace restored;require(serialize(workspace,document,error)&&deserialize(document,restored,error),"Universal preview did not preserve old port indexes through export/import");
}
}
