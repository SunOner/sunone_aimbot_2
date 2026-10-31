#include "nodes/node_config_bridge.h"
#include "config.h"
#include "keyboard/keycodes.h"
#include <algorithm>

namespace NodeGraph
{
void migrateLegacySettings(Workspace& workspace, const Config& config, bool seedDefaults)
{
    const bool legacy = workspace.sourceVersion < 3;
    const bool seedVision = workspace.sourceVersion < 4 || seedDefaults;
    if (!legacy && !seedDefaults && !seedVision) return;
    const Config::GameProfile* profile = nullptr;
    const auto active = config.game_profiles.find(config.active_game);
    if (active != config.game_profiles.end()) profile = &active->second;
    else
    {
        const auto unified = config.game_profiles.find("UNIFIED");
        if (unified != config.game_profiles.end()) profile = &unified->second;
    }
    for (auto& graph : workspace.graphs)
    {
        if(!legacy&&!seedDefaults&&graph.kind!=GraphKind::Vision)continue;
        for (auto& node : graph.nodes)
        {
            initializeParameters(node);
            const auto set = [&](const char* key, double value) {
                const auto& specs = definition(node.kind).parameters;
                const auto found = std::find_if(specs.begin(), specs.end(), [&](const ParameterSpec& spec) { return spec.key == std::string(key); });
                if (found != specs.end()) node.parameters[key] = std::clamp(value, found->minValue, found->maxValue);
            };
            const auto setText=[&](const char* key,const std::string& value){
                for(const auto& spec:definition(node.kind).textParameters)if(std::string(spec.key)==key)
                {node.textParameters[key]=value.substr(0,spec.maxLength);break;}
            };
            switch (node.kind)
            {
            case NodeKind::AutoAimEnabled: set("enabled", config.auto_aim); break;
            case NodeKind::AutoShootEnabled: set("enabled", config.auto_shoot); break;
            case NodeKind::RecoilEnabled: set("enabled", config.easynorecoil); break;
            case NodeKind::RecoilStrength: set("strength", config.easynorecoilstrength); break;
            case NodeKind::Target:
            case NodeKind::TargetNearest:
            case NodeKind::TargetTracker:
                if (seedDefaults && node.kind == NodeKind::TargetTracker && !config.tracker_enabled)
                {
                    node.kind = NodeKind::TargetNearest;
                    node.parameters.clear(); initializeParameters(node);
                }
                set("mode", node.value == 0 ? (config.tracker_enabled ? 2 : 1) : node.value);
                set("class_player", config.class_player); set("class_head", config.class_head);
                set("disable_headshot", config.disable_headshot);
                set("body_y_offset", config.body_y_offset); set("head_y_offset", config.head_y_offset);
                set("legacy_offsets", legacy && node.kind == NodeKind::Target);
                set("legacy_camera_compensation", legacy && node.kind == NodeKind::Target);
                if (legacy && node.kind == NodeKind::Target && node.value == 0) node.value = config.tracker_enabled ? 2 : 1;
                break;
            case NodeKind::FilterClasses:
                set("class_a", config.class_player); set("class_b", config.class_head);
                set("class_b_enabled", !config.disable_headshot);
                break;
            case NodeKind::PreferClass:
                set("class_id", config.class_head); set("enabled", !config.disable_headshot);
                break;
            case NodeKind::AimPoint:
                set("class_head", config.class_head);
                set("body_y_offset", config.body_y_offset); set("head_y_offset", config.head_y_offset);
                break;
            case NodeKind::Prediction:
                set("lookahead", config.predictionInterval); set("kalman_enabled", config.kalman_enabled);
                set("kalman_process_noise_position", config.kalman_process_noise_position);
                set("kalman_process_noise_velocity", config.kalman_process_noise_velocity);
                set("kalman_measurement_noise", config.kalman_measurement_noise);
                set("kalman_velocity_damping", config.kalman_velocity_damping);
                set("kalman_max_velocity", config.kalman_max_velocity);
                set("kalman_warmup_frames", config.kalman_warmup_frames);
                set("kalman_compensate_detection_delay", config.kalman_compensate_detection_delay);
                set("kalman_additional_prediction_ms", config.kalman_additional_prediction_ms);
                set("kalman_reset_timeout_sec", config.kalman_reset_timeout_sec);
                set("prediction_futurePositions", config.prediction_futurePositions);
                set("draw_futurePositions", config.draw_futurePositions || config.game_overlay_draw_future);
                break;
            case NodeKind::SpeedCurve:
                set("min_speed", config.minSpeedMultiplier); set("max_speed", config.maxSpeedMultiplier);
                set("snap_radius", config.snapRadius); set("near_radius", config.nearRadius);
                set("exponent", config.speedCurveExponent); set("snap_boost", config.snapBoostFactor);
                break;
            case NodeKind::MouseDelta:
                set("fov_x", config.fovX); set("fov_y", config.fovY);
                set("sens", profile ? profile->sens : 1); set("yaw", profile ? profile->yaw : 0.022);
                set("pitch", profile ? profile->pitch : 0.022); set("fov_scaled", profile && profile->fovScaled);
                set("base_fov", profile ? profile->baseFOV : 0);
                node.textParameters["profile_name"] = (profile ? profile->name : std::string("UNIFIED")).substr(0,128);
                break;
            case NodeKind::WindMotion:
                set("enabled", config.wind_mouse_enabled);
                set("wind_G", config.wind_G); set("wind_W", config.wind_W);
                set("wind_M", config.wind_M); set("wind_D", config.wind_D);
                break;
            case NodeKind::ScopeCheck: set("multiplier", config.bScope_multiplier); break;
            case NodeKind::FilterConfidence: set("threshold", config.confidence_threshold); break;
            case NodeKind::FilterCircle:
                set("enabled", config.circle_fov_enabled);
                set("radius", config.circle_fov_enabled ? config.detection_resolution * 0.5 * config.circle_fov_radius_percent / 100.0 : 1000000);
                break;
            case NodeKind::CaptureSource:
                set("enabled",1);set("detection_resolution",config.detection_resolution);set("capture_fps",config.capture_fps);
                set("monitor_idx",config.monitor_idx);set("capture_borders",config.capture_borders);set("capture_cursor",config.capture_cursor);
                set("virtual_camera_width",config.virtual_camera_width);set("virtual_camera_heigth",config.virtual_camera_heigth);set("udp_port",config.udp_port);
#ifdef USE_CUDA
                set("capture_use_cuda",config.capture_use_cuda);
#endif
                setText("capture_method",config.capture_method.empty()?"duplication_api":config.capture_method);
                setText("capture_target",config.capture_target.empty()?"monitor":config.capture_target);
                setText("capture_window_title",config.capture_window_title);setText("virtual_camera_name",config.virtual_camera_name);
                setText("udp_ip",config.udp_ip.empty()?"127.0.0.1":config.udp_ip);
                break;
            case NodeKind::Inference:
                setText("model_path",config.ai_model);
#ifdef USE_CUDA
                setText("backend","TRT");set("use_cuda_graph",config.use_cuda_graph);set("use_pinned_memory",config.use_pinned_memory);
                set("export_enable_fp16",config.export_enable_fp16);set("export_enable_fp8",config.export_enable_fp8);
#else
                setText("backend","DML");set("dml_device_id",config.dml_device_id);
#endif
                set("fixed_input_size",config.fixed_input_size);
                break;
            case NodeKind::DepthInference:
                set("enabled",config.depth_inference_enabled);set("depth_fps",config.depth_fps);
                setText("model_path",config.depth_model_path);
                break;
            case NodeKind::DepthToMask:
                set("enabled",config.depth_inference_enabled&&config.depth_mask_enabled);
                set("depth_mask_near_percent",config.depth_mask_near_percent);set("depth_mask_expand",config.depth_mask_expand);
                set("depth_mask_invert",config.depth_mask_invert);
                set("depth_mask_fps",config.depth_mask_fps);
                break;
            case NodeKind::ApplyMask:
            case NodeKind::FilterMask:
                set("enabled",config.depth_inference_enabled&&config.depth_mask_enabled);set("depth_mask_hold_frames",config.depth_mask_hold_frames);
                break;
            case NodeKind::FilterNms: set("threshold",config.nms_threshold);set("class_agnostic",1);set("max_candidates",config.max_detections);break;
            case NodeKind::LimitDetections: set("limit",config.max_detections);break;
            case NodeKind::CollectDataset:
                set("enabled",config.collect_data_while_playing);set("collect_only_when_aimbot_running",config.collect_only_when_aimbot_running);
                set("collect_only_when_targets_present",config.collect_only_when_targets_present);set("collect_save_every_n_frames",config.collect_save_every_n_frames);
                set("collect_jpeg_quality",config.collect_jpeg_quality);set("auto_label_data",config.auto_label_data);
                set("auto_label_min_conf",config.auto_label_min_conf);set("auto_label_max_boxes",config.auto_label_max_boxes);
                setText("collect_output_dir",config.collect_output_dir);setText("auto_label_record_classes",config.auto_label_record_classes);
                break;
            case NodeKind::ScreenshotOutput:
                set("cooldown_ms",config.screenshot_delay);setText("format","jpg");setText("output_dir","screenshots");
                break;
            case NodeKind::RenderOverlay:
                set("enabled",config.game_overlay_enabled);
#define GRAPH_SEED_FIELD(field) set(#field,config.field)
                GRAPH_SEED_FIELD(game_overlay_max_fps);GRAPH_SEED_FIELD(game_overlay_draw_boxes);GRAPH_SEED_FIELD(game_overlay_compensate_latency);
                GRAPH_SEED_FIELD(game_overlay_draw_future);GRAPH_SEED_FIELD(game_overlay_draw_wind_tail);GRAPH_SEED_FIELD(game_overlay_draw_frame);
                GRAPH_SEED_FIELD(game_overlay_draw_circle_fov);GRAPH_SEED_FIELD(game_overlay_show_target_correction);
                GRAPH_SEED_FIELD(game_overlay_box_a);GRAPH_SEED_FIELD(game_overlay_box_r);GRAPH_SEED_FIELD(game_overlay_box_g);GRAPH_SEED_FIELD(game_overlay_box_b);
                GRAPH_SEED_FIELD(game_overlay_frame_a);GRAPH_SEED_FIELD(game_overlay_frame_r);GRAPH_SEED_FIELD(game_overlay_frame_g);GRAPH_SEED_FIELD(game_overlay_frame_b);
                GRAPH_SEED_FIELD(game_overlay_box_thickness);GRAPH_SEED_FIELD(game_overlay_frame_thickness);
                GRAPH_SEED_FIELD(game_overlay_future_point_radius);GRAPH_SEED_FIELD(game_overlay_future_alpha_falloff);
                GRAPH_SEED_FIELD(game_overlay_icon_enabled);GRAPH_SEED_FIELD(game_overlay_icon_width);GRAPH_SEED_FIELD(game_overlay_icon_height);
                GRAPH_SEED_FIELD(game_overlay_icon_offset_x);GRAPH_SEED_FIELD(game_overlay_icon_offset_y);GRAPH_SEED_FIELD(game_overlay_icon_class);
                GRAPH_SEED_FIELD(circle_fov_enabled);GRAPH_SEED_FIELD(circle_fov_radius_percent);
                GRAPH_SEED_FIELD(depth_debug_overlay_enabled);GRAPH_SEED_FIELD(depth_mask_enabled);GRAPH_SEED_FIELD(depth_mask_alpha);GRAPH_SEED_FIELD(depth_colormap);
#undef GRAPH_SEED_FIELD
                set("min_speed",config.minSpeedMultiplier);set("max_speed",config.maxSpeedMultiplier);set("snap_radius",config.snapRadius);
                set("near_radius",config.nearRadius);set("exponent",config.speedCurveExponent);set("snap_boost",config.snapBoostFactor);
                setText("game_overlay_icon_path",config.game_overlay_icon_path);setText("game_overlay_icon_anchor",config.game_overlay_icon_anchor);
                break;
            default: break;
            }
        }
        if(graph.kind==GraphKind::Vision&&seedVision)
        {
            std::vector<int> keys;
            for(const auto& name:config.screenshot_button)
            {
                const int key=KeyCodes::getKeyCode(name);
                if(key>0&&key<256&&std::find(keys.begin(),keys.end(),key)==keys.end())keys.push_back(key);
            }
            std::vector<int> screenshots;
            for(const auto& node:graph.nodes)if(node.kind==NodeKind::ScreenshotOutput)screenshots.push_back(node.id);
            for(int output:screenshots)
            {
                auto input=std::find_if(graph.links.begin(),graph.links.end(),[&](const Link& link){return link.toNode==output&&link.toPort==0;});
                if(input==graph.links.end())continue;
                auto keyNode=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const Node& node){return node.id==input->fromNode&&node.kind==NodeKind::KeyHeld;});
                if(keyNode==graph.nodes.end())continue;
                keyNode->parameters["virtual_key"]=keys.empty()?0:keys.front();
                int previous=keyNode->id;
                const float y=keyNode->y;
                for(size_t k=1;k<keys.size()&&graph.nodes.size()+2<=MaxNodes&&graph.links.size()+2<=MaxLinks;++k)
                {
                    int next=1;for(const auto& node:graph.nodes)next=std::max(next,node.id+1);
                    auto key=defaultNode(NodeKind::KeyHeld,next,static_cast<float>(k*256),y+192);
                    key.parameters["virtual_key"]=keys[k];graph.nodes.push_back(std::move(key));
                    graph.nodes.push_back(defaultNode(NodeKind::Or,next+1,static_cast<float>(k*256),y+384));
                    graph.links.push_back({previous,0,next+1,0});graph.links.push_back({next,0,next+1,1});previous=next+1;
                }
                for(auto& link:graph.links)if(link.toNode==output&&link.toPort==0)link.fromNode=previous;
            }
        }
        if (legacy)
        {
            // Old Mouse Delta had an implicit center and FPS correction. Make
            // both dependencies explicit, preserving the old numeric order.
            std::vector<int> deltas;
            for (const auto& node : graph.nodes) if (node.kind == NodeKind::MouseDelta) deltas.push_back(node.id);
            auto nextId = [&]() {
                for (int candidate=1;candidate<=static_cast<int>(MaxNodes)+1;++candidate)
                    if (std::none_of(graph.nodes.begin(),graph.nodes.end(),[&](const Node& node){return node.id==candidate;})) return candidate;
                return 0;
            };
            for (int deltaId : deltas)
            {
                auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const Node& node){return node.id==deltaId;});
                const bool needsCenter=std::none_of(graph.links.begin(),graph.links.end(),[&](const Link& link){return link.toNode==deltaId&&link.toPort==2;});
                const bool enoughSpace=graph.nodes.size()+(needsCenter?3:2)<=MaxNodes&&graph.links.size()+(needsCenter?3:2)<=MaxLinks;
                if (!enoughSpace)
                {
                    found->parameters["legacy_center"]=needsCenter?1:0;
                    found->parameters["legacy_fps"]=1;
                    continue;
                }
                const float x=found->x,y=found->y;
                std::vector<int> additions;
                if(needsCenter)
                {
                    const int centerId=nextId();graph.nodes.push_back(defaultNode(NodeKind::Center,centerId,x-256,y+128));
                    graph.links.push_back({centerId,0,deltaId,2});additions.push_back(centerId);
                }
                const int fpsId=nextId();graph.nodes.push_back(defaultNode(NodeKind::CaptureFps,fpsId,x-256,y+256));
                const int normalizeId=nextId();graph.nodes.push_back(defaultNode(NodeKind::NormalizeFps,normalizeId,x,y+192));
                for(auto& link:graph.links)if(link.fromNode==deltaId)link.fromNode=normalizeId;
                graph.links.push_back({deltaId,0,normalizeId,0});graph.links.push_back({fpsId,0,normalizeId,1});
                additions.push_back(fpsId);additions.push_back(normalizeId);
                for(auto& group:graph.groups)if(std::find(group.nodes.begin(),group.nodes.end(),deltaId)!=group.nodes.end())
                {
                    group.nodes.insert(group.nodes.end(),additions.begin(),additions.end());
                    group.x=std::min(group.x,x-288);
                    group.width=std::max(group.width,x+224-group.x);
                    group.height=std::max(group.height,y+384-group.y);
                }
            }
        }
    }
    workspace.sourceVersion = CurrentFormatVersion;
}
}
