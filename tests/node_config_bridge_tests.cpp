#include "nodes/node_config_bridge.h"
#include "config/config.h"
#include <iostream>
#include <stdexcept>

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
    require(parameter(first(workspace,NodeKind::Prediction),"kalman_velocity_damping")==3,"Original damping range was narrowed");
    require(parameter(first(workspace,NodeKind::Prediction),"kalman_additional_prediction_ms")==-80,"Negative prediction was lost");
    require(parameter(first(workspace,NodeKind::RecoilStrength),"strength")==500,"Original recoil range was narrowed");
    require(parameter(first(workspace,NodeKind::SpeedCurve),"exponent")==10,"Original speed range was narrowed");
    require(parameter(first(workspace,NodeKind::MouseDelta),"sens")==1.75,"Profile sensitivity was not captured");
    require(textParameter(first(workspace,NodeKind::MouseDelta),"profile_name")=="CUSTOM","Profile name was not captured");
    std::string text,error;require(serialize(workspace,text,error),error.c_str());
    Workspace imported;require(deserialize(text,imported,error),error.c_str());
    c.wind_G=80;c.game_profiles["CUSTOM"].sens=25;
    migrateLegacySettings(imported,c);
    require(parameter(first(imported,NodeKind::WindMotion),"wind_G")==12,"Version 3 import read unrelated global settings");
    require(parameter(first(imported,NodeKind::MouseDelta),"sens")==1.75,"Import did not preserve independent profile");
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
    for(const auto& link:w.graphs[0].links)if(link.toNode==2&&link.toPort==2)centerConnected=true;
    require(centerConnected,"Legacy Mouse Delta lost center dependency");
    std::string error;require(validate(w,error),error.c_str());
}
void testFullLegacyGraphAndZeroProfile()
{
    auto c=settings();c.game_profiles["CUSTOM"].sens=0;
    auto w=legacyWorkspace();
    for(int id=3;id<=static_cast<int>(MaxNodes);++id)w.graphs[0].nodes.push_back({id,NodeKind::Boolean});
    migrateLegacySettings(w,c);
    const auto& delta=first(w,NodeKind::MouseDelta);
    require(parameter(delta,"legacy_center")==1&&parameter(delta,"legacy_fps")==1,"Full legacy graph cannot preserve old dependencies");
    require(parameter(delta,"sens")==0,"Zero sensitivity must stay zero, not generate movement");
    std::string error;require(validate(w,error),error.c_str());
}
void testVersionThreeAddsVisionWithoutResettingControls()
{
    auto c=settings();auto w=defaults();w.sourceVersion=3;
    for(auto& node:w.graphs[0].nodes)if(node.kind==NodeKind::Prediction)node.parameters["lookahead"]=0.42;
    migrateLegacySettings(w,c);
    require(parameter(first(w,NodeKind::Prediction),"lookahead")==0.42,"Vision migration reset node-owned control settings");
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
}
int main()
{
    try{testFreshAndPortable();testLegacyDependencies();testFullLegacyGraphAndZeroProfile();testVersionThreeAddsVisionWithoutResettingControls();std::cout<<"Node config migration tests passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
