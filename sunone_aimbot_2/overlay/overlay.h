#ifndef OVERLAY_H
#define OVERLAY_H

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <dcomp.h>

#include <vector>
#include <string>
#include <atomic>

#include "sunone_aimbot_2.h"
#include "window_geometry.h"

extern ID3D11Device* g_pd3dDevice;
extern ID3D11DeviceContext* g_pd3dDeviceContext;
extern IDXGISwapChain1* g_pSwapChain;
extern IDCompositionDevice* g_dcompDevice;
extern IDCompositionTarget* g_dcompTarget;
extern IDCompositionVisual* g_dcompVisual;
extern ID3D11RenderTargetView* g_mainRenderTargetView;
extern HWND g_hwnd;

void OverlayThread();
void OverlayPreview();
OverlayWindowGeometry::Mode Overlay_GetWindowMode();
void Overlay_ToggleMaximized();
void Overlay_ToggleFullscreen();
void Overlay_Hide();
void Overlay_OpenGraphNode(int graph,int node);

extern std::atomic<bool> detection_resolution_changed;
extern std::atomic<bool> capture_method_changed;
extern std::atomic<bool> capture_cursor_changed;
extern std::atomic<bool> capture_borders_changed;
extern std::atomic<bool> capture_fps_changed;
extern std::atomic<bool> capture_window_changed;
extern std::atomic<bool> detector_model_changed;

extern std::vector<std::string> key_names;
extern std::vector<const char*> key_names_cstrs;

extern const int BASE_OVERLAY_WIDTH;
extern const int BASE_OVERLAY_HEIGHT;
extern int overlayWidth;
extern int overlayHeight;
void Overlay_SetOpacity(int opacity255);
void Overlay_ApplyCaptureExclusion();

#endif // OVERLAY_H
