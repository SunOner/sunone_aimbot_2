#pragma once
namespace
{
void testDeletingReroutesWithDeleteKeepsWiresAcrossZoom()
{
    for(float zoom:GestureZooms)
    {
        gestureScenario="Delete routing points preserves connectivity at zoom "+std::to_string(zoom);
        TemporaryWorkspace files;EditorHarness h;auto& graph=h.graph();
        graph.nodes={defaultNode(NodeKind::Boolean,1,64,64),defaultNode(NodeKind::RerouteBoolean,2,352,96),defaultNode(NodeKind::RerouteBoolean,3,512,96),defaultNode(NodeKind::Not,4,736,64)};
        graph.links={{1,0,2,0},{2,0,3,0},{3,0,4,0}};graph.groups={{1,"Routes",320,32,320,256,0x40ffcd60u,{2,3}}};graph.ties.clear();graph.panX=graph.panY=0;graph.zoom=zoom;
        edited();h.frame();h.click(add(h.origin,ImVec2(24,450)));
        editor.selection[0]={2,3};editor.selected[0]=3;editor.selectedGroup[0]=0;const auto historySize=editor.undo.size();
        h.key(ImGuiKey_Delete,true);h.key(ImGuiKey_Delete,false);
        require(graph.nodes.size()==2&&graph.links.size()==1&&graph.links[0].fromNode==1&&graph.links[0].toNode==4,"Delete broke a chain through selected reroutes");
        require(service().workspace().graphs[0].links.size()==1&&editor.undo.size()==historySize+1,"Reroute deletion was not one immediate editor transaction");
        history(false);h.frame();require(h.graph().nodes.size()==4&&h.graph().links.size()==3&&h.graph().groups[0].nodes.size()==2,"Undo did not restore reroutes, wires and group membership");
        history(true);h.frame();require(h.graph().nodes.size()==2&&h.graph().links.size()==1,"Redo broke the healed wire");
        NodeEditor_Flush();std::string document,error;Workspace restored;
        require(serialize(service().workspace(),document,error)&&deserialize(document,restored,error)&&restored.graphs[0].links.size()==1,"Healed wires did not survive export/import");
    }
}
}
