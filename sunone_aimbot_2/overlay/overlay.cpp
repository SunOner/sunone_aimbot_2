#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include <tchar.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <dcomp.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <chrono>
#include <cmath>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "overlay.h"
#include "overlay/draw_settings.h"
#include "overlay/config_dirty.h"
#include "overlay/node_editor.h"
#include "overlay/window_geometry.h"
#include "overlay/window_controls.h"
#include "mcp/workflow_host.h"
#include "mcp/application_control.h"
#include "nodes/node_graph.h"
#include "nodes/vision_runtime.h"
#include "nodes/preview_image.h"
#include <opencv2/imgproc.hpp>
#include "runtime/input_state.h"
#include "include/other_tools.h"
#include "config.h"
#include "keycodes.h"
#include "keyboard_listener.h"

#ifdef USE_CUDA
#include "trt_detector.h"
#endif

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3d11.lib")

ID3D11Device* g_pd3dDevice = NULL;
ID3D11DeviceContext* g_pd3dDeviceContext = NULL;
IDXGISwapChain1* g_pSwapChain = NULL;
IDCompositionDevice* g_dcompDevice = NULL;
IDCompositionTarget* g_dcompTarget = NULL;
IDCompositionVisual* g_dcompVisual = NULL;
ID3D11RenderTargetView* g_mainRenderTargetView = NULL;
HWND g_hwnd = NULL;

extern Config config;
extern std::mutex configMutex;
extern std::atomic<bool> shouldExit;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();

ID3D11BlendState* g_pBlendState = nullptr;

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

const int BASE_OVERLAY_WIDTH = 1180;
const int BASE_OVERLAY_HEIGHT = 740;
static const int MIN_EDITOR_OPACITY = 252;

int overlayWidth = 0;
int overlayHeight = 0;

static const int DRAG_BAR_HEIGHT_PX = 34;
static const int RESIZE_BORDER_PX = 8;

static bool g_autoResizeEnabled = true;
static ImGuiStyle g_baseStyle{};
static bool g_baseStyleReady = false;
static float g_runtimeUiScale = -1.0f;
static bool g_overlayVisible = false;
static bool OverlayOwnsForegroundInput()
{
    const HWND foreground=::GetForegroundWindow();
    return g_hwnd&&foreground&&(foreground==g_hwnd||::IsChild(g_hwnd,foreground)||
        ::GetAncestor(foreground,GA_ROOTOWNER)==g_hwnd);
}
static bool g_overlayPreview = false;
static bool g_renderingOverlayFrame = false;
static bool g_manualWindowDragActive = false;
static WPARAM g_manualWindowDragHit = HTNOWHERE;
static POINT g_manualWindowDragStartPoint{};
static RECT g_manualWindowDragStartRect{};
static int g_activeOverlayTab = 0;
static int ActiveWorkflowId(){return g_activeOverlayTab<0?-g_activeOverlayTab:g_activeOverlayTab<static_cast<int>(NodeGraph::GraphCount)?g_activeOverlayTab+1:0;}
static RECT g_sidebarToggleRect{};
static OverlayWindowControls::Strip g_windowControls;
static bool g_pendingOverlayGeometryDirty = false;
static OverlayWindowGeometry::Presentation g_windowPresentation;
enum class WindowAction { None, Maximize, Fullscreen, Hide, Show, Windowed, SetMaximized, SetFullscreen };
static int g_agentPause=-1;
static WindowAction g_windowAction = WindowAction::None;
static bool g_applyingWindowMode = false;

std::vector<std::string> key_names;
std::vector<const char*> key_names_cstrs;


static UINT GetDpiForWindowSafe(HWND hwnd);
static RECT GetOverlayWorkArea(HWND hwnd);
static void ClampOverlayToWorkArea(HWND hwnd, int& x, int& y, int& w, int& h);
static void EnsureOverlayInsideWorkArea(HWND hwnd, bool persistGeometry = false);
static bool StoreOverlayWindowGeometry(HWND hwnd, bool markDirty);
static bool ResizeOverlayBackBuffer(UINT width, UINT height);
static HRESULT RenderOverlayFrame(bool allowAutoResize, bool allowConfigSave);
static void ApplyOverlayPresentation(HWND hwnd);
static void ApplyPendingWindowAction();
static void SetOverlayVisible(bool visible);


static inline int ClampInt(int v, int lo, int hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

struct OverlayThreadConfigSnapshot
{
    std::vector<std::string> buttonOpenOverlay;
    std::vector<std::string> buttonExit;
    bool excludeFromCapture = true;
};

static OverlayThreadConfigSnapshot SnapshotOverlayThreadConfig()
{
    std::lock_guard<std::mutex> lock(configMutex);
    OverlayThreadConfigSnapshot snapshot;
    snapshot.buttonOpenOverlay = config.button_open_overlay;
    snapshot.buttonExit = config.button_exit;
    snapshot.excludeFromCapture = config.overlay_exclude_from_capture;
    return snapshot;
}

static float ComputeRuntimeUiScale()
{
    return std::clamp(config.overlay_ui_scale, 0.85f, 1.35f);
}

static void ApplyRuntimeUiScale()
{
    if (!g_baseStyleReady)
        return;

    const float targetScale = ComputeRuntimeUiScale();
    ImGuiStyle& style = ImGui::GetStyle();
    if (std::fabs(targetScale - g_runtimeUiScale) > 0.01f)
    {
        style = g_baseStyle;
        style.ScaleAllSizes(targetScale);
        g_runtimeUiScale = targetScale;
    }
    style.FontScaleMain = targetScale;
}

static void TryAutoResizeOverlay(float extraContentWidth)
{
    IM_UNUSED(extraContentWidth);
    if (!g_hwnd || !g_autoResizeEnabled)
        return;

    // Keep the editor size stable. Long model names and combo popups should clip/scroll,
    // not grow the overlay frame across the screen.
}

void Overlay_SetOpacity(int opacity255)
{
    if (!g_hwnd) return;

    opacity255 = ClampInt(opacity255, MIN_EDITOR_OPACITY, 255);

    LONG exStyle = GetWindowLong(g_hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_LAYERED) == 0)
        SetWindowLong(g_hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);

    SetLayeredWindowAttributes(g_hwnd, 0, (BYTE)opacity255, LWA_ALPHA);
}

static void Overlay_SetDisplayAffinity(HWND hwnd, bool excludeFromCapture)
{
    if (!hwnd)
        return;

    const DWORD wanted = excludeFromCapture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    if (SetWindowDisplayAffinity(hwnd, wanted))
        return;

    if (excludeFromCapture)
    {
        const DWORD err = GetLastError();
        std::cerr << "[OverlayUI] SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed, err=" << err
                  << ". Trying WDA_MONITOR fallback." << std::endl;
        if (!SetWindowDisplayAffinity(hwnd, WDA_MONITOR))
        {
            std::cerr << "[OverlayUI] SetWindowDisplayAffinity(WDA_MONITOR) failed, err="
                      << GetLastError() << std::endl;
        }
    }
}

void Overlay_ApplyCaptureExclusion()
{
    Overlay_SetDisplayAffinity(g_hwnd, config.overlay_exclude_from_capture);
}

static inline ImVec4 RGBA(int r, int g, int b, int a = 255)
{
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

static void ApplyTheme_Windows11Dark()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.48f;

    style.WindowRounding = 8.0f;
    style.ChildRounding = 8.0f;
    style.PopupRounding = 5.0f;
    style.FrameRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.ImageRounding = 6.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;

    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(8.0f, 9.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.CellPadding = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 8.0f;
    style.ScrollbarPadding = 2.0f;
    style.GrabMinSize = 12.0f;
    style.IndentSpacing = 18.0f;
    style.ColumnsMinSpacing = 10.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 2.0f;
    style.SeparatorSize = 1.0f;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    ImVec4* c = style.Colors;

    const ImVec4 surfaceBase = RGBA(32, 32, 32, 248);
    const ImVec4 surfaceRaised = RGBA(39, 39, 39, 250);
    const ImVec4 control = RGBA(45, 45, 45, 255);
    const ImVec4 controlHover = RGBA(55, 55, 55, 255);
    const ImVec4 controlActive = RGBA(65, 65, 65, 255);
    const ImVec4 stroke = RGBA(82, 82, 82, 150);
    const ImVec4 strokeHi = RGBA(112, 112, 112, 190);
    const ImVec4 accent = RGBA(96, 205, 255, 245);
    const ImVec4 accentActive = RGBA(0, 120, 212, 255);
    const ImVec4 accentSoft = RGBA(0, 120, 212, 92);

    const ImVec4 text = RGBA(245, 245, 245, 255);
    const ImVec4 textDim = RGBA(188, 188, 188, 255);

    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;

    c[ImGuiCol_WindowBg] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = RGBA(38, 39, 43, 255);

    c[ImGuiCol_Border] = stroke;
    c[ImGuiCol_BorderShadow] = RGBA(0, 0, 0, 0);

    c[ImGuiCol_FrameBg] = control;
    c[ImGuiCol_FrameBgHovered] = controlHover;
    c[ImGuiCol_FrameBgActive] = controlActive;

    c[ImGuiCol_TitleBg] = surfaceRaised;
    c[ImGuiCol_TitleBgActive] = surfaceRaised;
    c[ImGuiCol_TitleBgCollapsed] = surfaceBase;
    c[ImGuiCol_MenuBarBg] = surfaceRaised;

    c[ImGuiCol_ScrollbarBg] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = RGBA(112, 112, 112, 100);
    c[ImGuiCol_ScrollbarGrabHovered] = RGBA(132, 132, 132, 155);
    c[ImGuiCol_ScrollbarGrabActive] = accent;

    c[ImGuiCol_CheckMark] = RGBA(255, 255, 255, 255);
    c[ImGuiCol_CheckboxSelectedBg] = accentActive;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = RGBA(122, 214, 255, 255);

    c[ImGuiCol_Button] = control;
    c[ImGuiCol_ButtonHovered] = controlHover;
    c[ImGuiCol_ButtonActive] = controlActive;

    c[ImGuiCol_Header] = RGBA(38, 39, 43, 230);
    c[ImGuiCol_HeaderHovered] = RGBA(43, 45, 50, 238);
    c[ImGuiCol_HeaderActive] = accentSoft;

    c[ImGuiCol_Separator] = stroke;
    c[ImGuiCol_SeparatorHovered] = strokeHi;
    c[ImGuiCol_SeparatorActive] = accent;

    c[ImGuiCol_Tab] = RGBA(43, 43, 43, 245);
    c[ImGuiCol_TabHovered] = RGBA(55, 55, 55, 255);
    c[ImGuiCol_TabSelected] = RGBA(62, 62, 62, 255);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = RGBA(36, 36, 36, 235);
    c[ImGuiCol_TabDimmedSelected] = RGBA(50, 50, 50, 245);
    c[ImGuiCol_TabDimmedSelectedOverline] = RGBA(96, 205, 255, 170);

    c[ImGuiCol_ResizeGrip] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripActive] = RGBA(0, 0, 0, 0);

    c[ImGuiCol_InputTextCursor] = accent;
    c[ImGuiCol_PlotLines] = accent;
    c[ImGuiCol_PlotLinesHovered] = RGBA(122, 214, 255, 255);
    c[ImGuiCol_PlotHistogram] = RGBA(252, 225, 115, 255);
    c[ImGuiCol_PlotHistogramHovered] = RGBA(255, 235, 150, 255);

    c[ImGuiCol_TableHeaderBg] = surfaceRaised;
    c[ImGuiCol_TableBorderStrong] = stroke;
    c[ImGuiCol_TableBorderLight] = RGBA(70, 70, 70, 115);
    c[ImGuiCol_TableRowBg] = RGBA(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = RGBA(255, 255, 255, 10);

    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_NavWindowingHighlight] = accentSoft;
    c[ImGuiCol_NavWindowingDimBg] = RGBA(0, 0, 0, 120);

    c[ImGuiCol_TextLink] = accent;
    c[ImGuiCol_TextSelectedBg] = RGBA(0, 120, 212, 120);
    c[ImGuiCol_TreeLines] = RGBA(100, 100, 100, 115);
    c[ImGuiCol_DragDropTarget] = accent;
    c[ImGuiCol_DragDropTargetBg] = RGBA(0, 120, 212, 70);
    c[ImGuiCol_UnsavedMarker] = RGBA(252, 225, 115, 255);
    c[ImGuiCol_ModalWindowDimBg] = RGBA(0, 0, 0, 140);
}

enum class SidebarIconKind
{
    Camera,
    Chip,
    Layers,
    Crosshair,
    Move,
    Curve,
    Spark,
    User,
    Mouse,
    Keyboard,
    Sliders,
    Monitor,
    Palette,
    Image,
    Bars,
    Debug,
    Plus
};

struct OverlayTabItem
{
    const char* label;
    const char* group;
    const char* description;
    void (*draw)();
    SidebarIconKind icon;
};

static const OverlayTabItem kOverlayTabs[] = {
    { "Aim Graph",     "Nodes",   "Target, prediction and movement graph.",             draw_node_aim,                SidebarIconKind::Crosshair },
    { "Auto Shoot",    "Nodes",   "Conditions for automatic shooting.",                  draw_node_auto_shoot,         SidebarIconKind::Spark },
    { "Recoil",        "Nodes",   "Recoil conditions and compensation graph.",           draw_node_recoil,             SidebarIconKind::Curve },
    { "Vision",        "Nodes",   "Capture, inference, filtering, depth and visual outputs.",draw_node_vision,             SidebarIconKind::Camera },

    { "Input Device",  "Control", "Mouse backend, device connection and reconnect data.",draw_input_device,             SidebarIconKind::Mouse },
    { "Hotkeys",       "Control", "Bindings for aiming, shooting and runtime actions.",  draw_buttons,                 SidebarIconKind::Keyboard },
    { "Overlay",       "Control", "Overlay appearance and privacy options.",             draw_overlay,                 SidebarIconKind::Sliders },

    { "System",       "Control", "Startup CPU, memory and GPU reservations.",             draw_system,                  SidebarIconKind::Monitor },
    { "Export",        "Tools",   "Build object and depth TensorRT engines.",              draw_model_export,            SidebarIconKind::Chip },

    { "Stats",         "Monitor", "Performance, capture source and timing graphs.",      draw_stats,                   SidebarIconKind::Bars },
    { "Debug",         "Monitor", "Graph status and tracker diagnostics.",        draw_debug,                   SidebarIconKind::Debug },
};

static void DrawMainPanelBackground(const ImVec2& pos, const ImVec2& size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    draw->AddRectFilled(pos, max, IM_COL32(29, 30, 33, 252), 10.0f);
    draw->AddRectFilledMultiColor(
        ImVec2(pos.x + 1.0f, pos.y + 1.0f),
        ImVec2(max.x - 1.0f, max.y - 1.0f),
        IM_COL32(42, 45, 52, 80),
        IM_COL32(32, 33, 36, 34),
        IM_COL32(24, 25, 27, 60),
        IM_COL32(35, 38, 45, 58));
    draw->AddRect(pos, max, IM_COL32(92, 92, 92, 128), 10.0f, 0, 1.0f);
}

static void DrawSidebarIcon(ImDrawList* draw, SidebarIconKind icon, const char* group, const ImVec2& pos, bool selected)
{
    const ImU32 color = selected ? IM_COL32(96, 205, 255, 255) :
        (std::strcmp(group, "Vision") == 0 ? IM_COL32(84, 182, 255, 230) :
         std::strcmp(group, "Aim") == 0 ? IM_COL32(255, 189, 92, 230) :
         std::strcmp(group, "Control") == 0 ? IM_COL32(71, 214, 190, 230) :
         std::strcmp(group, "Visuals") == 0 ? IM_COL32(178, 143, 255, 230) :
         IM_COL32(205, 213, 224, 230));

    const float x = pos.x;
    const float y = pos.y;
    const float s = 18.0f;
    const ImVec2 c(x + s * 0.5f, y + s * 0.5f);
    const float stroke = 1.8f;
    const ImU32 soft = (color & IM_COL32_A_MASK) ? (color & 0x88FFFFFFu) : color;

    switch (icon)
    {
    case SidebarIconKind::Plus:
        draw->AddLine(ImVec2(c.x-5,c.y),ImVec2(c.x+5,c.y),color,stroke);
        draw->AddLine(ImVec2(c.x,c.y-5),ImVec2(c.x,c.y+5),color,stroke);break;
    case SidebarIconKind::Camera:
        draw->AddRect(ImVec2(x + 3.0f, y + 5.5f), ImVec2(x + 15.0f, y + 13.5f), color, 2.5f, 0, stroke);
        draw->AddRectFilled(ImVec2(x + 6.0f, y + 3.8f), ImVec2(x + 10.0f, y + 5.8f), color, 1.0f);
        draw->AddCircle(ImVec2(c.x + 1.0f, c.y + 0.5f), 2.4f, color, 20, stroke);
        break;
    case SidebarIconKind::Chip:
        draw->AddRect(ImVec2(x + 4.5f, y + 4.5f), ImVec2(x + 13.5f, y + 13.5f), color, 2.0f, 0, stroke);
        draw->AddCircleFilled(c, 2.0f, soft, 16);
        for (int i = 0; i < 3; ++i)
        {
            const float p = y + 5.5f + i * 3.5f;
            draw->AddLine(ImVec2(x + 2.0f, p), ImVec2(x + 4.5f, p), color, stroke);
            draw->AddLine(ImVec2(x + 13.5f, p), ImVec2(x + 16.0f, p), color, stroke);
        }
        break;
    case SidebarIconKind::Layers:
    {
        const ImVec2 points[] = { ImVec2(c.x, y + 3.0f), ImVec2(x + 15.0f, y + 7.0f), ImVec2(c.x, y + 11.0f), ImVec2(x + 3.0f, y + 7.0f) };
        draw->AddPolyline(points, 4, color, ImDrawFlags_Closed, stroke);
        draw->AddLine(ImVec2(x + 4.0f, y + 11.5f), ImVec2(c.x, y + 15.0f), color, stroke);
        draw->AddLine(ImVec2(x + 14.0f, y + 11.5f), ImVec2(c.x, y + 15.0f), color, stroke);
        break;
    }
    case SidebarIconKind::Crosshair:
        draw->AddCircle(c, 5.8f, color, 28, stroke);
        draw->AddLine(ImVec2(c.x - 8.0f, c.y), ImVec2(c.x - 4.0f, c.y), color, stroke);
        draw->AddLine(ImVec2(c.x + 4.0f, c.y), ImVec2(c.x + 8.0f, c.y), color, stroke);
        draw->AddLine(ImVec2(c.x, c.y - 8.0f), ImVec2(c.x, c.y - 4.0f), color, stroke);
        draw->AddLine(ImVec2(c.x, c.y + 4.0f), ImVec2(c.x, c.y + 8.0f), color, stroke);
        break;
    case SidebarIconKind::Move:
        draw->AddLine(ImVec2(c.x, y + 3.0f), ImVec2(c.x, y + 15.0f), color, stroke);
        draw->AddLine(ImVec2(x + 3.0f, c.y), ImVec2(x + 15.0f, c.y), color, stroke);
        draw->AddTriangleFilled(ImVec2(c.x, y + 2.0f), ImVec2(c.x - 2.5f, y + 5.0f), ImVec2(c.x + 2.5f, y + 5.0f), color);
        draw->AddTriangleFilled(ImVec2(c.x, y + 16.0f), ImVec2(c.x - 2.5f, y + 13.0f), ImVec2(c.x + 2.5f, y + 13.0f), color);
        break;
    case SidebarIconKind::Curve:
        draw->AddBezierCubic(ImVec2(x + 3.0f, y + 13.5f), ImVec2(x + 6.5f, y + 5.0f), ImVec2(x + 11.5f, y + 14.0f), ImVec2(x + 15.0f, y + 4.0f), color, stroke, 18);
        draw->AddCircleFilled(ImVec2(x + 15.0f, y + 4.0f), 2.0f, color, 12);
        break;
    case SidebarIconKind::Spark:
        draw->AddLine(ImVec2(c.x, y + 3.0f), ImVec2(c.x, y + 15.0f), color, stroke);
        draw->AddLine(ImVec2(x + 3.0f, c.y), ImVec2(x + 15.0f, c.y), color, stroke);
        draw->AddLine(ImVec2(x + 5.0f, y + 5.0f), ImVec2(x + 13.0f, y + 13.0f), color, stroke);
        draw->AddLine(ImVec2(x + 13.0f, y + 5.0f), ImVec2(x + 5.0f, y + 13.0f), color, stroke);
        break;
    case SidebarIconKind::User:
        draw->AddCircle(ImVec2(c.x, y + 6.0f), 3.0f, color, 18, stroke);
        draw->AddBezierCubic(ImVec2(x + 4.0f, y + 15.0f), ImVec2(x + 5.0f, y + 10.5f), ImVec2(x + 13.0f, y + 10.5f), ImVec2(x + 14.0f, y + 15.0f), color, stroke, 14);
        break;
    case SidebarIconKind::Mouse:
        draw->AddRect(ImVec2(x + 5.0f, y + 2.5f), ImVec2(x + 13.0f, y + 15.5f), color, 4.0f, 0, stroke);
        draw->AddLine(ImVec2(c.x, y + 3.0f), ImVec2(c.x, y + 7.0f), color, stroke);
        break;
    case SidebarIconKind::Keyboard:
        draw->AddRect(ImVec2(x + 2.5f, y + 5.0f), ImVec2(x + 15.5f, y + 13.0f), color, 2.0f, 0, stroke);
        for (int i = 0; i < 3; ++i)
            draw->AddCircleFilled(ImVec2(x + 5.0f + i * 4.0f, y + 8.0f), 0.9f, color, 8);
        draw->AddLine(ImVec2(x + 5.0f, y + 11.0f), ImVec2(x + 13.0f, y + 11.0f), color, stroke);
        break;
    case SidebarIconKind::Sliders:
        for (int i = 0; i < 3; ++i)
        {
            const float yy = y + 5.0f + i * 4.0f;
            draw->AddLine(ImVec2(x + 3.0f, yy), ImVec2(x + 15.0f, yy), color, stroke);
            draw->AddCircleFilled(ImVec2(x + 6.0f + i * 3.0f, yy), 1.8f, color, 14);
        }
        break;
    case SidebarIconKind::Monitor:
        draw->AddRect(ImVec2(x + 3.0f, y + 4.0f), ImVec2(x + 15.0f, y + 12.0f), color, 2.0f, 0, stroke);
        draw->AddLine(ImVec2(c.x, y + 12.0f), ImVec2(c.x, y + 15.0f), color, stroke);
        draw->AddLine(ImVec2(x + 6.0f, y + 15.0f), ImVec2(x + 12.0f, y + 15.0f), color, stroke);
        break;
    case SidebarIconKind::Palette:
        draw->AddCircle(c, 6.5f, color, 24, stroke);
        draw->AddCircleFilled(ImVec2(x + 6.0f, y + 7.0f), 1.1f, color, 8);
        draw->AddCircleFilled(ImVec2(x + 9.0f, y + 5.5f), 1.1f, color, 8);
        draw->AddCircleFilled(ImVec2(x + 12.0f, y + 8.0f), 1.1f, color, 8);
        break;
    case SidebarIconKind::Image:
    {
        draw->AddRect(ImVec2(x + 3.0f, y + 4.0f), ImVec2(x + 15.0f, y + 14.0f), color, 2.0f, 0, stroke);
        draw->AddCircleFilled(ImVec2(x + 6.0f, y + 7.0f), 1.3f, color, 10);
        const ImVec2 points[] = { ImVec2(x + 4.0f, y + 13.0f), ImVec2(x + 8.0f, y + 9.0f), ImVec2(x + 11.0f, y + 12.0f), ImVec2(x + 14.0f, y + 9.0f) };
        draw->AddPolyline(points, 4, color, 0, stroke);
        break;
    }
    case SidebarIconKind::Bars:
        draw->AddRectFilled(ImVec2(x + 4.0f, y + 10.0f), ImVec2(x + 6.5f, y + 15.0f), color, 1.0f);
        draw->AddRectFilled(ImVec2(x + 8.0f, y + 6.5f), ImVec2(x + 10.5f, y + 15.0f), color, 1.0f);
        draw->AddRectFilled(ImVec2(x + 12.0f, y + 3.5f), ImVec2(x + 14.5f, y + 15.0f), color, 1.0f);
        break;
    case SidebarIconKind::Debug:
        draw->AddRect(ImVec2(x + 5.0f, y + 5.0f), ImVec2(x + 13.0f, y + 13.5f), color, 3.0f, 0, stroke);
        draw->AddLine(ImVec2(x + 3.0f, y + 8.0f), ImVec2(x + 15.0f, y + 8.0f), color, stroke);
        draw->AddLine(ImVec2(x + 3.0f, y + 11.5f), ImVec2(x + 15.0f, y + 11.5f), color, stroke);
        break;
    }
}

struct SidebarAnimation
{
    bool initialized = false;
    bool targetCollapsed = false;
    double expansion = 1.0, velocity = 0.0;
    std::chrono::steady_clock::time_point updatedAt{};

    float update(bool collapsed)
    {
        const auto now = std::chrono::steady_clock::now();
        const double target = collapsed ? 0.0 : 1.0;
        if (!initialized)
        {
            initialized = true;
            expansion = target;
        }
        else if (expansion != target || velocity != 0.0)
        {
            // Exact critically damped motion stays consistent across frame rates
            // and retains velocity when the user reverses an unfinished transition.
            const double elapsed = std::chrono::duration<double>(now - updatedAt).count();
            constexpr double speed = 30.0;
            const double offset = expansion - target;
            const double impulse = velocity + speed * offset;
            const double decay = std::exp(-speed * elapsed);
            expansion = std::clamp(target + (offset + impulse * elapsed) * decay, 0.0, 1.0);
            velocity = (velocity - speed * impulse * elapsed) * decay;
            if (std::abs(expansion - target) < 0.001 && std::abs(velocity) < 0.03)
            {
                expansion = target;
                velocity = 0.0;
            }
        }
        targetCollapsed = collapsed;
        updatedAt = now;
        return static_cast<float>(expansion);
    }

    bool running() const { return expansion != (targetCollapsed ? 0.0 : 1.0); }
};
static bool WorkflowIconPicker(int& icon)
{
    static const char* names[]={"Camera","Chip","Layers","Crosshair","Movement","Curve","Spark","User","Mouse","Keyboard","Sliders","Monitor","Palette","Image","Bars","Debug"};
    bool changed=false;
    if(ImGui::BeginCombo("Icon",icon<0?"Automatic":names[std::clamp(icon,0,15)],ImGuiComboFlags_HeightLarge))
    {
        if(ImGui::Selectable("Automatic",icon<0)){icon=-1;changed=true;}
        for(int i=0;i<16;++i)
        {
            ImGui::PushID(i);if(ImGui::Selectable((std::string("     ")+names[i]).c_str(),icon==i)){icon=i;changed=true;}
            const auto pos=ImGui::GetItemRectMin();const float height=ImGui::GetItemRectSize().y;
            DrawSidebarIcon(ImGui::GetWindowDrawList(),static_cast<SidebarIconKind>(i),"Nodes",ImVec2(pos.x+3,pos.y+(height-18)*0.5f),false);ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

static SidebarAnimation g_sidebarAnimation;

static ImU32 SidebarFade(ImU32 color, float opacity)
{
    ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
    value.w *= opacity;
    return ImGui::ColorConvertFloat4ToU32(value);
}

static bool DrawSidebarHeader(bool collapsed, float expansion)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float size = 24.0f * g_runtimeUiScale;
    const float offset = width - size - 10.0f * g_runtimeUiScale * (1.0f - expansion);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + offset, pos.y));
    const bool pressed = ImGui::InvisibleButton("##toggle_sidebar", ImVec2(size, size));
    g_sidebarToggleRect = {static_cast<LONG>(std::floor(pos.x + offset)), static_cast<LONG>(std::floor(pos.y)),
        static_cast<LONG>(std::ceil(pos.x + offset + size)), static_cast<LONG>(std::ceil(pos.y + size))};
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered)
        draw->AddRectFilled(ImVec2(pos.x + offset, pos.y), ImVec2(pos.x + offset + size, pos.y + size), IM_COL32(54, 55, 60, 230), 5.0f);
    const ImVec2 center(pos.x + offset + size * 0.5f, pos.y + size * 0.5f);
    const float scale = g_runtimeUiScale;
    const ImU32 color = hovered ? IM_COL32(96, 205, 255, 255) : IM_COL32(177, 187, 199, 255);
    const float angle = expansion * 3.14159265f;
    const float cosine = std::cos(angle), sine = std::sin(angle);
    const auto arrowPoint = [&](float x, float y) {
        return ImVec2(center.x + (x * cosine - y * sine) * scale, center.y + (x * sine + y * cosine) * scale);
    };
    draw->AddLine(arrowPoint(-2.5f, -4.5f), arrowPoint(2.0f, 0.0f), color, 1.6f * scale);
    draw->AddLine(arrowPoint(2.0f, 0.0f), arrowPoint(-2.5f, 4.5f), color, 1.6f * scale);
    if (expansion > 0.001f && offset > 4.0f * scale)
    {
        draw->PushClipRect(pos, ImVec2(pos.x + offset - 4.0f * scale, pos.y + size), true);
        draw->AddText(ImVec2(pos.x, pos.y + (size - ImGui::GetTextLineHeight()) * 0.5f), SidebarFade(IM_COL32(177, 177, 177, 230), expansion * expansion), kOverlayTabs[0].group);
        draw->PopClipRect();
    }
    if (hovered) ImGui::SetTooltip("%s sidebar", collapsed ? "Expand" : "Collapse");
    return pressed;
}

static void DrawSidebarGroup(const char* group, float expansion)
{
    ImGui::Dummy(ImVec2(0.0f, 5.0f * g_runtimeUiScale));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 1.0f + (ImGui::GetTextLineHeight() - 1.0f) * expansion;
    const float opacity = expansion * expansion;
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (opacity < 1.0f)
    {
        const float y = pos.y + height * 0.5f;
        draw->AddLine(ImVec2(pos.x + 5.0f * g_runtimeUiScale, y), ImVec2(pos.x + width - 5.0f * g_runtimeUiScale, y), SidebarFade(ImGui::GetColorU32(ImGuiCol_Separator), 1.0f - opacity));
    }
    if (opacity > 0.001f)
    {
        draw->PushClipRect(pos, ImVec2(pos.x + width, pos.y + height), true);
        draw->AddText(pos, SidebarFade(IM_COL32(177, 177, 177, 230), opacity), group);
        draw->PopClipRect();
    }
}

static bool DrawSidebarTabButton(const OverlayTabItem& tab, bool selected, float expansion)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImGuiStyle& style = ImGui::GetStyle();
    ImVec2 size = ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() + style.ItemSpacing.y * 0.22f);
    if (size.x < 1.0f)
        size.x = 1.0f;

    const std::string id = std::string("##nav_") + tab.label;
    const bool pressed = ImGui::InvisibleButton(id.c_str(), size);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);

    ImU32 rowBg = IM_COL32(0, 0, 0, 0);
    if (selected)
        rowBg = IM_COL32(54, 55, 60, 230);
    else if (hovered)
        rowBg = IM_COL32(43, 44, 49, 212);

    if (selected || hovered)
        draw->AddRectFilled(pos, max, rowBg, 5.0f);
    if (selected)
    {
        const float markerY0 = pos.y + 7.0f;
        const float markerY1 = max.y - 7.0f;
        draw->AddRectFilled(ImVec2(pos.x + 3.0f, markerY0), ImVec2(pos.x + 6.0f, markerY1), IM_COL32(96, 205, 255, 255), 3.0f);
        draw->AddRect(pos, max, IM_COL32(255, 255, 255, 18), 5.0f, 0, 1.0f);
    }

    const float textY = pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
    const ImU32 textCol = selected ? IM_COL32(255, 255, 255, 255) : (hovered ? IM_COL32(238, 238, 238, 255) : IM_COL32(202, 202, 202, 240));
    const float closedIconX = (44.0f * g_runtimeUiScale - 18.0f) * 0.5f;
    const float iconX = closedIconX + (style.FramePadding.x + 4.0f - closedIconX) * expansion;
    DrawSidebarIcon(draw, tab.icon, tab.group, ImVec2(pos.x + iconX, pos.y + (size.y - 18.0f) * 0.5f), selected);
    const float textX = pos.x + style.FramePadding.x + 31.0f;
    const float textRight = max.x - 8.0f * g_runtimeUiScale;
    if (expansion > 0.001f && textRight > textX)
    {
        draw->PushClipRect(ImVec2(textX, pos.y), ImVec2(textRight, max.y), true);
        draw->AddText(ImVec2(textX, textY), SidebarFade(textCol, expansion * expansion), tab.label);
        draw->PopClipRect();
    }
    if (hovered && expansion < 0.95f)
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tab.label);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
        ImGui::TextDisabled("%s", tab.description);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    return pressed;
}

static UINT GetDpiForWindowSafe(HWND hwnd)
{
    UINT dpi = 96;
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        auto pGetDpiForWindow = (UINT(WINAPI*)(HWND))::GetProcAddress(user32, "GetDpiForWindow");
        if (pGetDpiForWindow)
            dpi = pGetDpiForWindow(hwnd);
    }
    return dpi ? dpi : 96;
}

static HMONITOR GetCursorMonitor()
{
    POINT pt{};
    ::GetCursorPos(&pt);
    return ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
}

static RECT GetMonitorWorkArea(HMONITOR monitor)
{
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (monitor && ::GetMonitorInfo(monitor, &mi))
        return mi.rcWork;

    RECT work{};
    if (::SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0))
        return work;
    work.right = std::max(1, ::GetSystemMetrics(SM_CXSCREEN));
    work.bottom = std::max(1, ::GetSystemMetrics(SM_CYSCREEN));
    return work;
}

static RECT GetOverlayWorkArea(HWND hwnd)
{
    return GetMonitorWorkArea(hwnd ? ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : GetCursorMonitor());
}

static OverlayWindowGeometry::Rect GeometryRect(const RECT& rect)
{
    return {static_cast<int>(rect.left), static_cast<int>(rect.top),
        static_cast<int>(rect.right - rect.left), static_cast<int>(rect.bottom - rect.top)};
}

static UINT GetDpiForMonitorSafe(HMONITOR monitor)
{
    return static_cast<UINT>(std::max(1.0f, std::round(ImGui_ImplWin32_GetDpiScaleForMonitor(monitor) * 96)));
}

static OverlayWindowGeometry::Limits GetOverlayWindowLimits(HWND hwnd)
{
    return OverlayWindowGeometry::limits(GeometryRect(GetOverlayWorkArea(hwnd)), GetDpiForWindowSafe(hwnd));
}

static void FitOverlayToMonitor(HMONITOR monitor, UINT dpi, int& x, int& y, int& w, int& h, bool center = false)
{
    const auto fitted = OverlayWindowGeometry::fit({x, y, w, h}, GeometryRect(GetMonitorWorkArea(monitor)), dpi, center);
    x = fitted.x; y = fitted.y; w = fitted.width; h = fitted.height;
}

static void ClampOverlayToWorkArea(HWND hwnd, int& x, int& y, int& w, int& h)
{
    const RECT desired{x, y, x + std::max(1, w), y + std::max(1, h)};
    const auto monitor = ::MonitorFromRect(&desired, MONITOR_DEFAULTTONEAREST);
    const UINT dpi = hwnd && monitor == ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) ?
        GetDpiForWindowSafe(hwnd) : GetDpiForMonitorSafe(monitor);
    FitOverlayToMonitor(monitor, dpi, x, y, w, h);
}

static void PlaceOverlayAtStartup(int& x, int& y, int& w, int& h)
{
    const RECT desired{x, y, x + std::max(1, w), y + std::max(1, h)};
    auto monitor = ::MonitorFromRect(&desired, MONITOR_DEFAULTTONULL);
    const bool missingMonitor = monitor == nullptr;
    if (missingMonitor) monitor = GetCursorMonitor();
    const UINT dpi = GetDpiForMonitorSafe(monitor);
    const auto bounds = OverlayWindowGeometry::limits(GeometryRect(GetMonitorWorkArea(monitor)), dpi);
    const bool oversized = w > bounds.available.width || h > bounds.available.height;
    FitOverlayToMonitor(monitor, dpi, x, y, w, h, missingMonitor || oversized);
}

static void ApplyOverlayWindowRect(HWND hwnd, const RECT& desired, UINT dpi)
{
    if (g_windowPresentation.mode != OverlayWindowGeometry::Mode::Windowed)
    {
        ApplyOverlayPresentation(hwnd);
        return;
    }
    int x = desired.left, y = desired.top, w = desired.right - desired.left, h = desired.bottom - desired.top;
    const auto monitor = ::MonitorFromRect(&desired, MONITOR_DEFAULTTONEAREST);
    FitOverlayToMonitor(monitor, dpi, x, y, w, h);
    ::SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_manualWindowDragActive)
    {
        ::GetWindowRect(hwnd, &g_manualWindowDragStartRect);
        ::GetCursorPos(&g_manualWindowDragStartPoint);
    }
    StoreOverlayWindowGeometry(hwnd, true);
}

static void MarkOverlayGeometryDirty()
{
    if (ImGui::GetCurrentContext())
    {
        OverlayConfig_MarkDirty();
    }
    else
    {
        g_pendingOverlayGeometryDirty = true;
    }
}

static bool StoreOverlayWindowGeometry(HWND hwnd, bool markDirty)
{
    if (!hwnd || g_windowPresentation.mode != OverlayWindowGeometry::Mode::Windowed)
        return false;

    RECT wndRect{};
    if (!::GetWindowRect(hwnd, &wndRect))
        return false;

    const int x = wndRect.left;
    const int y = wndRect.top;
    const int w = wndRect.right - wndRect.left;
    const int h = wndRect.bottom - wndRect.top;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        changed = config.overlay_x != x ||
                  config.overlay_y != y ||
                  config.overlay_width != w ||
                  config.overlay_height != h;

        if (changed)
        {
            config.overlay_x = x;
            config.overlay_y = y;
            config.overlay_width = w;
            config.overlay_height = h;
        }
    }

    if (changed && markDirty)
        MarkOverlayGeometryDirty();

    return changed;
}

static void EnsureOverlayInsideWorkArea(HWND hwnd, bool persistGeometry)
{
    if (!hwnd || ::IsIconic(hwnd))
        return;

    if (g_windowPresentation.mode != OverlayWindowGeometry::Mode::Windowed)
    {
        ApplyOverlayPresentation(hwnd);
        return;
    }

    RECT wndRect{};
    if (!::GetWindowRect(hwnd, &wndRect)) return;

    int x = wndRect.left;
    int y = wndRect.top;
    int w = wndRect.right - wndRect.left;
    int h = wndRect.bottom - wndRect.top;
    ClampOverlayToWorkArea(hwnd, x, y, w, h);

    overlayWidth = w;
    overlayHeight = h;

    if (x != wndRect.left || y != wndRect.top || w != wndRect.right - wndRect.left || h != wndRect.bottom - wndRect.top)
        ::SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);

    if (persistGeometry)
        StoreOverlayWindowGeometry(hwnd, true);
}

static void ApplyOverlayPresentation(HWND hwnd)
{
    const HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{}; info.cbSize = sizeof(info);
    const RECT work = GetMonitorWorkArea(monitor);
    const RECT screen = ::GetMonitorInfo(monitor, &info) ? info.rcMonitor : work;
    const auto target = g_windowPresentation.bounds(GeometryRect(work), GeometryRect(screen), GetDpiForWindowSafe(hwnd));
    RECT current{};
    if (!::GetWindowRect(hwnd, &current)) return;
    if (current.left == target.x && current.top == target.y &&
        current.right - current.left == target.width && current.bottom - current.top == target.height) return;
    const bool applying = g_applyingWindowMode;
    g_applyingWindowMode = true;
    ::SetWindowPos(hwnd, nullptr, target.x, target.y, target.width, target.height, SWP_NOZORDER | SWP_NOACTIVATE);
    g_applyingWindowMode = applying;
}

static void ApplyPendingWindowAction()
{
    const auto action = g_windowAction; g_windowAction = WindowAction::None;
    if(action==WindowAction::Show){SetOverlayVisible(true);return;}
    if (action == WindowAction::Hide) { SetOverlayVisible(false); return; }
    RECT current{};
    if (action == WindowAction::None || !g_hwnd || !::GetWindowRect(g_hwnd, &current)) return;
    NodeEditor_EndInteraction(true);
    if (g_windowPresentation.mode == OverlayWindowGeometry::Mode::Windowed)
        StoreOverlayWindowGeometry(g_hwnd, true);
    if (action == WindowAction::Maximize) g_windowPresentation.toggleMaximized(GeometryRect(current));
    else if(action==WindowAction::Fullscreen) g_windowPresentation.toggleFullscreen(GeometryRect(current));
    else
    {
        const auto desired=action==WindowAction::Windowed?OverlayWindowGeometry::Mode::Windowed:action==WindowAction::SetMaximized?OverlayWindowGeometry::Mode::Maximized:OverlayWindowGeometry::Mode::Fullscreen;
        if(g_windowPresentation.mode==desired)return;
        g_windowPresentation.setMode(desired,GeometryRect(current));
    }
    g_autoResizeEnabled = false;
    ApplyOverlayPresentation(g_hwnd);
    StoreOverlayWindowGeometry(g_hwnd, true);
}

static void SetOverlayVisible(bool visible)
{
    if (!g_hwnd || g_overlayVisible == visible) return;
    g_overlayVisible = visible;
    NodeGraph::service().setTelemetryEnabled(visible);
    if (visible)
    {
        g_autoResizeEnabled = true;
        EnsureOverlayInsideWorkArea(g_hwnd, true);
        ::ShowWindow(g_hwnd, SW_SHOW);
        ::SetForegroundWindow(g_hwnd);
    }
    else
    {
        editorInputCaptured.store(false);
        NodeEditor_EndInteraction();
        NodeEditor_Flush();
        StoreOverlayWindowGeometry(g_hwnd, true);
        if (!g_overlayPreview)
        {
            std::lock_guard<std::mutex> lock(configMutex);
            OverlayConfig_SaveNow();
        }
        ::ShowWindow(g_hwnd, SW_HIDE);
    }
}

bool InitializeBlendState()
{
    D3D11_BLEND_DESC blendDesc;
    ZeroMemory(&blendDesc, sizeof(blendDesc));

    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    HRESULT hr = g_pd3dDevice->CreateBlendState(&blendDesc, &g_pBlendState);
    if (FAILED(hr))
        return false;

    float blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
    g_pd3dDeviceContext->OMSetBlendState(g_pBlendState, blendFactor, 0xffffffff);
    return true;
}

bool CreateDeviceD3D(HWND hWnd)
{
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        ARRAYSIZE(featureLevelArray),
        D3D11_SDK_VERSION,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext);

    if (FAILED(hr))
        return false;

    IDXGIDevice* dxgiDev = nullptr;
    hr = g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    if (FAILED(hr) || !dxgiDev)
        return false;

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDev->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter)
    {
        dxgiDev->Release();
        return false;
    }

    IDXGIFactory2* factory2 = nullptr;
    {
        IDXGIFactory* baseFactory = nullptr;
        hr = adapter->GetParent(IID_PPV_ARGS(&baseFactory));
        if (FAILED(hr) || !baseFactory)
        {
            adapter->Release();
            dxgiDev->Release();
            return false;
        }
        hr = baseFactory->QueryInterface(IID_PPV_ARGS(&factory2));
        baseFactory->Release();
    }

    if (FAILED(hr) || !factory2)
    {
        adapter->Release();
        dxgiDev->Release();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = overlayWidth;
    scd.Height = overlayHeight;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    scd.Scaling = DXGI_SCALING_STRETCH;

    hr = factory2->CreateSwapChainForComposition(
        g_pd3dDevice,
        &scd,
        nullptr,
        &g_pSwapChain);

    factory2->Release();
    adapter->Release();

    if (FAILED(hr) || !g_pSwapChain)
    {
        dxgiDev->Release();
        return false;
    }

    hr = DCompositionCreateDevice(dxgiDev, IID_PPV_ARGS(&g_dcompDevice));
    dxgiDev->Release();
    if (FAILED(hr) || !g_dcompDevice)
        return false;

    hr = g_dcompDevice->CreateTargetForHwnd(hWnd, TRUE, &g_dcompTarget);
    if (FAILED(hr) || !g_dcompTarget)
        return false;

    hr = g_dcompDevice->CreateVisual(&g_dcompVisual);
    if (FAILED(hr) || !g_dcompVisual)
        return false;

    hr = g_dcompVisual->SetContent(g_pSwapChain);
    if (FAILED(hr))
        return false;

    hr = g_dcompTarget->SetRoot(g_dcompVisual);
    if (FAILED(hr))
        return false;

    g_dcompDevice->Commit();

    if (!InitializeBlendState())
        return false;

    CreateRenderTarget();
    return true;
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = NULL;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_pd3dDeviceContext)
    {
        ID3D11RenderTargetView* nullRenderTarget = nullptr;
        g_pd3dDeviceContext->OMSetRenderTargets(1, &nullRenderTarget, NULL);
    }
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}

static bool ResizeOverlayBackBuffer(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;

    overlayWidth = static_cast<int>(width);
    overlayHeight = static_cast<int>(height);

    if (!g_pd3dDevice || !g_pSwapChain)
        return true;

    CleanupRenderTarget();
    const HRESULT hr = g_pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
        return false;

    CreateRenderTarget();
    if (g_dcompDevice)
        g_dcompDevice->Commit();

    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();

    if (g_dcompVisual) { g_dcompVisual->Release(); g_dcompVisual = NULL; }
    if (g_dcompTarget) { g_dcompTarget->Release(); g_dcompTarget = NULL; }
    if (g_dcompDevice) { g_dcompDevice->Release(); g_dcompDevice = NULL; }

    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
    if (g_pBlendState) { g_pBlendState->Release(); g_pBlendState = nullptr; }
}

static bool IsOverlayResizeHit(WPARAM hit)
{
    return hit == HTLEFT || hit == HTRIGHT || hit == HTTOP || hit == HTBOTTOM ||
           hit == HTTOPLEFT || hit == HTTOPRIGHT || hit == HTBOTTOMLEFT || hit == HTBOTTOMRIGHT;
}

static bool IsOverlayMoveOrResizeHit(WPARAM hit)
{
    return hit == HTCAPTION || IsOverlayResizeHit(hit);
}

static void BeginManualOverlayWindowDrag(HWND hwnd, WPARAM hit)
{
    if (!hwnd || !IsOverlayMoveOrResizeHit(hit))
        return;

    g_manualWindowDragActive = true;
    g_manualWindowDragHit = hit;
    ::GetCursorPos(&g_manualWindowDragStartPoint);
    ::GetWindowRect(hwnd, &g_manualWindowDragStartRect);
    ::SetCapture(hwnd);

    if (IsOverlayResizeHit(hit))
        g_autoResizeEnabled = false;
}

static void EndManualOverlayWindowDrag(HWND hwnd)
{
    if (!g_manualWindowDragActive)
        return;

    g_manualWindowDragActive = false;
    g_manualWindowDragHit = HTNOWHERE;

    if (::GetCapture() == hwnd)
        ::ReleaseCapture();

    EnsureOverlayInsideWorkArea(hwnd, true);
}

static void UpdateManualOverlayWindowDrag(HWND hwnd)
{
    if (!hwnd || !g_manualWindowDragActive)
        return;

    POINT pt{};
    ::GetCursorPos(&pt);

    const int dx = pt.x - g_manualWindowDragStartPoint.x;
    const int dy = pt.y - g_manualWindowDragStartPoint.y;
    const int startW = g_manualWindowDragStartRect.right - g_manualWindowDragStartRect.left;
    const int startH = g_manualWindowDragStartRect.bottom - g_manualWindowDragStartRect.top;

    int x = g_manualWindowDragStartRect.left;
    int y = g_manualWindowDragStartRect.top;
    int w = startW;
    int h = startH;

    if (g_manualWindowDragHit == HTCAPTION)
    {
        x += dx;
        y += dy;
        ClampOverlayToWorkArea(hwnd, x, y, w, h);
    }
    else
    {
        const auto bounds = GetOverlayWindowLimits(hwnd);
        const int minW = bounds.minWidth, minH = bounds.minHeight;
        const int maxW = bounds.available.width, maxH = bounds.available.height;

        const bool left = g_manualWindowDragHit == HTLEFT ||
                          g_manualWindowDragHit == HTTOPLEFT ||
                          g_manualWindowDragHit == HTBOTTOMLEFT;
        const bool right = g_manualWindowDragHit == HTRIGHT ||
                           g_manualWindowDragHit == HTTOPRIGHT ||
                           g_manualWindowDragHit == HTBOTTOMRIGHT;
        const bool top = g_manualWindowDragHit == HTTOP ||
                         g_manualWindowDragHit == HTTOPLEFT ||
                         g_manualWindowDragHit == HTTOPRIGHT;
        const bool bottom = g_manualWindowDragHit == HTBOTTOM ||
                            g_manualWindowDragHit == HTBOTTOMLEFT ||
                            g_manualWindowDragHit == HTBOTTOMRIGHT;

        if (left)
        {
            w = ClampInt(startW - dx, minW, maxW);
            x = g_manualWindowDragStartRect.right - w;
        }
        else if (right)
        {
            w = ClampInt(startW + dx, minW, maxW);
        }

        if (top)
        {
            h = ClampInt(startH - dy, minH, maxH);
            y = g_manualWindowDragStartRect.bottom - h;
        }
        else if (bottom)
        {
            h = ClampInt(startH + dy, minH, maxH);
        }

        ClampOverlayToWorkArea(hwnd, x, y, w, h);
    }

    ::SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if(msg==WM_ACTIVATE)
    {
        if(LOWORD(wParam)==WA_INACTIVE)NodeEditor_EndInteraction();
        editorInputCaptured.store(g_overlayVisible&&LOWORD(wParam)!=WA_INACTIVE);
    }
    else if(msg==WM_DESTROY)
        editorInputCaptured.store(false);
    switch (msg)
    {
        case WM_CLOSE:
            Overlay_Hide();
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_MINIMIZE || (wParam & 0xfff0) == SC_CLOSE)
            {
                Overlay_Hide();
                return 0;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wParam == VK_F11 && !(lParam & (1LL << 30)))
            {
                Overlay_ToggleFullscreen();
                return 0;
            }
            if (g_overlayPreview)
            {
                const auto snapshot = SnapshotOverlayThreadConfig();
                for (const auto& key : snapshot.buttonExit)
                {
                    const int code = KeyCodes::getKeyCode(key);
                    if (code != -1 && static_cast<WPARAM>(code) == wParam)
                    {
                        shouldExit = true;
                        ::PostQuitMessage(0);
                        return 0;
                    }
                }
            }
            break;

        case WM_NCHITTEST:
        {
            POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            ::ScreenToClient(hWnd, &pt);

            RECT rc;
            ::GetClientRect(hWnd, &rc);

            if (g_windowPresentation.mode != OverlayWindowGeometry::Mode::Windowed) return HTCLIENT;
            if (g_windowControls.contains(static_cast<float>(pt.x), static_cast<float>(pt.y))) return HTCLIENT;

            const UINT dpi = GetDpiForWindowSafe(hWnd);
            const int border = ::MulDiv(RESIZE_BORDER_PX, (int)dpi, 96);
            const bool left = pt.x < rc.left + border;
            const bool right = pt.x >= rc.right - border;
            const bool top = pt.y < rc.top + border;
            const bool bottom = pt.y >= rc.bottom - border;

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;

            if (pt.y >= rc.top && pt.y < rc.top + DRAG_BAR_HEIGHT_PX)
            {
                if (::PtInRect(&g_sidebarToggleRect, pt) ||
                    (ActiveWorkflowId()!=0 && NodeEditor_ToolbarContainsPoint(static_cast<float>(pt.x), static_cast<float>(pt.y))))
                    return HTCLIENT;
                return HTCAPTION;
            }

            return HTCLIENT;
        }
        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            const auto bounds = GetOverlayWindowLimits(hWnd);
            mmi->ptMinTrackSize.x = bounds.minWidth;
            mmi->ptMinTrackSize.y = bounds.minHeight;
            mmi->ptMaxTrackSize.x = bounds.available.width;
            mmi->ptMaxTrackSize.y = bounds.available.height;
            if (g_windowPresentation.mode != OverlayWindowGeometry::Mode::Windowed)
            {
                MONITORINFO info{}; info.cbSize = sizeof(info);
                if (::GetMonitorInfo(::MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &info))
                {
                    mmi->ptMaxTrackSize.x = info.rcMonitor.right - info.rcMonitor.left;
                    mmi->ptMaxTrackSize.y = info.rcMonitor.bottom - info.rcMonitor.top;
                }
            }
            return 0;
        }
        case WM_NCLBUTTONDOWN:
            if (IsOverlayMoveOrResizeHit(wParam))
            {
                BeginManualOverlayWindowDrag(hWnd, wParam);
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
            if (g_manualWindowDragActive)
            {
                UpdateManualOverlayWindowDrag(hWnd);
                return 0;
            }
            break;

        case WM_LBUTTONUP:
        case WM_NCLBUTTONUP:
            if (g_manualWindowDragActive)
            {
                EndManualOverlayWindowDrag(hWnd);
                return 0;
            }
            break;

        case WM_CAPTURECHANGED:
            if (g_manualWindowDragActive)
            {
                g_manualWindowDragActive = false;
                g_manualWindowDragHit = HTNOWHERE;
                EnsureOverlayInsideWorkArea(hWnd, true);
                return 0;
            }
            break;
    }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_EXITSIZEMOVE:
        g_autoResizeEnabled = false;
        EnsureOverlayInsideWorkArea(hWnd, true);
        return 0;

    case WM_DISPLAYCHANGE:
        EnsureOverlayInsideWorkArea(hWnd, true);
        return 0;

    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETWORKAREA)
        {
            EnsureOverlayInsideWorkArea(hWnd, true);
            return 0;
        }
        return ::DefWindowProc(hWnd, msg, wParam, lParam);

    case WM_DPICHANGED:
        if (lParam) ApplyOverlayWindowRect(hWnd, *reinterpret_cast<const RECT*>(lParam), LOWORD(wParam));
        else EnsureOverlayInsideWorkArea(hWnd, true);
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            const UINT width = (UINT)LOWORD(lParam);
            const UINT height = (UINT)HIWORD(lParam);

            if (ResizeOverlayBackBuffer(width, height) && g_overlayVisible && !g_applyingWindowMode)
                RenderOverlayFrame(false, false);
        }
        return 0;

    case WM_DESTROY:
        shouldExit = true;
        ::PostQuitMessage(0);
        return 0;

    default:
        return ::DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

void SetupImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetStyle().FontScaleMain = 1.0f;
    ImFontConfig fontConfig{};
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 2;
    fontConfig.PixelSnapH = true;
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 16.5f, &fontConfig) &&
        !io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\SegUIVar.ttf", 16.5f, &fontConfig) &&
        !io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.5f, &fontConfig))
    {
        io.Fonts->AddFontDefault();
    }

    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ApplyTheme_Windows11Dark();
    g_baseStyle = ImGui::GetStyle();
    g_baseStyleReady = true;
    g_runtimeUiScale = -1.0f;

    if (g_pendingOverlayGeometryDirty)
    {
        g_pendingOverlayGeometryDirty = false;
        OverlayConfig_MarkDirty();
    }
}

bool CreateOverlayWindow()
{
    int overlayX = config.overlay_x;
    int overlayY = config.overlay_y;
    overlayWidth = config.overlay_width > 0 ? config.overlay_width : BASE_OVERLAY_WIDTH;
    overlayHeight = config.overlay_height > 0 ? config.overlay_height : BASE_OVERLAY_HEIGHT;

    PlaceOverlayAtStartup(overlayX, overlayY, overlayWidth, overlayHeight);

    WNDCLASSEX wc = {
        sizeof(WNDCLASSEX),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        GetModuleHandle(NULL),
        NULL,
        NULL,
        NULL,
        NULL,
        _T("Chrome"),
        NULL
    };
    ::RegisterClassEx(&wc);

    // The standalone test editor is a normal, discoverable app window.
    const DWORD exStyle = WS_EX_TOPMOST | (g_overlayPreview ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW) | WS_EX_LAYERED;
    const DWORD style = WS_POPUP;

    RECT wr = { overlayX, overlayY, overlayX + overlayWidth, overlayY + overlayHeight };
    ::AdjustWindowRectEx(&wr, style, FALSE, exStyle);

    const int wndW = wr.right - wr.left;
    const int wndH = wr.bottom - wr.top;

    g_hwnd = ::CreateWindowEx(
        exStyle,
        wc.lpszClassName, g_overlayPreview ? _T("Node editor preview") : _T("Chrome"),
        style,
        wr.left, wr.top, wndW, wndH,
        NULL, NULL, wc.hInstance, NULL);

    if (g_hwnd == NULL)
        return false;

    EnsureOverlayInsideWorkArea(g_hwnd, true);

    BOOL dwm = FALSE;
    if (SUCCEEDED(DwmIsCompositionEnabled(&dwm)) && dwm)
    {
        MARGINS m = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(g_hwnd, &m);
    }

    if (config.overlay_opacity < MIN_EDITOR_OPACITY)  config.overlay_opacity = MIN_EDITOR_OPACITY;
    if (config.overlay_opacity >= 256) config.overlay_opacity = 255;

    Overlay_SetOpacity(config.overlay_opacity);

    if (!CreateDeviceD3D(g_hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        return false;
    }

    Overlay_ApplyCaptureExclusion();

    return true;
}

static HRESULT RenderOverlayFrame(bool allowAutoResize, bool allowConfigSave)
{
    if (!g_overlayVisible || !g_pSwapChain || !g_pd3dDeviceContext || !g_mainRenderTargetView ||
        !ImGui::GetCurrentContext() || g_renderingOverlayFrame)
    {
        return S_FALSE;
    }

    const float w = static_cast<float>(overlayWidth);
    const float h = static_cast<float>(overlayHeight);
    if (w <= 0.0f || h <= 0.0f)
        return S_FALSE;

    g_renderingOverlayFrame = true;

    ApplyRuntimeUiScale();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    NodeEditor_PrepareFrame();
    ImGui::NewFrame();
    editorInputCaptured.store(g_overlayVisible&&OverlayOwnsForegroundInput());

    float sidebarLabelWidth = 0.0f;
    for (const auto& tab : kOverlayTabs)
        sidebarLabelWidth = std::max(sidebarLabelWidth, ImGui::CalcTextSize(tab.label).x);
    const float expandedSidebarWidth = std::ceil(sidebarLabelWidth + ImGui::GetStyle().FramePadding.x + 31.0f + 12.0f * g_runtimeUiScale);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * g_runtimeUiScale, 8.0f * g_runtimeUiScale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::Begin("##editor_root", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    DrawMainPanelBackground(ImGui::GetWindowPos(), ImGui::GetWindowSize());

    {
        std::lock_guard<std::mutex> lock(configMutex);

        if (g_overlayPreview)
        {
            // Exercise the graph and its live visual state with synthetic
            // observations. No capture, input device or mouse worker exists.
            NodeGraph::Context preview;
            preview.elapsedSeconds = ImGui::GetTime();
            preview.aimHeld = true;
            preview.shootHeld = true;
            preview.zoomHeld = true;
            preview.detectionsAvailable = true;
            preview.captureFps = 120;
            preview.center = {160,160};
            const double t=preview.elapsedSeconds;
            auto frame=std::make_shared<NodeGraph::DetectionFrame>();
            frame->version=static_cast<std::int64_t>(t*120);frame->fresh=true;
            frame->timestampSeconds=t;frame->width=frame->height=320;
            const double x=180+std::sin(t*0.8)*40,y=160+std::cos(t*0.6)*24;
            frame->objects.push_back({x-25,y-40,50,80,0,0.96});
            frame->objects.push_back({x-10,y-46,20,20,1,0.91});
            NodeGraph::Context visionPreview;
            visionPreview.elapsedSeconds=t;visionPreview.center=preview.center;
            visionPreview.captureFps=preview.captureFps;visionPreview.controlActive=true;
            const auto clockTime=std::chrono::steady_clock::now();
            cv::Mat demo(320,320,CV_8UC3,cv::Scalar(35,45,60));
            for(int at=0;at<320;at+=32){cv::line(demo,{at,0},{at,319},{54,64,78});cv::line(demo,{0,at},{319,at},{54,64,78});}
            cv::rectangle(demo,cv::Rect(static_cast<int>(x-25),static_cast<int>(y-40),50,80),cv::Scalar(95,170,220),cv::FILLED);
            const auto image=NodeVision::makeFrame(demo,{},clockTime,0);
            visionPreview.frame=image;
            visionPreview.captureFrame=[image](int,const NodeGraph::Node&){return image;};
            visionPreview.preview=NodeVision::makePreview;
            visionPreview.infer=[frame](int,const NodeGraph::FrameStream& input,const NodeGraph::Node& node){
                auto result=std::make_shared<NodeGraph::DetectionFrame>(*frame);
                result->timestampSeconds=input->timestampSeconds;result->version=input->version;result->generation=input->generation;
                result->modelName=NodeGraph::textParameter(node,"model_path");return result;
            };
            visionPreview.depth=[](int,const NodeGraph::FrameStream& input,const NodeGraph::Node&){
                auto native=std::make_shared<NodeVision::FrameStorage>();native->image=cv::Mat(input->height,input->width,CV_32FC1);
                for(int y=0;y<input->height;++y)for(int x=0;x<input->width;++x)native->image.at<float>(y,x)=static_cast<float>(std::hypot(x-input->width*0.5,y-input->height*0.5));
                auto depth=std::make_shared<NodeGraph::FrameData>(*input);depth->gpu=false;depth->storage=native;return depth;
            };
            visionPreview.depthToMask=[](const NodeGraph::FrameStream& input,const NodeGraph::Node&){
                auto native=std::make_shared<NodeVision::MaskStorage>();native->mask=cv::Mat::zeros(input->height,input->width,CV_8UC1);
                cv::circle(native->mask,{input->width/2,input->height/2},std::min(input->width,input->height)/3,cv::Scalar(255),cv::FILLED);
                auto mask=std::make_shared<NodeGraph::MaskData>();mask->width=input->width;mask->height=input->height;
                mask->version=input->version;mask->generation=input->generation;mask->timestampSeconds=input->timestampSeconds;mask->storage=native;return mask;
            };
            visionPreview.applyMask=[](const NodeGraph::FrameStream& input,const NodeGraph::MaskStream&,const NodeGraph::Node&){return input;};
            visionPreview.filterMask=[](const NodeGraph::DetectionStream& input,const NodeGraph::MaskStream&,const NodeGraph::Node&){return input;};
            // Preview evaluates data flow, never dataset writes or real overlays.
            visionPreview.collect=[](int,const NodeGraph::FrameStream&,const NodeGraph::DetectionStream&,const NodeGraph::Node&){return true;};
            visionPreview.screenshot=[](int,const NodeGraph::FrameStream&,const NodeGraph::Node&){return true;};
            visionPreview.render=[](int,const NodeGraph::FrameStream&,const NodeGraph::DetectionStream&,const NodeGraph::MaskStream&,const NodeGraph::FrameStream&,const NodeGraph::Node&){return true;};
            visionPreview.keyDown=[](int key){return (GetAsyncKeyState(key)&0x8000)!=0;};
            preview.detections=NodeGraph::service().evaluateVision(visionPreview).visionDetections;
            preview.selectTarget = [](int nodeKey,const NodeGraph::DetectionStream& stream,bool,const NodeGraph::Node& node) {
                NodeGraph::TargetData target;
                if(!stream||stream->objects.empty())return target;
                const NodeGraph::Detection* selected=&stream->objects.front();
                for(const auto& item:stream->objects)
                    if(std::hypot(item.x+item.width*0.5-160,item.y+item.height*0.5-160)<
                       std::hypot(selected->x+selected->width*0.5-160,selected->y+selected->height*0.5-160))selected=&item;
                target.x=selected->x;target.y=selected->y;target.width=selected->width;target.height=selected->height;
                target.point={target.x+target.width*0.5,target.y+target.height*0.5};
                target.classId=selected->classId;target.confidence=selected->confidence;target.trackId=nodeKey;
                target.valid=target.observed=target.fresh=true;
                if(node.kind==NodeGraph::NodeKind::Target&&NodeGraph::parameter(node,"legacy_offsets")!=0)
                {
                    const bool head=target.classId==static_cast<int>(NodeGraph::parameter(node,"class_head"));
                    target.point.y=target.y+target.height*NodeGraph::parameter(node,head?"head_y_offset":"body_y_offset");
                    target.pointPrepared=true;
                }
                return target;
            };
            preview.inScope=[](const NodeGraph::TargetData& target,double multiplier,NodeGraph::Point center) {
                return target.valid&&std::abs(target.x+target.width*0.5-center.x)<target.width*multiplier*0.5&&
                       std::abs(target.y+target.height*0.5-center.y)<target.height*multiplier*0.5;
            };
            preview.predict=[t](int,NodeGraph::Point point,const NodeGraph::Node&,double lookahead,bool) {
                return NodeGraph::Point{point.x+std::cos(t*0.8)*32*lookahead,point.y-std::sin(t*0.6)*14.4*lookahead};
            };
            preview.estimateMotion=[t](int,NodeGraph::Point point,const NodeGraph::Node& node,bool reset){
                return NodeGraph::MotionData{point,{std::cos(t*0.8)*32,-std::sin(t*0.6)*14.4},NodeGraph::parameter(node,"kalman_velocity_damping"),!reset,!reset};
            };
            preview.speedCurve=[](NodeGraph::Point point,NodeGraph::Point center,double,const NodeGraph::Node& node) {
                const double distance=std::hypot(point.x-center.x,point.y-center.y);
                const double nearRadius=std::max(0.001,NodeGraph::parameter(node,"near_radius"));
                const double amount=std::clamp(distance/nearRadius,0.0,1.0);
                return NodeGraph::parameter(node,"min_speed")+(NodeGraph::parameter(node,"max_speed")-NodeGraph::parameter(node,"min_speed"))*amount;
            };
            preview.mouseDelta=[](NodeGraph::Point point,double speed,NodeGraph::Point center,const NodeGraph::Node& node) {
                NodeGraph::Movement movement;
                const double sensitivity=NodeGraph::parameter(node,"sens");
                const double yaw=NodeGraph::parameter(node,"yaw"),pitch=NodeGraph::parameter(node,"pitch");
                if(sensitivity<=0||yaw<=0||pitch<=0)return movement;
                movement.steps.push_back({static_cast<int>(std::clamp((point.x-center.x)*NodeGraph::parameter(node,"fov_x")/320*speed/(sensitivity*yaw),-32767.0,32767.0)),
                    static_cast<int>(std::clamp((point.y-center.y)*NodeGraph::parameter(node,"fov_y")/320*speed/(sensitivity*pitch),-32767.0,32767.0))});
                return movement;
            };
            preview.windMotion=[](int,const NodeGraph::Movement& movement,const NodeGraph::Node&) { return movement; };
            preview.motionCompensation=[](NodeGraph::Point point){return point;};
            preview.keyDown=[](int key){return (GetAsyncKeyState(key)&0x8000)!=0;};
            NodeGraph::service().evaluate(preview);
        }

        const int tabCount = static_cast<int>(sizeof(kOverlayTabs) / sizeof(kOverlayTabs[0]));
        auto workflows=NodeGraph::service().workspaceMetadata();
        if(g_activeOverlayTab>=tabCount||(ActiveWorkflowId()!=0&&NodeGraph::activePage(workflows,ActiveWorkflowId())<0))
            g_activeOverlayTab=workflows.graphs.empty()?0:-NodeGraph::workflowId(workflows.graphs.front(),0);
        static char workflowName[129]{};static int workflowPreset=1,workflowIcon=-1,renameWorkflowId=0;static std::string workflowError;
        bool openCreate=false,openRename=false;

        const bool sidebarCollapsed = config.overlay_sidebar_collapsed;
        if (g_sidebarAnimation.initialized && g_sidebarAnimation.targetCollapsed != sidebarCollapsed)
            NodeEditor_EndInteraction(true);
        const float sidebarExpansion = g_sidebarAnimation.update(sidebarCollapsed);
        const float collapsedSidebarWidth = 44.0f * g_runtimeUiScale;
        const float sidebarWidth = collapsedSidebarWidth + (expandedSidebarWidth - collapsedSidebarWidth) * sidebarExpansion;
        if (g_sidebarAnimation.running()) NodeEditor_PreserveViewport();
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 4.0f * g_runtimeUiScale));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##options_nav", ImVec2(sidebarWidth, 0.0f),
            ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_None);
        ImGui::PopStyleVar();

        if (DrawSidebarHeader(sidebarCollapsed, sidebarExpansion))
        {
            config.overlay_sidebar_collapsed = !sidebarCollapsed;
            OverlayConfig_MarkDirty();
        }
        const auto workflowButton=[&](int id,const OverlayTabItem& base){
            auto item=base;const int graph=NodeGraph::activePage(workflows,id);if(graph<0)return;
            const auto label=NodeGraph::workflowTitle(workflows.graphs[graph]);item.label=label.c_str();
            if(workflows.graphs[graph].workflowIcon>=0)item.icon=static_cast<SidebarIconKind>(workflows.graphs[graph].workflowIcon);
            ImGui::PushID(id);
            if(!workflows.graphs[graph].enabled)ImGui::PushStyleVar(ImGuiStyleVar_Alpha,ImGui::GetStyle().Alpha*0.55f);
            if(DrawSidebarTabButton(item,ActiveWorkflowId()==id,sidebarExpansion))
            {if(ActiveWorkflowId()!=id)NodeEditor_EndInteraction();g_activeOverlayTab=-id;}
            if(!workflows.graphs[graph].enabled)ImGui::PopStyleVar();
            if(ImGui::BeginPopupContextItem("##workflow_menu"))
            {
                if(ImGui::MenuItem("Enabled",nullptr,workflows.graphs[graph].enabled))NodeEditor_SetWorkflowEnabled(id,!workflows.graphs[graph].enabled,workflowError);
                if(ImGui::MenuItem("Name and icon")){workflowIcon=workflows.graphs[graph].workflowIcon;renameWorkflowId=id;std::snprintf(workflowName,sizeof(workflowName),"%s",label.c_str());openRename=true;workflowError.clear();}
                if(ImGui::MenuItem("Remove workflow"))NodeEditor_RemoveWorkflow(id,workflowError);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        };
        std::vector<int> drawn;
        for(std::size_t graph=0;graph<workflows.graphs.size();++graph)
        {
            const int id=NodeGraph::workflowId(workflows.graphs[graph],graph);
            if(std::find(drawn.begin(),drawn.end(),id)!=drawn.end())continue;drawn.push_back(id);
            auto item=kOverlayTabs[static_cast<int>(workflows.graphs[graph].kind)];workflowButton(id,item);
        }
        OverlayTabItem add={"Add","Nodes","Add an empty workflow or a template",nullptr,SidebarIconKind::Plus};
        if(DrawSidebarTabButton(add,false,sidebarExpansion)){workflowName[0]=0;workflowPreset=1;workflowIcon=-1;workflowError.clear();openCreate=true;}
        const char* lastGroup="Nodes";
        for(int i=static_cast<int>(NodeGraph::GraphCount);i<tabCount;++i)
        {
            const char* group=kOverlayTabs[i].group;
            if(std::strcmp(lastGroup,group)!=0)DrawSidebarGroup(group,sidebarExpansion);
            if(DrawSidebarTabButton(kOverlayTabs[i],g_activeOverlayTab==i,sidebarExpansion))
            {if(g_activeOverlayTab!=i)NodeEditor_EndInteraction();g_activeOverlayTab=i;}
            lastGroup=group;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        ImGui::SameLine(0.0f, 8.0f * g_runtimeUiScale);

        float contentExtraW = 0.0f;
        const bool graphTab = ActiveWorkflowId()!=0;
        OverlayWindowControls::reserveHeader(graphTab);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, graphTab ? ImVec2(0.0f, 0.0f) : ImGui::GetStyle().WindowPadding);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##options_content", ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_AlwaysUseWindowPadding,
            graphTab ? (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse) : ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::PopStyleVar();

        if(g_activeOverlayTab<0)draw_node_workflow(-g_activeOverlayTab);else kOverlayTabs[g_activeOverlayTab].draw();

        contentExtraW = ImGui::GetScrollMaxX();
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        switch (g_windowControls.draw(Overlay_GetWindowMode()))
        {
        case OverlayWindowControls::Action::Hide: Overlay_Hide(); break;
        case OverlayWindowControls::Action::Maximize: Overlay_ToggleMaximized(); break;
        case OverlayWindowControls::Action::Fullscreen: Overlay_ToggleFullscreen(); break;
        default: break;
        }

        if(openCreate)ImGui::OpenPopup("New workflow");if(openRename)ImGui::OpenPopup("Workflow appearance");
        if(ImGui::BeginPopup("New workflow"))
        {
            if(ImGui::IsWindowAppearing())ImGui::SetKeyboardFocusHere();
            const bool enter=ImGui::InputTextWithHint("##name","Workflow name",workflowName,sizeof(workflowName),ImGuiInputTextFlags_EnterReturnsTrue);
            const char* presets[]={"Empty logic","Empty vision","Aim template","Auto Shoot template","Recoil template","Vision template","Prediction overlay"};
            ImGui::Combo("Start with",&workflowPreset,presets,7);WorkflowIconPicker(workflowIcon);
            const int kind=workflowPreset==1?3:workflowPreset>=2?workflowPreset-2:0;
            if(ImGui::Button("Create")||enter){const int id=NodeEditor_CreateWorkflow(workflowName,kind,workflowError,workflowIcon,workflowPreset>=2);if(id>0){g_activeOverlayTab=-id;ImGui::CloseCurrentPopup();}}
            if(!workflowError.empty())ImGui::TextWrapped("%s",workflowError.c_str());ImGui::EndPopup();
        }
        if(ImGui::BeginPopup("Workflow appearance"))
        {
            if(ImGui::IsWindowAppearing())ImGui::SetKeyboardFocusHere();
            const bool enter=ImGui::InputText("##name",workflowName,sizeof(workflowName),ImGuiInputTextFlags_EnterReturnsTrue);
            const bool nameEdited=ImGui::IsItemDeactivatedAfterEdit();
            const bool iconEdited=WorkflowIconPicker(workflowIcon);
            if(enter||nameEdited||iconEdited)NodeEditor_ConfigureWorkflow(renameWorkflowId,workflowName,workflowIcon,workflowError);
            if(enter&&workflowError.empty())ImGui::CloseCurrentPopup();
            if(ImGui::Button("Done"))ImGui::CloseCurrentPopup();
            if(!workflowError.empty())ImGui::TextWrapped("%s",workflowError.c_str());ImGui::EndPopup();
        }
        if (allowAutoResize)
            TryAutoResizeOverlay(contentExtraW);

        if (allowConfigSave)
            OverlayConfig_TrySave();
    }

    ImGui::End();
    const auto& input=ImGui::GetIO();
    const bool uiOwnsInput=ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow)||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)||ImGui::IsAnyItemActive()||
        input.WantCaptureMouse||input.WantCaptureKeyboard||input.WantTextInput;
    editorInputCaptured.store(g_overlayVisible&&OverlayOwnsForegroundInput()&&uiOwnsInput);
    ImGui::Render();

    const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    const HRESULT result = g_pSwapChain->Present(0, 0);
    g_renderingOverlayFrame = false;
    NodeEditor_RunFileActions();
    ApplyPendingWindowAction();
    return result;
}

void OverlayThread()
{
    NodeGraph::service().setTelemetryEnabled(g_overlayPreview);
    if (!CreateOverlayWindow())
    {
        std::cout << "[Overlay] Can't create overlay window!" << std::endl;
        return;
    }

    SetupImGui();

    bool mcpEnabled=false;
    {std::lock_guard<std::mutex> lock(configMutex);mcpEnabled=config.local_mcp_enabled;}
    if(mcpEnabled)Overlay_StartMcp();else WorkflowMcp::stop();

    SetOverlayVisible(g_overlayPreview);

    for (const auto& pair : KeyCodes::key_code_map)
        key_names.push_back(pair.first);

    std::sort(key_names.begin(), key_names.end());
    key_names_cstrs.reserve(key_names.size());
    for (const auto& name : key_names)
        key_names_cstrs.push_back(name.c_str());


    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    OverlayThreadConfigSnapshot overlayCfg = SnapshotOverlayThreadConfig();
    bool lastExcludeFromCapture = overlayCfg.excludeFromCapture;
    bool overlayHotkeyWasDown = false;
    Overlay_SetDisplayAffinity(g_hwnd, lastExcludeFromCapture);

    while (!shouldExit)
    {
        while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
            {
                shouldExit = true;
                break;
            }
        }
        if (shouldExit) break;

        {
            std::lock_guard<std::mutex> lock(configMutex);NodeEditor_ProcessPendingActions();
            if(mcpEnabled!=config.local_mcp_enabled)
            {mcpEnabled=config.local_mcp_enabled;if(mcpEnabled)Overlay_StartMcp();else WorkflowMcp::stop();}
            WorkflowMcp::process();
        }
        if(g_agentPause>=0){detectionBuffer.setPaused(g_agentPause!=0);g_agentPause=-1;}
        ApplyPendingWindowAction();
        overlayCfg = SnapshotOverlayThreadConfig();

        // Preview has no keyboard listener. Keep its configured exit hotkey
        // available while retaining the normal flush and window cleanup path.
        if (g_overlayPreview && isAnyKeyPressedWin32Only(overlayCfg.buttonExit))
        {
            shouldExit = true;
            break;
        }

        if (lastExcludeFromCapture != overlayCfg.excludeFromCapture)
        {
            lastExcludeFromCapture = overlayCfg.excludeFromCapture;
            Overlay_SetDisplayAffinity(g_hwnd, lastExcludeFromCapture);
        }

        const bool overlayHotkeyDown = isAnyKeyPressedWin32Only(overlayCfg.buttonOpenOverlay);
        if (overlayHotkeyDown && !overlayHotkeyWasDown)
        {
            SetOverlayVisible(!g_overlayVisible);
        }
        overlayHotkeyWasDown = overlayHotkeyDown;
        editorInputCaptured.store(g_overlayVisible&&OverlayOwnsForegroundInput());

        if (!g_overlayVisible)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        HRESULT result = RenderOverlayFrame(true, !g_overlayPreview);
        if (result == DXGI_STATUS_OCCLUDED || result == DXGI_ERROR_ACCESS_LOST)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    WorkflowMcp::stop();
    NodeEditor_EndInteraction();
    NodeGraph::service().setTelemetryEnabled(false);
    editorInputCaptured.store(false);
    {
        if (!g_overlayPreview) StoreOverlayWindowGeometry(g_hwnd, true);
        std::lock_guard<std::mutex> lock(configMutex);
        if (!g_overlayPreview) OverlayConfig_SaveNow();
    }

    NodeEditor_Flush();
    ModelExport_Shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(g_hwnd);
    ::UnregisterClass(_T("Chrome"), GetModuleHandle(NULL));
}

void OverlayPreview()
{
    g_overlayPreview = true;
    NodeEditor_SetPreview(true);
    config.overlay_exclude_from_capture = false;
    OverlayThread();
}

OverlayWindowGeometry::Mode Overlay_GetWindowMode() { return g_windowPresentation.mode; }
void Overlay_ToggleMaximized() { g_windowAction = WindowAction::Maximize; }
void Overlay_ToggleFullscreen() { g_windowAction = WindowAction::Fullscreen; }
void Overlay_Hide() { g_windowAction = WindowAction::Hide; }
AgentApplicationState Overlay_AgentState()
{
    AgentApplicationState state;state.visible=g_overlayVisible;state.preview=g_overlayPreview;state.paused=detectionBuffer.isPaused();state.workflow=ActiveWorkflowId();
    state.windowMode=g_windowPresentation.mode==OverlayWindowGeometry::Mode::Windowed?"windowed":g_windowPresentation.mode==OverlayWindowGeometry::Mode::Maximized?"maximized":"fullscreen";
    const char* tabs[]={"input","hotkeys","overlay","system","export","stats","debug"};
    state.tab=state.workflow?"workflow":tabs[std::clamp(g_activeOverlayTab-4,0,6)];return state;
}
bool Overlay_AgentAction(const std::string& action,const std::string& tab,std::string& error)
{
    if(action=="show")g_windowAction=WindowAction::Show;
    else if(action=="hide")g_windowAction=WindowAction::Hide;
    else if(action=="windowed")g_windowAction=WindowAction::Windowed;
    else if(action=="maximized")g_windowAction=WindowAction::SetMaximized;
    else if(action=="fullscreen")g_windowAction=WindowAction::SetFullscreen;
    else if(action=="pause"||action=="resume")g_agentPause=action=="pause"?1:0;
    else if(action=="reconnect_input")input_method_changed=true;
    else if(action=="select_tab")
    {
        const std::vector<std::string> tabs={"input","hotkeys","overlay","system","export","stats","debug"};const auto found=std::find(tabs.begin(),tabs.end(),tab);
        if(found==tabs.end()){error="Unknown application tab. Use get_application.tabs.";return false;}
        NodeEditor_EndInteraction();g_activeOverlayTab=4+static_cast<int>(found-tabs.begin());
    }
    else{error="Unknown application action.";return false;}return true;
}
void Overlay_ApplicationSettingsChanged(const Config& before)
{
    if(!config.sameInputDeviceSettings(before))input_method_changed=true;
    if(config.overlay_opacity!=before.overlay_opacity)Overlay_SetOpacity(config.overlay_opacity);
    OverlayConfig_MarkDirty();
}
bool Overlay_StartMcp()
{
    return WorkflowMcp::start([](const std::string& request){return NodeEditor_McpCall(request,g_overlayVisible&&
        (ImGui::IsAnyItemActive()||ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel)));});
}

int APIENTRY _tWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPTSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    std::thread overlay(OverlayThread);
    overlay.join();
    return 0;
}


void Overlay_OpenGraphNode(int graph,int node)
{
    const auto workspace=NodeGraph::service().workspace();if(graph<0||graph>=static_cast<int>(workspace.graphs.size()))return;
    const int workflow=NodeGraph::workflowId(workspace.graphs[graph],graph);g_activeOverlayTab=workflow<=4?workflow-1:-workflow;
    if(node>0)NodeEditor_FocusNode(graph,node);
}
