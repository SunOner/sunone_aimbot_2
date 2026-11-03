#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include "imgui/imgui.h"
#include "overlay/draw_settings.h"
#include "overlay/overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"
#include "nodes/node_graph.h"
#include "runtime/thread_loops.h"
#include "scr/data_collector.h"
#include <opencv2/core.hpp>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <vector>

void draw_debug()
{
    static NodeGraph::Workspace workspace;static NodeGraph::Telemetry telemetry;
    static std::vector<TrackDebugInfo> tracks;static int locked=-1;static double sampled=-1;
    if(ImGui::GetTime()-sampled>=0.15||sampled<0)
    {
        workspace=NodeGraph::service().workspace();telemetry=NodeGraph::service().telemetry();
        {std::lock_guard<std::mutex> lock(g_trackerDebugMutex);tracks=g_trackerDebugTracks;locked=g_trackerLockedId;}
        sampled=ImGui::GetTime();
    }
    if(OverlayUI::BeginSection("Diagnostics","diagnostic_options"))
    {
        if(OverlayUI::CheckboxRow("Verbose console output",&config.verbose))OverlayConfig_MarkDirty();
        if(ImGui::Button("OpenCV build information"))std::cout<<cv::getBuildInformation()<<std::endl;
        ImGui::PushTextWrapPos(0);ImGui::TextDisabled("Capture, recording and screenshot settings are edited in their workflow nodes.");ImGui::PopTextWrapPos();
        OverlayUI::EndSection();
    }
    if(OverlayUI::BeginSection("Active workflow nodes","diagnostic_nodes"))
    {
        bool any=false;
        for(std::size_t graph=0;graph<workspace.graphs.size();++graph)
        {
            const auto& page=workspace.graphs[graph];if(!page.active||!page.enabled)continue;
            for(const auto& node:page.nodes)
            {
                using K=NodeGraph::NodeKind;
                if(node.kind!=K::CaptureSource&&node.kind!=K::Inference&&node.kind!=K::DepthInference&&node.kind!=K::DatasetOutput&&node.kind!=K::CollectDataset&&node.kind!=K::ScreenshotOutput&&node.kind!=K::OverlayOutput)continue;
                any=true;ImGui::PushID(static_cast<int>(graph));ImGui::PushID(node.id);
                ImGui::Separator();ImGui::Text("%s / %s",NodeGraph::workflowTitle(page).c_str(),NodeGraph::pageTitle(page).c_str());
                ImGui::Text("%s #%d",NodeGraph::definition(node.kind).title,node.id);
                const NodeGraph::NodeValue* value=nullptr;
                if(telemetry.graphRevisions[graph]==NodeGraph::service().revision())
                    for(const auto& item:telemetry.graphs[graph])if(item.nodeId==node.id){value=&item;break;}
                if(node.disabled)ImGui::TextDisabled("Disabled");else if(node.bypassed)ImGui::TextDisabled("Bypassed");
                else if(value&&!value->value.error.empty()){ImGui::PushTextWrapPos(0);ImGui::TextColored(ImVec4(1,0.4f,0.4f,1),"%s",value->value.error.c_str());ImGui::PopTextWrapPos();}
                else ImGui::TextDisabled("%s",value&&value->active?"Active":"Waiting");
                if(node.kind==K::DatasetOutput||node.kind==K::CollectDataset)
                {
                    const auto state=cvm::GetDataCollectionNodeUiState(static_cast<int>(graph)*NodeGraph::NodeKeyStride+node.id);
                    ImGui::Text("Images: %llu   Labels: %llu",static_cast<unsigned long long>(state.saved_image_count),static_cast<unsigned long long>(state.saved_label_count));
                    if(!state.resolved_output_dir.empty()){ImGui::PushTextWrapPos(0);ImGui::TextUnformatted(state.resolved_output_dir.c_str());ImGui::PopTextWrapPos();}
                    if(!state.status.empty()){ImGui::PushTextWrapPos(0);ImGui::TextUnformatted(state.status.c_str());ImGui::PopTextWrapPos();}
                }
                if(ImGui::Button("Open node"))Overlay_OpenGraphNode(static_cast<int>(graph),node.id);
                ImGui::PopID();ImGui::PopID();
            }
        }
        if(!any)ImGui::TextDisabled("No source or output nodes in active pages.");
        OverlayUI::EndSection();
    }
    if(OverlayUI::BeginSection("Tracker","diagnostic_tracker"))
    {
        ImGui::Text("Latest tracker: %zu tracks / locked ID %d",tracks.size(),locked);
        if(ImGui::BeginTable("##tracks",7,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,ImVec2(0,240)))
        {
            for(const char* name:{"ID","Class","Locked","Observed","Missed","Position","Speed"})ImGui::TableSetupColumn(name);
            ImGui::TableHeadersRow();ImGuiListClipper clipper;clipper.Begin(static_cast<int>(tracks.size()));
            while(clipper.Step())for(int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i)
            {
                const auto& track=tracks[i];ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%d",track.trackId);
                ImGui::TableNextColumn();ImGui::Text("%d",track.classId);ImGui::TableNextColumn();ImGui::TextUnformatted(track.isLocked?"Yes":"No");
                ImGui::TableNextColumn();ImGui::TextUnformatted(track.observedThisFrame?"Yes":"No");ImGui::TableNextColumn();ImGui::Text("%d",track.missedFrames);
                ImGui::TableNextColumn();ImGui::Text("%.0f, %.0f",track.pivotX,track.pivotY);ImGui::TableNextColumn();ImGui::Text("%.0f",std::hypot(track.velocityX,track.velocityY));
            }
            ImGui::EndTable();
        }
        OverlayUI::EndSection();
    }
}
