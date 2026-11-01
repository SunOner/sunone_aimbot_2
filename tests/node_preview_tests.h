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
    require(!kindAllowed(editor.draft.graphs[0],NodeKind::ImagePreview),"A Vision preview can be added to a control graph");
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
