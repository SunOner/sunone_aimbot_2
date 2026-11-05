#pragma once
#include <string>

void NodeEditor_Load();
void NodeEditor_RequestReload(const std::string& applicationWarning = {});
void NodeEditor_RequestTuning(int offsetDirection,int recoilDirection,bool head);
void NodeEditor_ProcessPendingActions();
std::string NodeEditor_McpCall(const std::string& request,bool userEditing=false);
void NodeEditor_SetPreview(bool preview);
void NodeEditor_Flush();
void NodeEditor_EndInteraction(bool preserveViewport = false);
void NodeEditor_PreserveViewport();
bool NodeEditor_ToolbarContainsPoint(float x, float y);
void NodeEditor_PrepareFrame();
void NodeEditor_RunFileActions();
void NodeEditor_FocusNode(int graph,int node);
// Kinds 0..3 select the standard graph types; 4 creates the prediction drawing example.
int NodeEditor_CreateWorkflow(const std::string& name,int kind,std::string& error,int icon=-1,bool filledTemplate=false);
bool NodeEditor_ConfigureWorkflow(int workflow,const std::string& name,int icon,std::string& error);
bool NodeEditor_RenameWorkflow(int workflow,const std::string& name,std::string& error);
bool NodeEditor_RemoveWorkflow(int workflow,std::string& error);
bool NodeEditor_SetWorkflowEnabled(int workflow,bool enabled,std::string& error);
void draw_node_workflow(int workflow);
void draw_node_aim();
void draw_node_auto_shoot();
void draw_node_recoil();
void draw_node_vision();
