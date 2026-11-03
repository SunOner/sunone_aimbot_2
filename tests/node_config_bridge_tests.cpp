#include "nodes/node_config_bridge.h"
#include "config/config.h"
#include "depth/model_path.h"
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{
using namespace NodeGraph;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
Config settings()
{
    Config c{};
    c.detection_resolution=320;c.class_player=7;c.class_head=9;c.tracker_enabled=true;
    c.body_y_offset=0.22f;c.head_y_offset=0.08f;c.auto_aim=true;c.auto_shoot=true;
    c.easynorecoil=true;c.easynorecoilstrength=500;
    c.predictionInterval=0.12f;c.kalman_enabled=true;c.kalman_process_noise_position=65;
    c.kalman_process_noise_velocity=2300;c.kalman_measurement_noise=42;
    c.kalman_velocity_damping=3;c.kalman_max_velocity=30000;c.kalman_warmup_frames=4;
    c.kalman_compensate_detection_delay=true;c.kalman_additional_prediction_ms=-80;
    c.kalman_reset_timeout_sec=0.7f;c.prediction_futurePositions=36;c.draw_futurePositions=true;
    c.minSpeedMultiplier=0.5f;c.maxSpeedMultiplier=2.0f;c.snapRadius=12;c.nearRadius=80;
    c.speedCurveExponent=10;c.snapBoostFactor=2;c.fovX=110;c.fovY=75;
    c.wind_mouse_enabled=true;c.wind_G=12;c.wind_W=8;c.wind_M=15;c.wind_D=6;
    c.bScope_multiplier=0.6f;c.confidence_threshold=0.37f;
    c.depth_mask_fps=7;c.max_detections=3500;c.screenshot_delay=750;c.screenshot_button={"F6","F7"};
#ifdef USE_CUDA
    c.export_enable_fp16=true;c.export_enable_fp8=false;
#endif
    c.active_game="CUSTOM";
    c.game_profiles.emplace("CUSTOM",Config::GameProfile{"CUSTOM",1.75,0.031,0.027,true,100});
    return c;
}
const Node& first(const Workspace& workspace,NodeKind kind)
{
    for(const auto& graph:workspace.graphs)for(const auto& node:graph.nodes)if(node.kind==kind)return node;
    throw std::runtime_error("Expected node missing");
}
void testFreshAndPortable()
{
    auto c=settings();auto workspace=defaults();migrateLegacySettings(workspace,c,true);
    require(parameter(first(workspace,NodeKind::MotionEstimate),"kalman_velocity_damping")==3,"Original damping range was narrowed");
    require(parameter(first(workspace,NodeKind::PredictionTime),"kalman_additional_prediction_ms")==-80,"Negative prediction was lost");
    require(parameter(first(workspace,NodeKind::RecoilStrength),"strength")==500,"Original recoil range was narrowed");
    require(parameter(first(workspace,NodeKind::SpeedCurve),"exponent")==10,"Original speed range was narrowed");
    require(parameter(first(workspace,NodeKind::MouseProfile),"sens")==1.75,"Profile sensitivity was not captured");
    require(textParameter(first(workspace,NodeKind::MouseProfile),"profile_name")=="CUSTOM","Profile name was not captured");
    require(parameter(first(workspace,NodeKind::MouseProfile),"pitch")==0.027&&parameter(first(workspace,NodeKind::MouseProfile),"sens")==1.75,
        "Recoil did not inherit the legacy mouse calibration");
    std::string text,error;require(serialize(workspace,text,error),error.c_str());
    Workspace imported;require(deserialize(text,imported,error),error.c_str());
    c.wind_G=80;c.game_profiles["CUSTOM"].sens=25;
    migrateLegacySettings(imported,c);
    require(parameter(first(imported,NodeKind::WindMotion),"wind_G")==12,"Version 3 import read unrelated global settings");
    require(parameter(first(imported,NodeKind::MouseProfile),"sens")==1.75,"Import did not preserve independent profile");
}
Workspace legacyWorkspace()
{
    Workspace w;w.sourceVersion=2;
    for(int i=0;i<static_cast<int>(GraphCount);++i)w.graphs[i].kind=static_cast<GraphKind>(i);
    w.graphs[0].nodes={{1,NodeKind::Target,0,0},{2,NodeKind::MouseDelta,256,0}};
    return w;
}
void testLegacyDependencies()
{
    auto c=settings();auto w=legacyWorkspace();migrateLegacySettings(w,c);
    require(w.sourceVersion==CurrentFormatVersion,"Legacy version not migrated");
    require(parameter(first(w,NodeKind::Target),"legacy_offsets")==1,"Legacy aim point semantics changed");
    require(parameter(first(w,NodeKind::Target),"legacy_camera_compensation")==1,"Legacy camera correction lost");
    require(first(w,NodeKind::NormalizeFps).kind==NodeKind::NormalizeFps,"Legacy FPS normalization missing");
    bool centerConnected=false;
    for(const auto& link:w.graphs[0].links)for(const auto& node:w.graphs[0].nodes)if(node.kind==NodeKind::PointDelta&&link.toNode==node.id&&link.toPort==1)centerConnected=true;
    require(centerConnected,"Legacy Mouse Delta lost center dependency");
    std::string error;require(validate(w,error),error.c_str());
}
void testFullLegacyGraphAndZeroProfile()
{
    auto c=settings();c.game_profiles["CUSTOM"].sens=0;
    auto w=legacyWorkspace();
    for(int id=3;id<=static_cast<int>(MaxNodes);++id)w.graphs[0].nodes.push_back({id,NodeKind::Boolean});
    migrateLegacySettings(w,c);
    const auto& delta=first(w,NodeKind::SharedMouseDelta);
    require(parameter(delta,"legacy_center")==1&&parameter(delta,"legacy_fps")==1,"Full legacy graph cannot preserve old dependencies");
    require(parameter(first(w,NodeKind::MouseProfile),"sens")==0,"Zero sensitivity must stay zero, not generate movement");
    std::string error;require(validate(w,error),error.c_str());
}
void testVersionThreeAddsVisionWithoutResettingControls()
{
    auto c=settings();auto w=defaults();w.sourceVersion=3;
    for(auto& node:w.graphs[0].nodes)if(node.kind==NodeKind::PredictionTime)node.parameters["lookahead"]=0.42;
    migrateLegacySettings(w,c);
    require(parameter(first(w,NodeKind::PredictionTime),"lookahead")==0.42,"Vision migration reset node-owned control settings");
    require(parameter(first(w,NodeKind::DepthToMask),"depth_mask_fps")==7,"Legacy mask cadence was lost");
    require(parameter(first(w,NodeKind::FilterNms),"max_candidates")==3500,"NMS input limit was narrowed");
    require(parameter(first(w,NodeKind::Inference),"export_enable_fp16")==1,"Model build precision was not captured");
    require(parameter(first(w,NodeKind::ScreenshotOutput),"cooldown_ms")==750,"Screenshot cadence was lost");
    require(textParameter(first(w,NodeKind::ScreenshotOutput),"format")=="jpg","Legacy screenshot format changed");
    int keys=0;for(const auto& node:w.graphs[3].nodes)if(node.kind==NodeKind::KeyHeld&&parameter(node,"virtual_key")>0)++keys;
    require(keys==2,"Multiple screenshot hotkeys were not migrated");
    std::string error;require(validate(w,error),error.c_str());
    std::string encoded;require(serialize(w,encoded,error),error.c_str());
    Workspace imported;require(deserialize(encoded,imported,error),error.c_str());
    c.depth_mask_fps=40;c.screenshot_delay=10;migrateLegacySettings(imported,c);
    require(parameter(first(imported,NodeKind::DepthToMask),"depth_mask_fps")==7,"Portable Vision settings were overwritten");
    require(parameter(first(imported,NodeKind::ScreenshotOutput),"cooldown_ms")==750,"Portable screenshot settings were overwritten");
}
struct ModelDirectory
{
    std::filesystem::path previous=std::filesystem::current_path();
    std::filesystem::path path=std::filesystem::temp_directory_path()/("sunone_model_seed_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ModelDirectory(){std::filesystem::create_directories(path/"models");std::filesystem::current_path(path);}
    void file(const std::string& name){std::ofstream output(std::filesystem::path("models")/std::filesystem::u8path(name),std::ios::binary);output<<"model fixture";}
    ~ModelDirectory()
    {
        std::error_code error;std::filesystem::current_path(previous,error);
        const auto absolute=std::filesystem::weakly_canonical(path,error),temporary=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(),error);
        if(!error&&absolute.parent_path()==temporary&&absolute.filename().string().rfind("sunone_model_seed_",0)==0)std::filesystem::remove_all(absolute,error);
    }
};
int modelPin(const char* key)
{
    const auto& inputs=definition(NodeKind::Inference).inputs;
    for(std::size_t p=0;p<inputs.size();++p)if(inputs[p].defaultParameter&&std::string(inputs[p].defaultParameter)==key)return static_cast<int>(p);
    throw std::runtime_error("Missing model setting input");
}
void testExistingGraphsGetExplicitModelsWithoutOverridingInputs()
{
    ModelDirectory files;files.file("chosen.engine");files.file("other.engine");
    auto workspace=defaults();auto& graph=workspace.graphs[3];
    for(auto& node:graph.nodes)if(node.kind==NodeKind::Inference){node.textParameters["backend"]="TRT";node.textParameters["model_path"]="";node.parameters["confidence_threshold"]=0.042;}
    auto disabled=defaultNode(NodeKind::Inference,90);disabled.disabled=true;disabled.textParameters["backend"]="TRT";graph.nodes.push_back(disabled);
    auto explicitPath=defaultNode(NodeKind::Inference,91);explicitPath.textParameters["model_path"]="user-chosen.onnx";graph.nodes.push_back(explicitPath);
    for(int id:{92,94}){auto wired=defaultNode(NodeKind::Inference,id);wired.textParameters["backend"]="TRT";graph.nodes.push_back(wired);}
    auto path=defaultNode(NodeKind::String,93);path.textParameters["value"]="";graph.nodes.push_back(path);
    auto backend=defaultNode(NodeKind::String,95);backend.textParameters["value"]="TRT";graph.nodes.push_back(backend);
    graph.links.push_back({93,0,92,modelPin("model_path")});graph.links.push_back({95,0,94,modelPin("backend")});
    const auto old=workspace;
    require(initializeMissingModels(workspace,"chosen.engine","TRT"),"An existing workspace with an empty local path was not initialized");
    for(const auto& node:graph.nodes)if(node.kind==NodeKind::Inference)
    {
        if(node.id==2||node.id==90)require(textParameter(node,"model_path")=="chosen.engine","The configured compatible model was not written into the node");
        if(node.id==91)require(textParameter(node,"model_path")=="user-chosen.onnx","An explicit node path was replaced");
        if(node.id==92||node.id==94)require(textParameter(node,"model_path").empty(),"A wired model/backend received an implicit fallback");
    }
    require(parameter(first(workspace,NodeKind::Inference),"confidence_threshold")==0.042&&workspace.sourceVersion==old.sourceVersion&&graph.links.size()==old.graphs[3].links.size(),
        "Model initialization reset tuning, version or wiring");
    require(!initializeMissingModels(workspace,"other.engine","TRT"),"Restarting replaced a node-owned selection with a global model");
    std::string text,error;require(serialize(workspace,text,error),error.c_str());Workspace loaded;require(deserialize(text,loaded,error),error.c_str());
    require(textParameter(first(loaded,NodeKind::Inference),"model_path")=="chosen.engine","The initialized path did not survive export/import");
}
void testModelInitializationRespectsBackendAndAmbiguity()
{
    ModelDirectory files;files.file("wrong.engine");auto workspace=defaults();
    for(auto& node:workspace.graphs[3].nodes)if(node.kind==NodeKind::Inference){node.textParameters["backend"]="CPU";node.textParameters["model_path"]="";}
    require(!initializeMissingModels(workspace,"wrong.engine","DML"),"DML/CPU selected a TensorRT engine");
    files.file("a.onnx");files.file("b.onnx");
    require(!initializeMissingModels(workspace,"missing.onnx","DML"),"An ambiguous models directory chose an arbitrary network");
    require(initializeMissingModels(workspace,"b.onnx","DML")&&textParameter(first(workspace,NodeKind::Inference),"model_path")=="b.onnx", "CPU inference did not select its configured ONNX model");
    for(auto& node:workspace.graphs[3].nodes)if(node.kind==NodeKind::Inference){node.textParameters["backend"]="TRT";node.textParameters["model_path"]="";}
    require(!initializeMissingModels(workspace,"a.onnx","DML"),"A foreign backend was silently changed on load");
}
void testDepthModelsResolveSharedLegacyAndCustomLocations()
{
    ModelDirectory files;files.file("shared.onnx");files.file(u8"глубина.ONNX");
    std::string resolved,error;
    require(depth_anything::resolveModelPath("models/shared.onnx",resolved,error)&&std::filesystem::u8path(resolved)==files.path/"models/shared.onnx",
        "Depth inference still rejects a model selected from models");
    require(depth_anything::resolveModelPath("shared.onnx",resolved,error)&&std::filesystem::u8path(resolved)==files.path/"models/shared.onnx","A bare depth model cannot fall back to models");
    require(depth_anything::resolveModelPath(u8"models/глубина.ONNX",resolved,error),"Unicode depth model paths are not resolved");
    std::filesystem::create_directory("depth_models");{std::ofstream file("depth_models/shared.onnx");file<<"legacy fixture";}
    require(depth_anything::resolveModelPath("shared.onnx",resolved,error)&&std::filesystem::u8path(resolved)==files.path/"depth_models/shared.onnx","A legacy bare depth path changed its selected file");
    std::filesystem::create_directory("custom");{std::ofstream file("custom/chosen.engine");file<<"custom fixture";}
    require(depth_anything::resolveModelPath((files.path/"custom/chosen.engine").u8string(),resolved,error)&&std::filesystem::u8path(resolved)==files.path/"custom/chosen.engine","The depth file picker cannot use an explicit custom path");
    require(!depth_anything::resolveModelPath("models/missing.onnx",resolved,error)&&resolved.empty()&&!error.empty(),"A missing depth model retains a stale resolved path");
}
void testLegacyMouseProfilesBecomeDisabledCalibrationNodes()
{
    auto config=settings();config.circle_fov_enabled=true;config.circle_fov_radius_percent=37;
    config.game_profiles.emplace("SECOND",Config::GameProfile{"SECOND",2.5,0.05,0.04,false,90});
    auto workspace=defaults();migrateLegacySettings(workspace,config,true);const auto count=workspace.graphs.size();const auto nodes=workspace.graphs[0].nodes.size();
    require(importLegacyCalibrations(workspace,config)&&workspace.graphs.size()==count&&workspace.graphs[0].nodes.size()==nodes+1,"Legacy calibration created a profile workflow or lost the alternative settings");
    const auto& calibration=workspace.graphs[0].nodes.back();
    require(calibration.kind==NodeKind::MouseProfile&&calibration.disabled&&parameter(calibration,"sens")==2.5&&textParameter(calibration,"profile_name")=="SECOND","Legacy tuning was not retained in a disabled calibration node");
    require(activePage(workspace,1)==0&&parameter(first(workspace,NodeKind::MouseProfile),"sens")==1.75,"Calibration migration replaced the working settings");
    require(!importLegacyCalibrations(workspace,config),"Calibration migration duplicated an existing node");
    require(parameter(first(workspace,NodeKind::FilterCircle),"radius_percent")==37,"FOV migration lost its relative radius");
}

}
int main()
{
    try{testLegacyMouseProfilesBecomeDisabledCalibrationNodes();testFreshAndPortable();testLegacyDependencies();testFullLegacyGraphAndZeroProfile();testVersionThreeAddsVisionWithoutResettingControls();testExistingGraphsGetExplicitModelsWithoutOverridingInputs();testModelInitializationRespectsBackendAndAmbiguity();testDepthModelsResolveSharedLegacyAndCustomLocations();std::cout<<"Node config migration tests passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
