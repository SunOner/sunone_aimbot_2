#pragma once

void NodeEditor_Load();
void NodeEditor_SetPreview(bool preview);
void NodeEditor_Flush();
void NodeEditor_EndInteraction(bool preserveViewport = false);
void NodeEditor_PreserveViewport();
bool NodeEditor_ToolbarContainsPoint(float x, float y);
void NodeEditor_PrepareFrame();
void NodeEditor_RunFileActions();
void draw_node_aim();
void draw_node_auto_shoot();
void draw_node_recoil();
void draw_node_vision();
