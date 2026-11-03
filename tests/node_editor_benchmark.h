#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include "imgui/imgui_impl_dx11.h"
#include <iomanip>
#include <ctime>
#include <thread>

namespace
{
using Microsoft::WRL::ComPtr;
struct EditorBenchmarkGpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11Query> disjoint, begin, end;
    std::string adapter;

    EditorBenchmarkGpu()
    {
        D3D_FEATURE_LEVEL level;
        require(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
            &device,&level,&context)),"Hardware D3D11 benchmark device could not be created");
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> gpu;DXGI_ADAPTER_DESC desc{};
        if(SUCCEEDED(device.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&gpu))&&SUCCEEDED(gpu->GetDesc(&desc)))
            for(wchar_t ch:desc.Description){if(!ch)break;adapter.push_back(ch<128?static_cast<char>(ch):'?');}
        D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
        require(SUCCEEDED(device->CreateQuery(&query,&disjoint)),"Disjoint query unavailable");
        query.Query=D3D11_QUERY_TIMESTAMP;
        require(SUCCEEDED(device->CreateQuery(&query,&begin))&&SUCCEEDED(device->CreateQuery(&query,&end)),"GPU timestamp queries unavailable");
    }
    void resize(int width,int height)
    {
        context->OMSetRenderTargets(0,nullptr,nullptr);target.Reset();texture.Reset();
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        require(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&texture))&&SUCCEEDED(device->CreateRenderTargetView(texture.Get(),nullptr,&target)),"Offscreen benchmark target could not be created");
    }
    std::pair<double,double> render()
    {
        const auto cpuBegin=std::chrono::steady_clock::now();
        context->Begin(disjoint.Get());context->End(begin.Get());
        ID3D11RenderTargetView* view=target.Get();context->OMSetRenderTargets(1,&view,nullptr);
        const float clear[]={0,0,0,0};context->ClearRenderTargetView(view,clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        context->End(end.Get());context->End(disjoint.Get());context->Flush();
        const double cpu=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-cpuBegin).count();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        HRESULT ready;
        while((ready=context->GetData(disjoint.Get(),&timing,sizeof(timing),0))==S_FALSE&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
        require(ready==S_OK,"GPU benchmark query timed out");
        UINT64 first=0,last=0;
        require(context->GetData(begin.Get(),&first,sizeof(first),0)==S_OK&&context->GetData(end.Get(),&last,sizeof(last),0)==S_OK,"GPU timestamps did not complete");
        return {cpu,timing.Disjoint||!timing.Frequency?-1:static_cast<double>(last-first)*1000/timing.Frequency};
    }
};

void benchmarkGraph(int count,int width,bool routing=false,bool settings=false)
{
    editor=Editor{};editor.initialized=true;editor.preview=true;editor.activeGraph=0;
    editor.fit.fill(false);editor.autoFit.fill(false);editor.viewMode=1;
    for(std::size_t i=0;i<GraphCount;++i)editor.draft.graphs[i].kind=static_cast<GraphKind>(i);
    auto& graph=editor.draft.graphs[0];graph.panX=graph.panY=16;graph.zoom=0.65f;
    const auto denseNode=defaultNode(NodeKind::Prediction,1);
    const float columnStep=settings?Layout::up(cardWidth(denseNode)+64):224;
    const float rowStep=settings?Layout::up(Layout::height(denseNode,true)+64):128;
    const int columns=settings?std::max(1,static_cast<int>((width-32)/(columnStep*graph.zoom))):width>=2000?16:12;
    for(int i=0;i<count;++i)graph.nodes.push_back(defaultNode(i<8?NodeKind::Number:settings?(i==8?NodeKind::Boolean:i==9?NodeKind::MakePoint:NodeKind::Prediction):routing&&i%8==0?NodeKind::RerouteNumber:NodeKind::Clamp,i+1,
        static_cast<float>((i%columns)*columnStep),static_cast<float>((i/columns)*rowStep)));
    if(settings)
    {
        graph.links={{1,0,10,0},{2,0,10,1}};
        for(int i=10;i<count&&graph.links.size()<MaxLinks;++i)
        {
            graph.links.push_back({i,0,i+1,0});
            if(graph.links.size()<MaxLinks)graph.links.push_back({1+i%8,0,i+1,1});
            if(graph.links.size()<MaxLinks)graph.links.push_back({9,0,i+1,2});
        }
    }
    else
    for(int i=8;i<count&&graph.links.size()<MaxLinks;++i)
        for(int pin=0;pin<static_cast<int>(definition(graph.nodes[i].kind).inputs.size())&&graph.links.size()<MaxLinks;++pin)
            graph.links.push_back({1+((i*7+pin*11)%i),0,i+1,pin});
    if(routing)
    {
        for(int i=0;i<4;++i)graph.ties.push_back({i+1,static_cast<float>(448+i*448),512,static_cast<float>(i*15)});
        for(std::size_t i=0;i<graph.links.size();++i)graph.links[i].ties={1+static_cast<int>(i%4),1+static_cast<int>((i+1)%4)};
        for(int i=0;i<4;++i)
        {
            Group group;group.id=i+1;group.title="Group "+std::to_string(i+1);group.x=0;group.y=static_cast<float>(i*256-32);group.width=static_cast<float>(columns*224);group.height=288;
            for(int id=i*32+1;id<=std::min(count,(i+1)*32);++id)group.nodes.push_back(id);graph.groups.push_back(std::move(group));
        }
    }
    std::string error;if(!service().publish(editor.draft,error))throw std::runtime_error(error);editor.observedRevision=service().revision();
    editor.live.graphRevisions[0]=editor.observedRevision;
    for(int i=0;i<count;++i)
    {
        NodeValue value;value.nodeId=i+1;value.value.type=settings&&i>=9?ValueType::Point:settings&&i==8?ValueType::Boolean:ValueType::Number;value.value.number=1.25;
        value.active=value.signalActive=true;value.readInputs=7;editor.live.graphs[0].push_back(value);editor.liveIndex[0].emplace_back(i+1,i);
    }
}
double percentile(std::vector<double> values,double fraction)
{
    if(values.empty())return -1;std::sort(values.begin(),values.end());return values[static_cast<std::size_t>((values.size()-1)*fraction)];
}
int benchmarkNodeEditor(const std::string& filename,const std::string& filter={})
{
    EditorBenchmarkGpu gpu;std::filesystem::path output(filename);
    if(output.has_parent_path())std::filesystem::create_directories(output.parent_path());
    const std::string header="timestamp_utc,commit_id,git_dirty,build_backend,model,provider,adapter,width,height,nodes,links,input_hz,mode,cpu_p50_ms,cpu_p95_ms,gpu_p50_ms,gpu_p95_ms,vertices,indices,max_pulses,route_rebuilds,formatted_values,accepted_flashes";
    const bool exists=std::filesystem::exists(output)&&std::filesystem::file_size(output)>0;
    if(exists){std::ifstream previous(output);std::string line;std::getline(previous,line);require(line==header,"Existing benchmark CSV uses another schema; choose a new output file");}
    std::ofstream csv(output,std::ios::app);require(static_cast<bool>(csv),"Could not write node benchmark results");
    if(!exists)csv<<header<<'\n';
    std::cout<<"Adapter: "<<gpu.adapter<<"\n"<<std::fixed<<std::setprecision(3);
    for(const auto resolution:{std::pair{1920,1080},std::pair{2560,1440}})
        for(const std::string mode:{"idle","30hz","1000hz","pan1000hz","zoom1000hz","drag1000hz","placement1000hz","wire1000hz","resize1000hz","far1000hz","groups1000hz","overviewzoom1000hz","routing1000hz","bend1000hz","settings1000hz","options1000hz","previews15hz"})
        {
            if(!filter.empty()&&mode!=filter)continue;
            const bool previews=mode=="previews15hz";const int graphIndex=previews?3:0;
            ImGui::CreateContext();auto& io=ImGui::GetIO();io.DisplaySize=ImVec2(static_cast<float>(resolution.first),static_cast<float>(resolution.second));
            io.DeltaTime=1.0f/144;io.IniFilename=io.LogFilename=nullptr;io.ConfigInputTrickleEventQueue=false;
            if(!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf",16.5f))io.Fonts->AddFontDefault();
            require(ImGui_ImplDX11_Init(gpu.device.Get(),gpu.context.Get()),"Benchmark renderer initialization failed");
            gpu.resize(resolution.first,resolution.second);benchmarkGraph(static_cast<int>(MaxNodes),resolution.first,mode=="routing1000hz"||mode=="bend1000hz",mode=="settings1000hz"||mode=="options1000hz");
            g_pd3dDevice=gpu.device.Get();g_pd3dDeviceContext=gpu.context.Get();
            if(previews)
            {
                editor=Editor{};editor.draft=defaults();editor.initialized=true;editor.activeGraph=3;editor.fit.fill(false);editor.autoFit.fill(false);
                for(auto& graph:editor.draft.graphs){graph.nodes.clear();graph.links.clear();graph.groups.clear();graph.ties.clear();}
                auto& graph=editor.draft.graphs[3];graph.panX=graph.panY=16;graph.zoom=0.85f;
                for(int id=1;id<=4;++id)graph.nodes.push_back(defaultNode(NodeKind::ImagePreview,id,32+(id-1)*384.0f,32));
                graph.nodes.push_back(defaultNode(NodeKind::Float,5,32,576));
                for(int id=6;id<=static_cast<int>(MaxNodes);++id)
                {
                    graph.nodes.push_back(defaultNode(NodeKind::Clamp,id,32+((id-6)%8)*224.0f,736+((id-6)/8)*128.0f));
                    for(int pin=0;pin<3&&graph.links.size()<MaxLinks;++pin)graph.links.push_back({5,0,id,pin});
                }
                std::string error;if(!service().publish(editor.draft,error))throw std::runtime_error(error);editor.observedRevision=service().revision();editor.live.graphRevisions[3]=editor.observedRevision;
                for(int id=1;id<=static_cast<int>(MaxNodes);++id){NodeValue value;value.nodeId=id;value.value.type=id<=4?ValueType::Boolean:ValueType::Float;value.value.boolean=true;editor.live.graphs[3].push_back(value);editor.liveIndex[3].emplace_back(id,id-1);}
            }
            if(mode=="options1000hz")
            {
                const auto& inputs=definition(NodeKind::Prediction).inputs;const auto found=std::find_if(inputs.begin(),inputs.end(),[](const Pin& pin){return pin.defaultParameter&&std::strcmp(pin.defaultParameter,"kalman_measurement_noise")==0;});
                require(found!=inputs.end(),"Benchmark option input is missing");const int pin=static_cast<int>(found-inputs.begin());
                for(auto& link:editor.draft.graphs[graphIndex].links)if(link.toPort==2){link.toPort=pin;link.fromNode=1;}
                edited();editor.live.graphRevisions[graphIndex]=editor.observedRevision;for(auto& value:editor.live.graphs[graphIndex])value.readInputs|=std::uint64_t{1}<<pin;
            }
            if(mode=="groups1000hz")
            {
                editor.viewMode=2;auto& graph=editor.draft.graphs[0];graph.zoom=0.3f;graph.groups.clear();
                for(int g=0;g<static_cast<int>(MaxGroups);++g)graph.groups.push_back({g+1,"Processing stage "+std::to_string(g+1),static_cast<float>((g%8)*384),static_cast<float>(128+(g/8)*384),384,384,0x40ffcd60u,{}});
                std::string error;require(service().publish(editor.draft,error),error.c_str());editor.observedRevision=service().revision();editor.live.graphRevisions[0]=editor.observedRevision;
            }
            if(mode=="far1000hz"){editor.viewMode=0;editor.draft.graphs[graphIndex].zoom=0.3f;}
            if(mode=="overviewzoom1000hz")editor.viewMode=0;
            if(mode=="resize1000hz"){editor.selection[graphIndex]={50};editor.selected[graphIndex]=50;}
            if(mode=="placement1000hz")
            {
                selectAll(editor.draft.graphs[graphIndex],graphIndex);
                captureDragSelection(editor.draft.graphs[graphIndex],graphIndex,true);
                editor.moveOriginal=editor.draft;editor.keyboardMove=true;editor.moveGraph=graphIndex;
                editor.dragStartMouse=ImVec2(static_cast<float>(resolution.first-25),static_cast<float>(resolution.second-25));
            }
            io.AddMousePosEvent(static_cast<float>(resolution.first-25),static_cast<float>(resolution.second-25));
            std::vector<double> cpu,gpuTimes;int vertices=0,indices=0;std::size_t pulses=0;
            for(int frame=0;frame<210;++frame)
            {
                const double time=ImGui::GetTime();const int hz=previews?15:mode=="idle"?0:mode=="30hz"?30:1000;
                const double sample=std::floor(time*30)/30;
                const auto now=std::chrono::steady_clock::now();
                for(auto& value:editor.live.graphs[graphIndex])
                {
                    value.signalSequence=hz?static_cast<std::uint64_t>(sample*hz)+1:1;
                    value.signaledAt=now-std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(hz?time-sample:2));
                    value.value.number=hz?std::sin(sample)*100:1.25;
                }
                editor.live.graphSampledAt[graphIndex]=now;editor.telemetryAt=time+1;
                if(mode=="pan1000hz")editor.draft.graphs[graphIndex].panX=static_cast<float>(16+std::sin(time)*24);
                if(mode=="zoom1000hz")editor.draft.graphs[graphIndex].zoom=static_cast<float>(0.72+std::sin(time)*0.04);
                if(mode=="overviewzoom1000hz")editor.draft.graphs[graphIndex].zoom=static_cast<float>(0.45+std::sin(time)*0.08);
                if(mode=="drag1000hz")editor.draft.graphs[graphIndex].nodes[49].x=static_cast<float>((49%(resolution.first>=2000?16:12))*224+(frame%2)*32);
                if(mode=="bend1000hz")editor.draft.graphs[graphIndex].links[0].bend={static_cast<float>(0.58+std::sin(time)*0.35),static_cast<float>(std::cos(time)*0.8)};
                if(mode=="placement1000hz")io.AddMousePosEvent(editor.dragStartMouse.x+static_cast<float>(std::sin(time)*43),editor.dragStartMouse.y+static_cast<float>(std::cos(time)*29));
                if(mode=="wire1000hz")
                {
                    const auto& graph=editor.draft.graphs[0];
                    if(frame==1)io.AddMousePosEvent(portPoint(graph,editor.canvasOrigin[0],graph.nodes[0],true,0).x,portPoint(graph,editor.canvasOrigin[0],graph.nodes[0],true,0).y);
                    if(frame==2)io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);
                    if(frame>=3){const auto point=portPoint(graph,editor.canvasOrigin[0],graph.nodes[12],false,0);io.AddMousePosEvent(point.x+14,point.y);}
                }
                const auto started=std::chrono::steady_clock::now();
                if(previews)
                {
                    require(editor.live.graphs[3].size()>=4,"Preview benchmark lost its synthetic telemetry");
                    const auto version=static_cast<std::int64_t>(time*15);
                    for(int i=0;i<4;++i)
                    {
                        auto& value=editor.live.graphs[3][i].value;if(value.preview&&value.preview->version==version)continue;
                        auto image=std::make_shared<PreviewImage>();image->width=image->sourceWidth=320;image->height=image->sourceHeight=180;image->version=version;image->rgba.resize(320*180*4);
                        for(std::size_t p=0;p<image->rgba.size();p+=4){image->rgba[p]=static_cast<std::uint8_t>(version*7+i*40);image->rgba[p+1]=static_cast<std::uint8_t>(p/4);image->rgba[p+2]=90;image->rgba[p+3]=255;}
                        value.preview=image;
                    }
                }

                if(mode=="resize1000hz")
                {
                    auto& node=editor.draft.graphs[graphIndex].nodes[49];node.width=static_cast<float>(256+(frame%2)*32);node.height=static_cast<float>(192+(frame%2)*32);
                    // Include immediate publication, as a real resize gesture does.
                    edited(false);
                }
                ImGui_ImplDX11_NewFrame();NodeEditor_PrepareFrame();ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(io.DisplaySize);
                ImGui::Begin("Node benchmark",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove);
                drawEditor(graphIndex);ImGui::End();ImGui::Render();
                const double build=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
                const auto timing=gpu.render();
                if(frame>=30)
                {
                    cpu.push_back(build+timing.first);if(timing.second>=0)gpuTimes.push_back(timing.second);
                    vertices=std::max(vertices,ImGui::GetDrawData()->TotalVtxCount);indices=std::max(indices,ImGui::GetDrawData()->TotalIdxCount);
                    pulses=std::max(pulses,editor.drawnSignalPulses);
                }
            }
            if(mode=="groups1000hz")require(editor.groupCaptions[0].size()==MaxGroups&&std::all_of(editor.groupCaptions[0].begin(),editor.groupCaptions[0].end(),[](const GroupCaption& caption){return caption.layout.layouts==1;}),"Maximum group overview captions are missing or rebuild at frame frequency");
            if(mode=="wire1000hz")require(editor.wireNode==1&&editor.wireFeedback.target.node==13&&editor.wireFeedback.checks==1,"Large-graph wire capture was inactive or repeatedly recompiled its target");
            const auto links=editor.draft.graphs[graphIndex].links.size();
            const auto timestamp=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());std::tm utc{};gmtime_s(&utc,&timestamp);
            const auto& cache=editor.canvasRoutes[graphIndex];std::uint64_t formats=0,flashes=0;for(const auto& display:cache.displays){formats+=display.formats;flashes+=display.flashes;}
            csv<<std::put_time(&utc,"%Y-%m-%dT%H:%M:%SZ")<<','<<NODE_BENCHMARK_COMMIT<<','<<NODE_BENCHMARK_DIRTY<<','<<NODE_BENCHMARK_BACKEND<<",synthetic-graph,d3d11-ui,"
                <<std::quoted(gpu.adapter)<<','<<resolution.first<<','<<resolution.second<<','<<MaxNodes<<','<<links<<','<<(previews?15:mode=="idle"?0:mode=="30hz"?30:1000)<<','<<mode<<','
                <<percentile(cpu,0.5)<<','<<percentile(cpu,0.95)<<','<<percentile(gpuTimes,0.5)<<','<<percentile(gpuTimes,0.95)<<','<<vertices<<','<<indices<<','<<pulses<<','<<cache.rebuilds<<','<<formats<<','<<flashes<<'\n';
            csv.flush();
            std::cout<<resolution.first<<'x'<<resolution.second<<' '<<mode<<" CPU p50/p95 "<<percentile(cpu,0.5)<<'/'<<percentile(cpu,0.95)<<" ms; GPU "<<percentile(gpuTimes,0.5)<<'/'<<percentile(gpuTimes,0.95)<<" ms; pulses "<<pulses<<std::endl;
            NodePreview::clear();ImGui_ImplDX11_Shutdown();ImGui::DestroyContext();editor=Editor{};g_pd3dDevice=nullptr;g_pd3dDeviceContext=nullptr;
        }
    std::cout<<"Results: "<<output.string()<<'\n';return 0;
}
}
