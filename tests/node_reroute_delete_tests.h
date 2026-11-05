#pragma once
namespace
{
void testRerouteDeletionPreservesBranchesAndRoutes()
{
    using K=NodeKind;auto workspace=emptyWorkspace();auto& graph=workspace.graphs[0];
    graph.nodes={defaultNode(K::Boolean,1),defaultNode(K::RerouteBoolean,2),defaultNode(K::RerouteBoolean,3),defaultNode(K::Not,4),defaultNode(K::Not,5)};
    graph.links={{1,0,2,0},{2,0,3,0},{3,0,4,0},{2,0,5,0}};
    for(int id=1;id<=4;++id)graph.ties.push_back({id,static_cast<float>(id*32),0,0});
    graph.links[0].ties={1,2};graph.links[1].ties={2,3};graph.links[2].ties={4};graph.links[2].bend={0.42f,-0.25f};graph.links[3].ties={3};
    graph.groups={{1,"Routing",0,0,512,256,0x40ffcd60u,{2,3}}};const auto before=graph;
    removeNodes(graph,{3,2});
    require(graph.nodes.size()==3&&graph.links.size()==2&&graph.groups[0].nodes.empty(),"Reroute deletion removed a branch or left a group member");
    require(graph.links[0].fromNode==1&&graph.links[0].toNode==4&&graph.links[0].ties==std::vector<int>({1,2,3,4}),"A reroute chain did not reconnect its original source and ordered ties");
    require(graph.links[0].bend.roundness==0.42f&&graph.links[0].bend.twist==-0.25f,"Deleting a reroute replaced the destination wire's manual bend");
    require(graph.links[1].fromNode==1&&graph.links[1].toNode==5&&graph.links[1].ties==std::vector<int>({1,2,3}),"A reroute fan-out lost a destination");
    std::string error;require(validate(workspace,error),error.c_str());
    graph=before;removeNodes(graph,{2,3,4});require(graph.links.size()==1&&graph.links[0].fromNode==1&&graph.links[0].toNode==5,"Mixed deletion kept a deleted destination or lost a surviving branch");
    graph=before;removeNodes(graph,{1,2,3});require(graph.links.empty(),"Reroute healing connected a deleted source");
    graph=before;graph.links.erase(graph.links.begin());removeNodes(graph,{2,3});require(graph.links.empty(),"A reroute without an incoming source invented a connection");
    graph=before;removeNodes(graph,{4});require(graph.links.size()==3,"Ordinary node deletion unexpectedly healed a processing node");
}
void testRerouteDeletionSupportsEveryTypeAndRouteLimits()
{
    for(const auto& def:definitions())if(Layout::isReroute(def.kind))
    {
        auto workspace=emptyWorkspace();auto& graph=workspace.graphs[3];
        for(int id=1;id<=3;++id)graph.nodes.push_back(defaultNode(def.kind,id));
        graph.links={{1,0,2,0},{2,0,3,0}};removeNodes(graph,{2});
        require(graph.links.size()==1&&graph.links[0].fromNode==1&&graph.links[0].toNode==3,"A typed reroute did not preserve its wire");
        std::string error;require(validate(workspace,error),error.c_str());
    }
    auto workspace=emptyWorkspace();auto& graph=workspace.graphs[0];
    graph.nodes={defaultNode(NodeKind::Boolean,1),defaultNode(NodeKind::RerouteBoolean,2),defaultNode(NodeKind::RerouteBoolean,3),defaultNode(NodeKind::Not,4)};
    graph.links={{1,0,2,0},{2,0,3,0},{3,0,4,0}};
    for(int id=1;id<=40;++id){graph.ties.push_back({id,static_cast<float>(id*32),0,0});graph.links[std::min(2,(id-1)/16)].ties.push_back(id);}
    removeNodes(graph,{2,3});const auto& ties=graph.links[0].ties;
    require(ties.size()==MaxTiesPerLink&&ties.front()==1&&ties.back()==40&&std::adjacent_find(ties.begin(),ties.end())==ties.end(),"A long healed route exceeded its limit or lost both end waypoints");
    std::string error;require(validate(workspace,error),error.c_str());
}
}
