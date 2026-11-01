#pragma once
#include "nodes/node_graph.h"

namespace NodeVision
{
NodeGraph::Preview makePreview(const NodeGraph::Value& image,const NodeGraph::DetectionStream& detections,const NodeGraph::Node& settings);
}
