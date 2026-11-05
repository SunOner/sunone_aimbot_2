#pragma once
namespace
{
int snapshotUniversalPreview(const std::string& filename,bool drawingExample=false,int focusGroup=0)
{
    EditorHarness h;EditorBenchmarkGpu gpu;
    if(drawingExample){h.windowWidth=1960;ImGui::GetIO().DisplaySize=ImVec2(2000,800);}
    gpu.resize(drawingExample?2000:1200,800);
    require(ImGui_ImplDX11_Init(gpu.device.Get(),gpu.context.Get()),"Preview snapshot renderer initialization failed");
    ImGui::GetIO().Fonts->Clear();
    if(!ImGui::GetIO().Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf",16.5f))ImGui::GetIO().Fonts->AddFontDefault();
    g_pd3dDevice=gpu.device.Get();g_pd3dDeviceContext=gpu.context.Get();h.graphIndex=drawingExample?0:3;
    auto& graph=h.graph();
    if(drawingExample)graph=predictionDrawingExample();
    else
    {
        graph.nodes={defaultNode(NodeKind::String,1,-600,32),defaultNode(NodeKind::ImagePreview,2,32,32),defaultNode(NodeKind::CurrentFrame,3,-600,320),defaultNode(NodeKind::ImagePreview,4,448,32)};
        graph.links={{1,0,2,1},{3,0,4,1}};graph.groups.clear();graph.ties.clear();graph.panX=graph.panY=0;graph.zoom=1;
    }
    edited();require(!editor.error,editor.message.c_str());if(drawingExample)editor.fit[0]=true;
    if(drawingExample&&focusGroup>0)
    {
        const auto group=std::find_if(graph.groups.begin(),graph.groups.end(),[&](const Group& group){return group.id==focusGroup;});
        require(group!=graph.groups.end(),"Snapshot group not found");graph.zoom=focusGroup==5?0.75f:1;
        graph.panX=48-group->x*graph.zoom;graph.panY=80-group->y*graph.zoom;editor.fit[0]=false;
    }
    auto text=std::make_shared<PreviewImage>();text->sourceType=ValueType::String;text->text="Universal data preview\n\nFull precision: 0.005555555555555556\n\nText wraps inside the node and stays complete. Use the mouse wheel to scroll. Ctrl + wheel zooms the graph.\n\n{\n  \"frame\": 120,\n  \"ready\": true,\n  \"confidence\": 0.9725\n}\n\nLong text remains available through Copy preview value.\n\n";
    for(int i=0;i<20;++i)text->text+="Additional line "+std::to_string(i)+"\n";
    auto image=std::make_shared<PreviewImage>();image->width=320;image->height=180;image->sourceWidth=1280;image->sourceHeight=720;image->rgba.resize(320*180*4);
    for(int y=0;y<180;++y)for(int x=0;x<320;++x){const auto at=(y*320+x)*4;image->rgba[at]=static_cast<std::uint8_t>(x*255/319);image->rgba[at+1]=static_cast<std::uint8_t>(y*255/179);image->rgba[at+2]=150;image->rgba[at+3]=255;}
    for(int frame=0;frame<3;++frame)
    {
        editor.live.graphs[3].clear();editor.liveIndex[3].clear();
        if(!drawingExample)for(const auto entry:{std::pair{2,Preview(text)},std::pair{4,Preview(image)}}){NodeValue value;value.nodeId=entry.first;value.value.preview=entry.second;value.value.boolean=true;editor.liveIndex[3].emplace_back(entry.first,editor.live.graphs[3].size());editor.live.graphs[3].push_back(std::move(value));}
        editor.live.graphRevisions[3]=service().revision();editor.live.graphSampledAt[3]=std::chrono::steady_clock::now();editor.telemetryAt=1000000;
        ImGui_ImplDX11_NewFrame();h.frame();gpu.render();
    }
    D3D11_TEXTURE2D_DESC desc{};gpu.texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;require(SUCCEEDED(gpu.device->CreateTexture2D(&desc,nullptr,&staging)),"Preview snapshot staging failed");gpu.context->CopyResource(staging.Get(),gpu.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};require(SUCCEEDED(gpu.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"Preview snapshot read failed");
    std::ofstream file(filename,std::ios::binary);require(static_cast<bool>(file),"Preview snapshot file could not be opened");
    const auto word=[&](std::uint16_t v){file.write(reinterpret_cast<const char*>(&v),2);};const auto dword=[&](std::uint32_t v){file.write(reinterpret_cast<const char*>(&v),4);};
    word(0x4d42);dword(54+desc.Width*desc.Height*4);word(0);word(0);dword(54);dword(40);dword(desc.Width);dword(static_cast<std::uint32_t>(-static_cast<int>(desc.Height)));word(1);word(32);dword(0);dword(desc.Width*desc.Height*4);dword(0);dword(0);dword(0);dword(0);
    for(UINT y=0;y<desc.Height;++y){const auto* row=static_cast<const std::uint8_t*>(mapped.pData)+y*mapped.RowPitch;for(UINT x=0;x<desc.Width;++x){const char pixel[]{static_cast<char>(row[x*4+2]),static_cast<char>(row[x*4+1]),static_cast<char>(row[x*4]),static_cast<char>(255)};file.write(pixel,4);}}
    gpu.context->Unmap(staging.Get(),0);NodePreview::clear();ImGui_ImplDX11_Shutdown();g_pd3dDevice=nullptr;g_pd3dDeviceContext=nullptr;
    require(static_cast<bool>(file),"Preview snapshot write failed");return 0;
}
}
