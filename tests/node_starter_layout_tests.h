#pragma once
#include <cstring>

namespace
{
std::uint64_t starterGeometryHash(const NodeGraph::Graph& graph)
{
    std::uint64_t hash=14695981039346656037ull;
    const auto byte=[&](unsigned char value){hash=(hash^value)*1099511628211ull;};
    const auto word=[&](std::uint64_t value){for(int shift=0;shift<32;shift+=8)byte(static_cast<unsigned char>(value>>shift));};
    const auto scalar=[&](float value){std::uint32_t bits=0;if(value!=0)std::memcpy(&bits,&value,sizeof(bits));word(bits);};
    const auto text=[&](const std::string& value){word(value.size());for(unsigned char c:value)byte(c);};
    word(static_cast<int>(graph.kind));scalar(graph.panX);scalar(graph.panY);scalar(graph.zoom);word(graph.nodes.size());
    for(const auto& node:graph.nodes)
    {word(node.id);word(static_cast<int>(node.kind));scalar(node.x);scalar(node.y);scalar(node.width);scalar(node.height);word(node.collapsed);}
    word(graph.links.size());
    for(const auto& link:graph.links)
    {
        word(link.fromNode);word(link.fromPort);word(link.toNode);word(link.toPort);word(link.ties.size());
        for(int tie:link.ties)word(tie);scalar(link.bend.roundness);scalar(link.bend.twist);
    }
    word(graph.groups.size());
    for(const auto& group:graph.groups)
    {
        word(group.id);text(group.title);scalar(group.x);scalar(group.y);scalar(group.width);scalar(group.height);word(group.color);word(group.nodes.size());
        for(int id:group.nodes)word(id);
    }
    word(graph.ties.size());for(const auto& tie:graph.ties){word(tie.id);scalar(tie.x);scalar(tie.y);scalar(tie.angle);}
    return hash;
}
void testStarterLayoutsAreExactAndPortable()
{
    using namespace NodeGraph;
    // Reference geometry for the four default workflows.
    const std::uint64_t expected[]={0xe4e139aed47a90ccull,0x3cdca1b0492c0d42ull,0x8c43540bee5288c8ull,0x6e297ff21a2a3ec1ull};
    const std::size_t counts[]={40,27,13,28};auto workspace=defaults();std::string document,error;Workspace restored;
    require(validate(workspace,error),error.c_str());require(serialize(workspace,document,error)&&deserialize(document,restored,error),error.c_str());
    for(std::size_t index=0;index<GraphCount;++index)
    {
        require(workspace.graphs[index].nodes.size()==counts[index]&&starterGeometryHash(workspace.graphs[index])==expected[index],"Default node geometry, routing or groups changed");
        require(starterGeometryHash(restored.graphs[index])==expected[index],"Saving and reopening changed the default layout");
        auto copy=workspace.graphs[index];require(!migrateStarterLayout(copy)&&starterGeometryHash(copy)==expected[index],"Legacy layout migration replaced a default layout");
    }
    const auto first=[&](NodeKind kind)->const Node&{for(const auto& graph:workspace.graphs)for(const auto& node:graph.nodes)if(node.kind==kind)return node;throw std::runtime_error("Missing default node");};
    require(parameter(first(NodeKind::RecoilStrength),"strength")==5,"Default recoil strength changed");
    require(textParameter(first(NodeKind::Inference),"model_path").empty()&&textParameter(first(NodeKind::DepthInference),"model_path").empty(),"Default inference model paths must be empty");
    for(const auto& node:workspace.graphs[3].nodes)
        require(node.kind!=NodeKind::DatasetOutput&&node.kind!=NodeKind::DatasetLabels&&node.kind!=NodeKind::ScreenshotOutput&&node.kind!=NodeKind::KeyHeld,"Vision contains optional recording nodes outside the default template");
    require(!definition(NodeKind::DatasetOutput).inputs.empty()&&!definition(NodeKind::ScreenshotOutput).inputs.empty(),"Optional recording nodes were removed from the library");
}
}
