#pragma once
#include "nodes/node_graph.h"
#include "imgui/imgui.h"
#include <d3d11.h>

namespace NodePreview
{
ImTextureID texture(int node,const NodeGraph::Preview& image,ID3D11Device* device,ID3D11DeviceContext* context);
void retain(const std::vector<int>& nodes);
void clear();
}
