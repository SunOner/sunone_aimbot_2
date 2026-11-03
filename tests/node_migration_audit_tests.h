#pragma once
namespace
{
void testProcessingMigrationPreservesScopeAndFullGraphs()
{
    auto original=emptyWorkspace();auto& graph=original.graphs[0];
    graph.nodes={defaultNode(NodeKind::Detections,1),defaultNode(NodeKind::TargetNearest,2),defaultNode(NodeKind::ScopeCheck,3),
        defaultNode(NodeKind::Not,4),defaultNode(NodeKind::Number,5),defaultNode(NodeKind::RecoilOutput,6)};
    node(graph,5).value=1;graph.links={{1,0,2,0},{2,0,3,0},{3,0,4,0},{4,0,6,0},{5,0,6,1}};
    auto expanded=original;require(expandProcessingNodes(expanded.graphs[0]),"Simple target and scope nodes did not expand");
    Fixture fixture;fixture.context.inScope=[](const TargetData& target,double multiplier,Point center){return target.valid&&target.observed&&std::abs(target.x+target.width/2-center.x)<target.width*multiplier/2&&std::abs(target.y+target.height/2-center.y)<target.height*multiplier/2;};
    Service before,after;publish(before,original);publish(after,expanded);
    for(int scenario=0;scenario<4;++scenario)
    {
        fixture.setDetections(scenario==0?std::vector<Detection>{}:std::vector<Detection>{{10,20,20,40,0,0.9}});
        fixture.observed=scenario!=1;fixture.context.center=scenario==3?Point{1000,1000}:Point{20,40};
        const auto old=before.evaluate(fixture.context),current=after.evaluate(fixture.context);
        require(old.recoil==current.recoil&&current.recoil==(scenario!=2),"Scope migration changed missing, unobserved, inside or outside target behavior");
    }
    original=emptyWorkspace();auto& full=original.graphs[0];
    full.nodes={defaultNode(NodeKind::Center,1),defaultNode(NodeKind::Prediction,2),defaultNode(NodeKind::PointX,3),defaultNode(NodeKind::Boolean,4),defaultNode(NodeKind::RecoilOutput,5)};
    full.links={{1,0,2,0},{2,0,3,0},{4,0,5,0},{3,0,5,1}};
    for(int id=6;id<=static_cast<int>(MaxNodes);++id)full.nodes.push_back(defaultNode(NodeKind::Number,id));
    require(!expandProcessingNodes(full)&&full.nodes.size()==MaxNodes&&node(full,2).kind==NodeKind::Prediction,"A full graph lost its combined fallback");
    publish(after,original);fixture.context.center={20,40};require(after.evaluate(fixture.context).recoilStrength==20,"A full legacy graph stopped evaluating");
}
void testRecoilCalibrationAndMigration()
{
    auto workspace=emptyWorkspace();auto& graph=workspace.graphs[0];
    graph.nodes={defaultNode(NodeKind::Boolean,1),defaultNode(NodeKind::Number,2),defaultNode(NodeKind::MouseCalibration,3),defaultNode(NodeKind::RecoilOutput,4),defaultNode(NodeKind::Number,5)};
    node(graph,2).value=5;node(graph,5).value=2;
    auto& profile=node(graph,3);profile.parameters["fov_x"]=110;profile.parameters["fov_y"]=75;
    profile.parameters["sens"]=1.75;profile.parameters["yaw"]=0.031;profile.parameters["pitch"]=0.027;
    profile.parameters["fov_scaled"]=1;profile.parameters["base_fov"]=100;
    graph.links={{1,0,4,0},{2,0,4,1},{3,0,4,2}};
    Fixture fixture;fixture.context.center={160,160};Service service;publish(service,workspace);
    auto actions=service.evaluate(fixture.context);
    require(actions.recoil&&std::abs(actions.recoilPixelDeltaY-5*1.75*0.027*1.1*320/75)<1e-9,"Recoil calibration no longer matches the legacy pixel conversion");
    fixture.context.center={320,320};require(std::abs(service.evaluate(fixture.context).recoilPixelDeltaY-actions.recoilPixelDeltaY*2)<1e-9,"Recoil calibration ignored a capture size change");fixture.context.center={160,160};
    graph.links.push_back({5,0,3,optionPin(NodeKind::MouseCalibration,"sens")});publish(service,workspace);
    require(std::abs(service.evaluate(fixture.context).recoilPixelDeltaY-5*2*0.027*1.1*320/75)<1e-9,"A wire cannot override recoil calibration");
    node(graph,5).value=0;publish(service,workspace);require(service.evaluate(fixture.context).recoilPixelDeltaY==5,"Zero profile axes lost the original raw-count fallback");
    std::string text,error;Workspace restored;require(serialize(workspace,text,error)&&deserialize(text,restored,error),"Recoil calibration cannot round trip");
    publish(service,restored);require(service.evaluate(fixture.context).recoilPixelDeltaY==5,"Import changed a driven recoil calibration");
    workspace=emptyWorkspace();auto& old=workspace.graphs[0];
    old.nodes={defaultNode(NodeKind::Boolean,1),defaultNode(NodeKind::Number,2),defaultNode(NodeKind::RecoilOutput,4),defaultNode(NodeKind::MouseDelta,6)};
    node(old,2).value=5;node(old,6).parameters["sens"]=2;node(old,6).parameters["pitch"]=0.03;
    old.links={{1,0,4,0},{2,0,4,1}};
    require(serialize(workspace,text,error),"Cannot build old recoil fixture");text.replace(0,text.find('\n'),"SUNONE_NODE_GRAPH 16");
    require(deserialize(text,restored,error),"Version 16 recoil graph did not migrate");
    bool connected=false;for(const auto& link:restored.graphs[0].links)connected|=link.toNode==4&&link.toPort==2;
    require(connected,"Old recoil output did not receive an explicit calibration wire");
    publish(service,restored);require(std::abs(service.evaluate(fixture.context).recoilPixelDeltaY-5*2*0.03*320/90)<1e-9,"Old recoil lost its active mouse profile");
    while(old.nodes.size()<MaxNodes)old.nodes.push_back(defaultNode(NodeKind::Boolean,static_cast<int>(old.nodes.size())+100));
    migrateRecoilCalibration(workspace);publish(service,workspace);
    require(workspace.graphs[0].nodes.size()==MaxNodes&&std::abs(service.evaluate(fixture.context).recoilPixelDeltaY-5*2*0.03*320/90)<1e-9,"A graph at its node limit lost recoil calibration");
}
void testGeometryInputsAndLegacyPortMigration()
{
    auto workspace=emptyWorkspace();auto& graph=workspace.graphs[0];Fixture fixture;fixture.context.center={160,80};
    graph.nodes={defaultNode(NodeKind::Center,1),defaultNode(NodeKind::SpeedCurve,2),defaultNode(NodeKind::MakePoint,3),
        defaultNode(NodeKind::Number,4),defaultNode(NodeKind::Number,5),defaultNode(NodeKind::RecoilOutput,6),defaultNode(NodeKind::Boolean,7)};
    node(graph,4).value=100;node(graph,5).value=50;
    graph.links={{1,0,2,0},{4,0,3,0},{5,0,3,1},{3,0,2,1},{7,0,6,0},{2,0,6,1}};
    Point seen{};double maximum=0,minimum=0;
    fixture.context.speedCurve=[&](Point point,Point center,double distance,const Node& settings){seen=center;maximum=distance;minimum=parameter(settings,"min_speed");return std::hypot(point.x-center.x,point.y-center.y)/distance;};
    Service service;publish(service,workspace);const auto result=service.evaluate(fixture.context);
    require(result.recoil&&seen.x==100&&seen.y==50&&std::abs(maximum-std::hypot(160.0,80.0))<1e-9,"Speed curve ignores its center or automatic frame distance");
    node(graph,2).parameters["max_distance"]=200;publish(service,workspace);service.evaluate(fixture.context);
    require(maximum==200,"Speed curve still uses hidden mouse geometry instead of its distance setting");
    graph.links.erase(graph.links.begin()+3);publish(service,workspace);service.evaluate(fixture.context);
    require(seen.x==160&&seen.y==80,"An unwired Speed center no longer defaults to the capture center");
    graph.links.push_back({5,0,2,optionPin(NodeKind::SpeedCurve,"min_speed")});node(graph,5).value=2;
    std::string text,error;Workspace restored;require(serialize(workspace,text,error),"Cannot serialize geometry fixture");
    std::istringstream lines(text);std::ostringstream legacy;std::string line;
    while(std::getline(lines,line))
    {
        if(line.rfind("SUNONE_NODE_GRAPH ",0)==0)line="SUNONE_NODE_GRAPH 15";
        if(line.rfind("LINK ",0)==0)
        {
            std::istringstream row(line);std::string tag;int from,fromPin,to,toPin;row>>tag>>from>>fromPin>>to>>toPin;
            if(to==2&&toPin>=2){std::string tail;std::getline(row,tail);line="LINK "+std::to_string(from)+" "+std::to_string(fromPin)+" "+std::to_string(to)+" "+std::to_string(toPin-1)+tail;}
        }
        if(line.rfind("NODE 2 ",0)==0)
        {
            std::istringstream row(line);std::vector<std::string> fields;std::string field;while(row>>field)fields.push_back(field);
            fields[12]=std::to_string(Layout::up(Layout::minimumHeight(node(graph,2))-2*Layout::PinSpacing));line.clear();for(const auto& item:fields){if(!line.empty())line+=' ';line+=item;}
        }
        legacy<<line<<'\n';
    }
    require(deserialize(legacy.str(),restored,error),"An existing workflow with driven Speed settings no longer imports");
    require(node(restored.graphs[0],2).height>=Layout::minimumHeight(node(restored.graphs[0],2)),"Old manual card dimensions were not expanded for new ports");
    publish(service,restored);service.evaluate(fixture.context);require(minimum==2,"Speed center migration changed an existing option wire");
    graph.nodes={defaultNode(NodeKind::Detections,1),defaultNode(NodeKind::TargetNearest,2),defaultNode(NodeKind::ScopeCheck,3),
        defaultNode(NodeKind::MakePoint,4),defaultNode(NodeKind::Number,5),defaultNode(NodeKind::Number,6),defaultNode(NodeKind::RecoilOutput,7)};
    node(graph,5).value=20;node(graph,6).value=40;
    graph.links={{1,0,2,0},{2,0,3,0},{5,0,4,0},{6,0,4,1},{4,0,3,2},{3,0,7,0},{5,0,7,1}};
    fixture.context.inScope=[](const TargetData& target,double multiplier,Point center){return std::abs(target.x+target.width*0.5-center.x)<target.width*multiplier*0.5&&std::abs(target.y+target.height*0.5-center.y)<target.height*multiplier*0.5;};
    publish(service,workspace);require(service.evaluate(fixture.context).recoil,"Scope check ignores the connected center");
    graph.links.erase(graph.links.begin()+4);publish(service,workspace);require(!service.evaluate(fixture.context).recoil,"Scope check does not restore the capture center when disconnected");
}
}
