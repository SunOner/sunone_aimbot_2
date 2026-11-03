#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include "imgui/imgui.h"
#include "overlay/draw_settings.h"
#include "overlay/ui_sections.h"
#include "overlay/export_job.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#ifdef USE_CUDA
#include "tensorrt/nvinf.h"
#include "tensorrt/trt_monitor.h"
#include "depth/depth_anything_trt.h"
#include "overlay/export_progress_panel.h"
#endif

namespace
{
ModelExport::Job exportJob;
std::atomic<bool> ownsBuilder{false};
void cancelExport()
{
    exportJob.cancel();
#ifdef USE_CUDA
    if(ownsBuilder.load())gTrtExportCancelRequested=true;
#endif
}
#ifdef USE_CUDA
std::string buildModel(const std::string& input,bool depth,bool rebuild,const TrtBuildOptions& options,const std::atomic<bool>& cancel)
{
    std::unique_lock<std::recursive_mutex> lock(gTrtBuildMutex,std::defer_lock);
    while(!lock.try_lock()){if(cancel.load())return {};std::this_thread::sleep_for(std::chrono::milliseconds(25));}
    if(cancel.load())return {};
    ownsBuilder=true;gTrtCancellationToken=&cancel;
    struct Finish{~Finish(){gTrtCancellationToken=nullptr;ownsBuilder=false;}} finish;
    const auto source=std::filesystem::u8path(input);
    if(depth)
    {
        auto output=source;output.replace_extension(".engine");std::error_code error;
        if(!rebuild&&std::filesystem::is_regular_file(output,error)&&std::filesystem::file_size(output,error)>0)return output.u8string();
        depth_anything::DepthAnythingTrt builder;std::string path;
        if(!builder.exportEngine(input,gLogger,&path))throw std::runtime_error(builder.lastError());
        return path;
    }
    const auto output=std::filesystem::u8path(trtEngineCachePath(input,options));std::error_code error;
    if(!rebuild&&std::filesystem::is_regular_file(output,error)&&std::filesystem::file_size(output,error)>0)return output.u8string();
    std::unique_ptr<nvinfer1::IRuntime> runtime(nvinfer1::createInferRuntime(gLogger));
    if(!runtime)throw std::runtime_error("TensorRT runtime creation failed.");
    std::unique_ptr<nvinfer1::ICudaEngine> engine(buildEngineFromOnnx(input,gLogger,options,*runtime));
    if(cancel.load())return {};
    if(!engine)throw std::runtime_error("TensorRT produced no engine.");
    std::unique_ptr<nvinfer1::IHostMemory> plan(engine->serialize());if(!plan)throw std::runtime_error("Engine serialization failed.");
    auto temporary=output;temporary+=L".tmp";
    {
        std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
        if(!file||!file.write(static_cast<const char*>(plan->data()),static_cast<std::streamsize>(plan->size())))throw std::runtime_error("Could not write the engine file.");
    }
    if(cancel.load()){std::filesystem::remove(temporary,error);return {};}
    if(!MoveFileExW(temporary.c_str(),output.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
    {std::filesystem::remove(temporary,error);throw std::runtime_error("Could not finish writing the engine file.");}
    return output.u8string();
}
#endif
struct ModelFile{std::string label,path;};
std::vector<ModelFile> onnxFiles(bool depth)
{
    std::vector<ModelFile> files;
    for(const char* directory:depth?std::vector<const char*>{"models","depth_models"}:std::vector<const char*>{"models"})
    {
        std::error_code error;
        for(std::filesystem::directory_iterator it(directory,error),end;!error&&it!=end;it.increment(error))
        {
            if(!it->is_regular_file(error))continue;
            auto extension=it->path().extension().u8string();std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            if(extension==".onnx")files.push_back({it->path().generic_u8string(),std::filesystem::absolute(it->path(),error).u8string()});
        }
    }
    std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.label<b.label;});return files;
}
}
void draw_model_export()
{
    static int type=0,lastType=-1;static std::vector<ModelFile> files;static std::string selected,error;
    static char customPath[1025]{};static bool fp16=true,fp8=false,fixed=false,rebuild=false;
    static int resolution=320,minimum=160,optimal=320,maximum=640;
    const auto result=exportJob.snapshot();const bool busy=result.state==ModelExport::State::Running;
    ImGui::TextUnformatted("Model export");
    ImGui::PushTextWrapPos(0);ImGui::TextDisabled("Build a TensorRT engine from ONNX, then select the engine in an Inference node.");ImGui::PopTextWrapPos();
    if(OverlayUI::BeginSection("Source model","model_export_source"))
    {
        ImGui::BeginDisabled(busy);
        const char* types[]={"Object detection","Depth"};OverlayUI::ComboRow("Model type",&type,types,2);
        bool refresh=lastType!=type;
        if(ImGui::Button("Refresh models"))refresh=true;
        if(refresh){files=onnxFiles(type==1);lastType=type;}
        const auto row=OverlayUI::BeginSettingRow("ONNX model");
        std::string label="Select model...";for(const auto& file:files)if(file.path==selected)label=file.label;
        if(ImGui::BeginCombo("##model",label.c_str()))
        {
            for(const auto& file:files)if(ImGui::Selectable(file.label.c_str(),selected==file.path)){selected=file.path;customPath[0]=0;error.clear();}
            if(files.empty())ImGui::TextDisabled("No ONNX models found.");ImGui::EndCombo();
        }
        OverlayUI::EndSettingRow(row);
        OverlayUI::InputTextRow("Custom ONNX path",customPath,sizeof(customPath));
        if(type==0)
        {
            OverlayUI::CheckboxRow("FP16",&fp16);OverlayUI::CheckboxRow("FP8",&fp8);OverlayUI::CheckboxRow("Fixed input size",&fixed);
            if(fixed)OverlayUI::InputIntRow("Input size",&resolution,32,64);
            else{OverlayUI::InputIntRow("Minimum size",&minimum,32,64);OverlayUI::InputIntRow("Optimal size",&optimal,32,64);OverlayUI::InputIntRow("Maximum size",&maximum,32,64);}
        }
        OverlayUI::CheckboxRow("Rebuild existing engine",&rebuild);
        ImGui::EndDisabled();
#ifndef USE_CUDA
        ImGui::TextWrapped("TensorRT export requires the CUDA build. DML/CPU nodes use ONNX directly.");
        ImGui::BeginDisabled();
#else
        ImGui::BeginDisabled(busy);
#endif
        if(ImGui::Button("Build engine"))
        {
#ifdef USE_CUDA
            const std::string input=customPath[0]?customPath:selected;std::error_code ec;const auto path=std::filesystem::u8path(input);
            auto extension=path.extension().u8string();std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            if(extension!=".onnx"||!std::filesystem::is_regular_file(path,ec))error="Choose an existing ONNX model.";
            else if(type==0&&(resolution<32||resolution>8192||minimum<32||maximum>8192||minimum>optimal||optimal>maximum))error="Use positive dimensions from 32 to 8192, with minimum <= optimal <= maximum.";
            else
            {
                TrtBuildOptions options;options.fixedInputSize=fixed;options.enableFp16=fp16;options.enableFp8=fp8;
                options.inputWidth=options.inputHeight=resolution;options.minProfileSize=minimum;options.optProfileSize=optimal;options.maxProfileSize=maximum;
                const bool depth=type==1,force=rebuild;const auto absolute=std::filesystem::absolute(path,ec).u8string();
                if(ec)error="Cannot resolve the model path.";
                else{error.clear();exportJob.start(absolute,[absolute,depth,force,options](const auto& cancel){return buildModel(absolute,depth,force,options,cancel);});}
            }
#endif
        }
        ImGui::EndDisabled();OverlayUI::EndSection();
    }
#ifdef USE_CUDA
    if(gIsTrtExporting.load())OverlayExportUI::DrawTensorRtExportPanel("all_model_export","TensorRT export","Building an optimized engine",ownsBuilder.load()?result.input.c_str():nullptr,"Cancel export",ownsBuilder.load()?cancelExport:nullptr);
    else if(busy){ImGui::TextUnformatted("Waiting for the current builder...");if(ImGui::Button("Cancel queued export"))cancelExport();}
#endif
    if(!error.empty()){ImGui::PushTextWrapPos(0);ImGui::TextColored(ImVec4(1,0.4f,0.4f,1),"%s",error.c_str());ImGui::PopTextWrapPos();}
    if(result.state==ModelExport::State::Complete)
    {ImGui::TextUnformatted("Engine ready");ImGui::PushTextWrapPos(0);ImGui::TextUnformatted(result.output.c_str());ImGui::PopTextWrapPos();if(ImGui::Button("Copy engine path"))ImGui::SetClipboardText(result.output.c_str());}
    else if(!result.message.empty()&&!busy){ImGui::PushTextWrapPos(0);ImGui::TextUnformatted(result.message.c_str());ImGui::PopTextWrapPos();}
}
void ModelExport_Shutdown(){cancelExport();exportJob.shutdown();}
