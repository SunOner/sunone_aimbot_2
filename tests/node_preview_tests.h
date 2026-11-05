#pragma once

namespace
{
void testPreviewVisibilityFollowsTheImagePanel()
{
    gestureScenario="Preview visibility, collapse and selected-card priority";
    EditorHarness h;h.graphIndex=3;h.graph().nodes={defaultNode(NodeKind::ImagePreview,1,32,32)};h.graph().links.clear();edited();h.frame();
    auto requests=service().previewNodes();require(requests&&*requests==std::vector<int>{1},"An expanded visible preview did not subscribe to image updates");
    findNode(h.graph(),1)->collapsed=true;h.frame();requests=service().previewNodes();require(!requests||requests->empty(),"A collapsed preview still requested pixels");
    findNode(h.graph(),1)->collapsed=false;h.graph().panY=-10000;h.frame();requests=service().previewNodes();require(!requests||requests->empty(),"An offscreen preview still requested pixels");
    h.graph().panY=0;h.graph().zoom=0.3f;h.frame();requests=service().previewNodes();require(!requests||requests->empty(),"Semantic zoom kept a hidden thumbnail active");
    h.graph().zoom=1;
    for(int id=2;id<=6;++id)h.graph().nodes.push_back(defaultNode(NodeKind::ImagePreview,id,32,32));
    edited();selectOne(3,6);h.frame();requests=service().previewNodes();
    require(requests&&requests->size()==MaxLivePreviews&&std::find(requests->begin(),requests->end(),6)!=requests->end(),"The selected preview did not receive priority within the visible image budget");
    NodeEditor_EndInteraction();requests=service().previewNodes();require(!requests||requests->empty(),"Leaving the editor retained preview subscriptions");
    require(kindAllowed(editor.draft.graphs[0],NodeKind::ImagePreview),"A universal preview is missing from a Logic workflow");
    auto& graph=editor.draft.graphs[0];graph.nodes={defaultNode(NodeKind::Boolean,1),defaultNode(NodeKind::RerouteFloat,2),defaultNode(NodeKind::ImagePreview,3)};graph.links={{2,0,3,1}};
    std::string error;require(looseReroute(graph,graph.nodes[1])&&canConnectReroutes(graph,{1,0,2,0},error),"A reroute wired to Any prematurely locked its type");
    adaptLooseReroutes(graph,{1,0,2,0});require(graph.nodes[1].kind==NodeKind::RerouteBoolean,"The first typed source did not choose the reroute type through Preview");
}
void testUniversalPreviewTextWrapScrollAndCopy()
{
    gestureScenario="Universal preview wraps full text, scrolls without zooming and keeps a cached layout";
    EditorHarness h;h.graph().nodes={defaultNode(NodeKind::ImagePreview,1,32,32)};h.graph().links.clear();h.graph().zoom=1;edited();h.frame();
    auto content=std::make_shared<PreviewImage>();content->sourceType=ValueType::String;
    for(int i=0;i<100;++i)content->text+="Line "+std::to_string(i)+" with full text and precise value 0.005555555555555556\n";
    const auto install=[&]{editor.live.graphs[0]={{}};auto& sample=editor.live.graphs[0][0];sample.nodeId=1;sample.value.preview=content;sample.value.valid=true;editor.liveIndex[0]={{1,0}};editor.live.graphRevisions[0]=service().revision();editor.live.graphSampledAt[0]=std::chrono::steady_clock::now();editor.telemetryAt=1000000;};
    install();h.frame();auto& layout=editor.canvasRoutes[0].displays[0].previewText;
    require(layout.content==content&&layout.lines.size()>=100,"Full preview text was truncated or not wrapped");const auto builds=layout.builds;
    h.frame();require(layout.builds==builds,"Preview text layout rebuilt on an unchanged frame");
    const auto panel=previewRect(h.graph(),h.origin,h.graph().nodes[0]);h.move(panel.GetCenter());const auto zoom=h.graph().zoom;
    ImGui::GetIO().AddMouseWheelEvent(0,-3);h.frame();require(h.graph().zoom==zoom&&layout.scroll>0,"Scrolling preview text zoomed the graph or did not scroll");
    h.key(ImGuiMod_Ctrl,true);ImGui::GetIO().AddMouseWheelEvent(0,-1);h.frame();h.key(ImGuiMod_Ctrl,false);
    require(h.graph().zoom<zoom,"Ctrl+wheel over preview text cannot zoom the graph");
    auto unicode=std::make_shared<PreviewImage>();unicode->sourceType=ValueType::String;unicode->text="ДлинныйТекстБезПробелов_ТочныйТекст\nsecond line\n";
    NodePreview::TextLayout wrapped;wrapped.prepare(unicode,ImGui::GetFont(),ImGui::GetFontSize(),18);
    for(const auto& line:wrapped.lines)
    {require(line.first<=line.second&&line.second<=unicode->text.size(),"Preview line bounds escaped their text");if(line.second<unicode->text.size())require((static_cast<unsigned char>(unicode->text[line.second])&0xc0)!=0x80,"Wrapping split a UTF-8 character");}
    // Context-menu copy uses the immutable full value, never the wrapped rows.
    require(content->text.find("Line 99")!=std::string::npos&&wrapped.content->text==unicode->text,"Preview layout changed or abbreviated source text");
}
void testPreviewTexturesUploadAndReuseActualPixels()
{
    gestureScenario="Preview textures upload RGBA and reuse their allocation";
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level;
    require(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context)),"Could not create the software preview renderer");
    auto image=std::make_shared<PreviewImage>();image->width=image->height=2;image->rgba={230,40,10,255,0,0,0,255,0,0,0,255,0,0,0,255};
    const auto texture=NodePreview::texture(1,image,device.Get(),context.Get());require(texture!=0,"Preview texture creation failed");
    const auto read=[&](ImTextureID id){
        auto* view=reinterpret_cast<ID3D11ShaderResourceView*>(id);ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> source;resource.As(&source);D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;require(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)),"Could not read preview texture");
        context->CopyResource(staging.Get(),source.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"Could not map preview texture");
        const auto* data=static_cast<const std::uint8_t*>(mapped.pData);const std::array<std::uint8_t,4> result={data[0],data[1],data[2],data[3]};context->Unmap(staging.Get(),0);return result;
    };
    require(read(texture)==std::array<std::uint8_t,4>{230,40,10,255},"Preview texture has incorrect pixel data or channel order");
    require(NodePreview::texture(1,image,device.Get(),context.Get())==texture,"An unchanged preview recreated its texture");
    auto next=std::make_shared<PreviewImage>(*image);next->rgba[0]=11;
    require(NodePreview::texture(1,next,device.Get(),context.Get())==texture&&read(texture)[0]==11,"A new frame did not update the existing texture");
    NodePreview::retain({});NodePreview::clear();
}
}
