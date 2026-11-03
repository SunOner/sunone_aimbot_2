#pragma once
namespace
{
void testSharedCalibrationAcrossWorkflows()
{
    auto workspace=emptyWorkspace();auto& aim=workspace.graphs[0];auto& recoil=workspace.graphs[2];
    aim.nodes={defaultNode(NodeKind::Boolean,1),defaultNode(NodeKind::Center,2),defaultNode(NodeKind::PointDelta,3),
        defaultNode(NodeKind::SharedCalibration,4),defaultNode(NodeKind::PixelsToCounts,5),defaultNode(NodeKind::ApplyMouseSpeed,6),
        defaultNode(NodeKind::AimOutput,7),defaultNode(NodeKind::MakePoint,8),defaultNode(NodeKind::Number,9),defaultNode(NodeKind::Number,10),
        defaultNode(NodeKind::SetCalibration,11),defaultNode(NodeKind::GetCalibration,12),defaultNode(NodeKind::RerouteCalibration,13)};
    node(aim,9).value=240;node(aim,10).value=120;node(aim,6).inputValues[1]=1.7;
    aim.links={{9,0,8,0},{10,0,8,1},{8,0,3,0},{2,0,3,1},{3,0,5,0},{4,0,11,0},{12,0,13,0},{13,0,5,1},{5,0,6,0},{1,0,7,0},{6,0,7,1}};
    recoil.nodes={defaultNode(NodeKind::Boolean,1),defaultNode(NodeKind::Number,2),defaultNode(NodeKind::SharedCalibration,3),defaultNode(NodeKind::CalibrationScale,4),defaultNode(NodeKind::RecoilOutput,5)};
    node(recoil,2).value=5;recoil.links={{1,0,5,0},{2,0,5,1},{3,0,4,0},{4,0,5,2}};
    Graph profiles;profiles.kind=GraphKind::Aim;profiles.workflow=5;profiles.workflowName="Mouse profiles";
    auto profile=defaultNode(NodeKind::MouseProfile,1);profile.parameters["fov_x"]=110;profile.parameters["fov_y"]=75;
    profile.parameters["sens"]=1.75;profile.parameters["yaw"]=0.031;profile.parameters["pitch"]=0.027;profile.parameters["base_fov"]=100;
    profile.textParameters["profile_name"]="First";profiles.nodes={profile,defaultNode(NodeKind::KeyHeld,2)};profiles.nodes[1].parameters["virtual_key"]=65;
    profiles.links={{2,0,1,optionPin(NodeKind::MouseProfile,"fov_scaled")}};workspace.graphs.push_back(profiles);
    Context context;context.center={160,160};context.captureFps=60;int reads=0;context.keyDown=[&](int key){++reads;return key==65;};
    Service service;publish(service,workspace);auto result=service.evaluate(context);
    const double expectedX=80*(110.0/320)/(1.75*0.031*1.1)*1.7;
    const double expectedScale=1.75*0.027*1.1*320/75;
    require(result.aim&&result.recoil&&std::abs(result.movement.preciseCounts.x-expectedX)<1e-9,"Shared profile changed aiming conversion");
    require(std::abs(result.recoilPixelDeltaY-5*expectedScale)<1e-9&&reads==1,"Aim and recoil evaluated different profiles or advanced shared inputs twice");
    const auto telemetry=service.telemetry();Calibration aimValue,recoilValue;
    for(const auto& value:telemetry.graphs[0])if(value.nodeId==4)aimValue=value.value.calibration;
    for(const auto& value:telemetry.graphs[2])if(value.nodeId==3)recoilValue=value.value.calibration;
    require(aimValue&&aimValue==recoilValue,"Readers copied independent calibration objects");
    reads=0;workspace.graphs[0].enabled=false;publish(service,workspace);result=service.evaluate(context);
    require(!result.aim&&result.recoil&&reads==1&&std::abs(result.recoilPixelDeltaY-5*expectedScale)<1e-9,"Disabling Aim disabled recoil's shared profile");
    workspace.graphs[0].enabled=true;auto second=profiles;second.workflow=6;second.workflowName="Alternative calibration";second.nodes[0].parameters["sens"]=3.5;second.nodes[0].textParameters["profile_name"]="Second";
    workspace.graphs[4].enabled=false;workspace.graphs.push_back(second);publish(service,workspace);result=service.evaluate(context);
    require(std::abs(result.movement.preciseCounts.x-expectedX/2)<1e-9&&std::abs(result.recoilPixelDeltaY-10*expectedScale)<1e-9,"Profile page switch did not update both consumers immediately");
    context.center={320,320};result=service.evaluate(context);
    require(std::abs(result.recoilPixelDeltaY-20*expectedScale)<1e-9,"Shared calibration ignored capture size");
    context.center={160,160};workspace.graphs.back().nodes[0].parameters["sens"]=0;publish(service,workspace);result=service.evaluate(context);
    require(result.movement.steps.empty()&&result.recoil&&result.recoilPixelDeltaY==5,"Zero sensitivity lost aim disable or raw recoil feedback");
    workspace.graphs.back().nodes[0].parameters["sens"]=3.5;
    auto& independent=workspace.graphs[1];independent.nodes={defaultNode(NodeKind::Boolean,1),defaultNode(NodeKind::Number,2),defaultNode(NodeKind::RecoilOutput,3)};
    node(independent,2).value=2;independent.links={{1,0,3,0},{2,0,3,1}};
    workspace.graphs[4].enabled=false;workspace.graphs.back().enabled=false;publish(service,workspace);result=service.evaluate(context);
    require(!result.aim&&result.recoil&&result.recoilStrength==2,"Missing profile disabled an unrelated branch or used stale calibration");
    workspace.graphs[4].enabled=false;workspace.graphs.back().enabled=true;auto duplicate=second;duplicate.workflow=7;workspace.graphs.push_back(duplicate);publish(service,workspace);result=service.evaluate(context);
    require(!result.aim&&result.recoilStrength==2,"Ambiguous profiles selected an arbitrary calibration");
    workspace.graphs.pop_back();auto& cyclic=workspace.graphs.back();cyclic.links.clear();cyclic.nodes.resize(1);
    cyclic.nodes.push_back(defaultNode(NodeKind::SharedCalibration,2));cyclic.nodes.push_back(defaultNode(NodeKind::CalibrationScale,3));cyclic.nodes.push_back(defaultNode(NodeKind::PointX,4));
    cyclic.links={{2,0,3,0},{3,0,4,0},{4,0,1,optionPin(NodeKind::MouseProfile,"sens")}};
    publish(service,workspace);result=service.evaluate(context);require(!result.aim&&result.recoilStrength==2,"Cross-workflow calibration cycle was not isolated");
    cyclic=second;publish(service,workspace);require(service.evaluate(context).aim,"A repaired profile retained invalid evaluation state");
    std::string text,error;Workspace restored;require(serialize(workspace,text,error)&&deserialize(text,restored,error),"Shared calibration did not export/import");
    publish(service,restored);require(std::abs(service.evaluate(context).recoilPixelDeltaY-(2+10*expectedScale))<1e-9,"Import lost the shared profile connection");
}
}
