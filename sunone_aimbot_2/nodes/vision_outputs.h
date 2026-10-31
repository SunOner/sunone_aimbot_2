#pragma once
#include "nodes/node_graph.h"
#include "runtime/thread_loops.h"
#include <chrono>
#include <memory>
#include <vector>

namespace NodeVision
{
struct OverlayPacket
{
    int nodeKey = 0;
    std::uint64_t graphRevision = 0;
    NodeGraph::FrameStream frame;
    NodeGraph::FrameStream depth;
    NodeGraph::DetectionStream detections;
    NodeGraph::MaskStream mask;
    NodeGraph::Node style;
    std::vector<std::pair<double,double>> futurePositions;
    std::vector<std::pair<double,double>> windTrail;
    std::vector<TrackDebugInfo> tracks;
    int lockedTrackId = -1;
    std::chrono::steady_clock::time_point publishedAt{};
};

NodeGraph::FrameStream inferDepth(int nodeKey,const NodeGraph::FrameStream& frame,const NodeGraph::Node& node);
NodeGraph::MaskStream depthToMask(const NodeGraph::FrameStream& depth,const NodeGraph::Node& node);
NodeGraph::FrameStream applyMask(const NodeGraph::FrameStream& frame,const NodeGraph::MaskStream& mask,const NodeGraph::Node& node);
NodeGraph::DetectionStream filterMask(const NodeGraph::DetectionStream& detections,const NodeGraph::MaskStream& mask,const NodeGraph::Node& node);
bool collectDataset(int nodeKey,const NodeGraph::FrameStream& frame,const NodeGraph::DetectionStream& detections,const NodeGraph::Node& node);
bool screenshot(int nodeKey,const NodeGraph::FrameStream& frame,const NodeGraph::Node& node);
bool publishOverlay(int nodeKey,const NodeGraph::FrameStream& frame,const NodeGraph::DetectionStream& detections,const NodeGraph::MaskStream& mask,const NodeGraph::FrameStream& depth,const NodeGraph::Node& node);
bool publishOverlayAtRevision(int nodeKey,const NodeGraph::FrameStream& frame,const NodeGraph::DetectionStream& detections,const NodeGraph::MaskStream& mask,const NodeGraph::FrameStream& depth,const NodeGraph::Node& node,std::uint64_t evaluatedRevision);
std::vector<std::shared_ptr<const OverlayPacket>> overlayPackets();
void retainVisionOutputs(const std::vector<int>& depthKeys,const std::vector<int>& collectorKeys,const std::vector<int>& renderKeys);
void retainVisionOutputs(const NodeGraph::Actions& actions);
}
