#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <string>
#include <vector>

// The editor only needs these host declarations. Its normal overlay header
// also pulls in physical devices and inference providers, which this test must
// never initialize. Include the real editor so every gesture uses its actual
// hit testing, ImGui ownership, preview rendering and publication code.
#define OVERLAY_H
HWND g_hwnd = nullptr;
std::vector<std::string> key_names{"None", "F2", "F6"};
void Overlay_ExpandForNodes() {}
#include "../sunone_aimbot_2/overlay/node_editor.cpp"
#include "imgui/imgui_internal.h"
#include <iostream>
#include <stdexcept>

Config config{};
namespace NodeVision { void seedMissingModelPaths() {} }

namespace
{
std::string gestureScenario;
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(gestureScenario.empty()?message:gestureScenario+": "+message);
}
struct TemporaryWorkspace
{
    std::filesystem::path previous=std::filesystem::current_path();
    std::filesystem::path path=std::filesystem::temp_directory_path()/("sunone_editor_load_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryWorkspace(){std::filesystem::create_directory(path);std::filesystem::current_path(path);}
    ~TemporaryWorkspace()
    {
        std::error_code ec;std::filesystem::current_path(previous,ec);
        const auto absolute=std::filesystem::weakly_canonical(path,ec),temp=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(),ec);
        if(!ec&&absolute.parent_path()==temp&&absolute.filename().string().rfind("sunone_editor_load_",0)==0)std::filesystem::remove_all(absolute,ec);
    }
};

struct EditorHarness
{
    ImVec2 origin{};
    float windowWidth=1100;
    int graphIndex=0;
    std::string clipboard;
    std::vector<ImDrawVert> vertices;

    EditorHarness()
    {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1200, 800);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.MouseDoubleClickTime = 0.001f;
        auto& platform=ImGui::GetPlatformIO();platform.Platform_ClipboardUserData=this;
        platform.Platform_GetClipboardTextFn=[](ImGuiContext* context){return static_cast<EditorHarness*>(context->PlatformIO.Platform_ClipboardUserData)->clipboard.c_str();};
        platform.Platform_SetClipboardTextFn=[](ImGuiContext* context,const char* text){static_cast<EditorHarness*>(context->PlatformIO.Platform_ClipboardUserData)->clipboard=text?text:"";};
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(ImTextureID(1));
        editor = Editor{};
        editor.draft = defaults();
        for (auto& graph : editor.draft.graphs)
        {
            graph.nodes.clear(); graph.links.clear(); graph.groups.clear();
            graph.panX = graph.panY = 0; graph.zoom = 1;
        }
        auto& graph = editor.draft.graphs[0];
        graph.nodes = {defaultNode(NodeKind::Boolean, 1, 64, 64),
            defaultNode(NodeKind::Not, 2, 416, 64),
            defaultNode(NodeKind::Number, 3, 64, 256)};
        graph.nodes[0].value = 1;
        editor.initialized = true;
        editor.fit.fill(false); editor.autoFit.fill(false);
        std::string error;
        require(service().publish(editor.draft, error), error.c_str());
        editor.observedRevision = service().revision();
        frame(); frame();
    }

    ~EditorHarness()
    {
        ImGui::DestroyContext();
        editor = Editor{};
    }

    Graph& graph() { return editor.draft.graphs[graphIndex]; }

    void frame()
    {
        // Match the renderer's atlas refresh after a scaled popup requests a
        // new baked font size. This harness has no actual renderer backend.
        if(!ImGui::GetIO().Fonts->IsBuilt())
        {
            unsigned char* pixels=nullptr;int width=0,height=0;
            ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);ImGui::GetIO().Fonts->SetTexID(ImTextureID(1));
        }
        NodeEditor_PrepareFrame();ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(windowWidth, 740), ImGuiCond_Always);
        ImGui::Begin("Input regression", nullptr, ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        drawEditor(graphIndex);
        origin = editor.canvasOrigin[graphIndex];
        const auto* draw = ImGui::GetWindowDrawList();
        vertices.assign(draw->VtxBuffer.begin(), draw->VtxBuffer.end());
        ImGui::End();
        ImGui::Render();
    }

    ImVec2 port(int id, bool output, int pin = 0)
    {
        const auto* node = findNode(graph(), id);
        require(node != nullptr, "Test node disappeared");
        return portPoint(graph(), origin, *node, output, pin);
    }

    ImVec2 body(int id)
    {
        const auto* node = findNode(graph(), id);
        return screenPoint(graph(), origin, node->x + 90, node->y + 12);
    }

    void move(ImVec2 point)
    {
        ImGui::GetIO().AddMousePosEvent(point.x, point.y); frame();
    }

    void button(int button, bool down)
    {
        ImGui::GetIO().AddMouseButtonEvent(button, down); frame();
    }

    void click(ImVec2 point, int which = ImGuiMouseButton_Left)
    {
        move(point); button(which, true); button(which, false);
    }

    void key(ImGuiKey which, bool down)
    {
        ImGui::GetIO().AddKeyEvent(which, down); frame();
    }

    void drag(ImVec2 from, ImVec2 to,int which=ImGuiMouseButton_Left)
    {
        move(from); button(which, true);
        move(to); button(which, false);
    }

    void releaseAt(ImVec2 point)
    {
        // Win32 may deliver the last mouse move and button-up before the next
        // render. Keep them in one frame rather than inserting an artificial
        // held-button frame at the destination.
        ImGui::GetIO().AddMousePosEvent(point.x,point.y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left,false);
        frame();
    }

    void fastDrag(ImVec2 from,ImVec2 to)
    {
        move(from);button(ImGuiMouseButton_Left,true);releaseAt(to);
    }
};

void testOutputToInputAndPreview()
{
    EditorHarness harness;
    const auto output = harness.port(1, true);
    const auto input = harness.port(2, false);
    harness.move(output); harness.button(ImGuiMouseButton_Left, true);
    require(editor.wireNode == 1 && editor.wireOutput, "Output click did not start a wire");
    const ImVec2 cursor(output.x + 85, output.y + 155);
    harness.move(cursor);
    require(editor.wireNode == 1, "Wire drag lost its source while moving");
    const auto previewColor=pinColor(definition(findNode(harness.graph(),1)->kind).outputs[0].type);
    bool previewAtCursor = false;
    for (const auto& vertex : harness.vertices)
        if (vertex.col == previewColor && distanceSquared(vertex.pos, cursor) < 16) previewAtCursor = true;
    require(previewAtCursor, "Free wire endpoint does not follow the cursor");
    harness.move(input); harness.button(ImGuiMouseButton_Left, false);
    require(editor.wireNode == 0, "Wire gesture did not finish on release");
    require(harness.graph().links.size() == 1, "Output-to-input drag did not connect");
    const auto& link = harness.graph().links.front();
    require(link.fromNode == 1 && link.toNode == 2 && link.toPort == 0, "Wrong ports connected");
    require(service().workspace().graphs[0].links.size() == 1, "Wire was not immediately published");
}

void testShortPreviewDoesNotLoop()
{
    EditorHarness harness;
    ImDrawList draw(ImGui::GetDrawListSharedData());
    const ImVec2 first(100, 100), second(98, 101);
    for (int direction = 0; direction < 2; ++direction)
    {
        draw._ResetForNewFrame(); draw.PushClipRectFullScreen();
        previewWire(&draw, direction ? second : first, direction ? first : second, Cyan);
        require(!draw.VtxBuffer.empty(), "Short wire preview did not render");
        for (const auto& vertex : draw.VtxBuffer)
            require(std::abs(vertex.pos.x - first.x) < 8 && std::abs(vertex.pos.y - first.y) < 8,
                "Short/backward wire preview formed a return loop around the source");
        draw.PopClipRect();
    }
}

void testInputToOutputAndReplacement()
{
    EditorHarness harness;
    harness.drag(harness.port(2, false), harness.port(1, true));
    require(harness.graph().links.size() == 1, "Input-to-output drag did not connect");
    harness.drag(harness.port(1, true), harness.port(2, false));
    require(harness.graph().links.size() == 1, "Reconnecting an occupied input duplicated its connection");
}

void testNearestCompactPin()
{
    EditorHarness harness;
    harness.graph().nodes[1] = defaultNode(NodeKind::And, 2, 704, 64);
    harness.graph().zoom = 0.3f;
    harness.frame();
    const auto first = harness.port(2, false, 0);
    const auto second = harness.port(2, false, 1);
    require(std::abs(first.y - second.y) < 7, "Compact regression fixture no longer overlaps hit radii");
    harness.drag(harness.port(1, true), first);
    require(harness.graph().links.size() == 1 && harness.graph().links.front().toPort == 0,
        "Compact hit testing selected the later pin instead of the nearest input");
}

void testPortWinsOverLaterBody()
{
    EditorHarness harness;
    harness.graph().nodes[1].x = 224;
    harness.frame();
    harness.move(harness.port(1, true)); harness.button(ImGuiMouseButton_Left, true);
    require(editor.wireNode == 1 && editor.wireOutput,
        "A later node body swallowed an earlier node's port hit");
    harness.button(ImGuiMouseButton_Left, false);
}

void testCancelInvalidAndOutside()
{
    EditorHarness harness;
    harness.drag(harness.port(3, true), harness.port(2, false));
    require(harness.graph().links.empty(), "Incompatible port types were connected");
    editor.message.clear(); editor.error = false; harness.frame();
    harness.drag(harness.port(1, true), ImVec2(1180, 780));
    require(harness.graph().links.empty() && editor.wireNode == 0, "Dropping outside did not cancel the wire");
    harness.move(harness.port(1, true)); harness.button(ImGuiMouseButton_Left, true);
    harness.key(ImGuiKey_Escape, true); harness.key(ImGuiKey_Escape, false);
    require(editor.wireNode == 0, "Escape did not cancel a wire drag");
    harness.button(ImGuiMouseButton_Left, false);
}

void testRightClickCancelsAndWheelDoesNotMoveGesture()
{
    EditorHarness harness;
    harness.move(harness.port(1, true)); harness.button(ImGuiMouseButton_Left, true);
    const float zoom = harness.graph().zoom, panX = harness.graph().panX, panY = harness.graph().panY;
    ImGui::GetIO().AddMouseWheelEvent(0, 1); harness.frame();
    require(harness.graph().zoom == zoom && harness.graph().panX == panX && harness.graph().panY == panY,
        "Wheel changed canvas coordinates during an active wire drag");
    harness.button(ImGuiMouseButton_Middle, true);
    const auto cursor = ImGui::GetIO().MousePos;
    harness.move(add(cursor, ImVec2(40, 40)));
    require(harness.graph().panX == panX && harness.graph().panY == panY,
        "Middle mouse panned the canvas during an active wire drag");
    harness.button(ImGuiMouseButton_Middle, false);
    harness.button(ImGuiMouseButton_Right, true);
    require(editor.wireNode == 0, "Right-click did not cancel a wire drag");
    require(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel),
        "Right-click opened a menu while cancelling a wire drag");
    harness.button(ImGuiMouseButton_Right, false); harness.button(ImGuiMouseButton_Left, false);
    require(!editor.rightGesture&&!editor.marquee&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel),
        "Cancelling a wire also started right selection or opened a menu on release");
}

void testAltDisconnect()
{
    EditorHarness harness;
    harness.graph().nodes[2] = defaultNode(NodeKind::Not, 3, 64, 256);
    harness.graph().links = {{1, 0, 2, 0}, {1, 0, 3, 0}};
    harness.frame();
    harness.key(ImGuiMod_Alt, true);
    harness.click(harness.port(2, false));
    require(harness.graph().links.size() == 1 && harness.graph().links.front().toNode == 3,
        "Alt-click input disconnected another branch");
    require(editor.wireNode == 0, "Alt-click started a wire drag");
    harness.click(harness.port(1, true));
    require(harness.graph().links.empty(), "Alt-click output did not disconnect its fan-out");
    harness.key(ImGuiMod_Alt, false);
}

void testShiftSelectionAndSnappedDrag()
{
    EditorHarness harness;
    harness.click(harness.body(1));
    require(isSelected(0, 1), "Left click did not select a node");
    harness.key(ImGuiMod_Shift, true); harness.click(harness.body(2));
    require(editor.selection[0].size() == 2, "Shift-click did not add to selection");
    harness.click(harness.body(1)); harness.key(ImGuiMod_Shift, false);
    require(editor.selection[0].size() == 1 && isSelected(0, 2), "Shift-click did not toggle selection");
    const auto from = harness.body(2);
    const float x = findNode(harness.graph(), 2)->x, y = findNode(harness.graph(), 2)->y;
    harness.drag(from, add(from, ImVec2(43, 47)));
    require(findNode(harness.graph(), 2)->x == x + 32 && findNode(harness.graph(), 2)->y == y + 32,
        "Node movement did not snap to the grid");
}

void testMiddlePanAndWheelAnchor()
{
    EditorHarness harness;
    const ImVec2 cursor = add(harness.origin, ImVec2(850, 450));
    harness.move(cursor); harness.button(ImGuiMouseButton_Middle, true);
    const float x = harness.graph().panX, y = harness.graph().panY;
    harness.move(add(cursor, ImVec2(42, 29))); harness.button(ImGuiMouseButton_Middle, false);
    require(std::abs(harness.graph().panX - x - 42) < 0.1f && std::abs(harness.graph().panY - y - 29) < 0.1f,
        "Middle mouse pan did not follow the cursor");
    harness.move(cursor);
    const ImVec2 before((cursor.x - harness.origin.x - harness.graph().panX) / harness.graph().zoom,
        (cursor.y - harness.origin.y - harness.graph().panY) / harness.graph().zoom);
    ImGui::GetIO().AddMouseWheelEvent(0, 1); harness.frame();
    const ImVec2 after((cursor.x - harness.origin.x - harness.graph().panX) / harness.graph().zoom,
        (cursor.y - harness.origin.y - harness.graph().panY) / harness.graph().zoom);
    require(harness.graph().zoom > 1 && distanceSquared(before, after) < 0.01f, "Wheel zoom lost its cursor anchor");
}

void testPopupBlocksGraphShortcuts()
{
    EditorHarness harness;
    harness.click(harness.body(2), ImGuiMouseButton_Right);
    require(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel),
        "Right-click did not open the node inspector");
    const bool collapsed = findNode(harness.graph(), 2)->collapsed;
    harness.key(ImGuiKey_Space, true); harness.key(ImGuiKey_Space, false);
    require(findNode(harness.graph(), 2)->collapsed == collapsed, "Graph collapse shortcut leaked through an inspector popup");
}

const float GestureZooms[] = {0.25f, 0.45f, 0.65f, 1.0f, 1.75f};

void arrangeCompactFixture(EditorHarness& harness, float zoom)
{
    harness.graph().nodes = {defaultNode(NodeKind::Boolean, 1, 64, 64),
        defaultNode(NodeKind::And, 2, 320, 64), defaultNode(NodeKind::Number, 3, 64, 192)};
    harness.graph().nodes[0].value = 1;
    harness.graph().zoom = zoom;
    harness.frame();
}

ImVec2 canvasAt(EditorHarness& harness, float x, float y)
{
    return screenPoint(harness.graph(), harness.origin, x, y);
}
void arrangeResizeFixture(EditorHarness& h,float zoom=1)
{
    h.graph().nodes={defaultNode(NodeKind::Number,1,64,64),defaultNode(NodeKind::Clamp,2,480,64)};
    h.graph().nodes[0].value=7;h.graph().links={{1,0,2,0}};h.graph().groups.clear();h.graph().zoom=zoom;
    edited();selectOne(0,1);h.frame();
}
void testNodeResizeEdgesAndCoalescedRelease()
{
    const int edges[]={ResizeLeft,ResizeRight,ResizeTop,ResizeBottom,ResizeLeft|ResizeTop,ResizeRight|ResizeTop,ResizeLeft|ResizeBottom,ResizeRight|ResizeBottom};
    for(float zoom:GestureZooms)for(int mask:edges)
    {
        gestureScenario="Resize edges "+std::to_string(mask)+" at zoom "+std::to_string(zoom);
        EditorHarness h;arrangeResizeFixture(h,zoom);const auto revision=service().revision();
        const auto original=*findNode(h.graph(),1);const float width=cardWidth(original),height=Layout::height(original,true);
        const float x=original.x+(mask&ResizeLeft?0:mask&ResizeRight?width:width*0.5f);
        const float y=original.y+(mask&ResizeTop?0:mask&ResizeBottom?height:height*0.5f);
        const float dx=mask&ResizeLeft?-64.0f:mask&ResizeRight?64.0f:0,dy=mask&ResizeTop?-64.0f:mask&ResizeBottom?64.0f:0;
        const auto start=canvasAt(h,x,y);h.fastDrag(start,add(start,ImVec2(dx*zoom,dy*zoom)));
        const auto* node=findNode(h.graph(),1);
        require(node->width==(dx!=0?snap(width+64):0)&&node->height==(dy!=0?snap(height+64):0),"Resize did not preserve the unmodified automatic dimension");
        require(node->x==(mask&ResizeLeft?0:64)&&node->y==(mask&ResizeTop?0:64),"Top/left resize moved the wrong anchor");
        require(!editor.resizeNode&&!editor.dragNode&&!editor.wireNode&&editor.panButton<0&&editor.undo.size()==1,"Resize also moved a node, panned, connected or created multiple undo entries");
        require(service().revision()==revision&&service().workspace().graphs[0].nodes[0].width==node->width,"Resize reset execution or did not apply immediately");
        h.frame();require(distanceSquared(editor.canvasRoutes[0].routes[0].path.front(),h.port(1,true))<0.01f,"Wire endpoint did not follow resized width");
        history(false);h.frame();node=findNode(h.graph(),1);
        require(node->x==64&&node->y==64&&node->width==0&&node->height==0&&node->value==7,"Undo did not restore automatic geometry");
    }
}
void testResizeCancellationMinimumAndGroupBounds()
{
    for(int cancellation=0;cancellation<3;++cancellation)
    {
        gestureScenario="Resize cancellation and group bounds "+std::to_string(cancellation);
        EditorHarness h;arrangeResizeFixture(h);h.graph().groups={{1,"Frame",32,0,256,288,0x40ffcd60u,{1}}};h.frame();
        editor.redo.push_back(editor.draft);
        const auto grip=canvasAt(h,254,224);h.move(grip);h.button(ImGuiMouseButton_Left,true);
        h.move(add(grip,ImVec2(64,96)));
        require(editor.resizeNode==1&&findGroup(h.graph(),1)->width>=320&&findGroup(h.graph(),1)->height>=352,"Resize did not grow the containing frame");
        if(cancellation==0){h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);}
        if(cancellation==1){h.button(ImGuiMouseButton_Right,true);h.button(ImGuiMouseButton_Right,false);}
        if(cancellation==2)NodeEditor_EndInteraction();
        h.button(ImGuiMouseButton_Left,false);
        require(!editor.resizeNode&&findNode(h.graph(),1)->width==0&&findNode(h.graph(),1)->height==0&&findGroup(h.graph(),1)->width==256&&findGroup(h.graph(),1)->height==288,
            "Cancelling resize retained manual geometry or expanded frames");
        require(editor.undo.empty()&&editor.redo.size()==1,"Cancelled resize changed undo/redo history");
        require(!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel),"Resize cancellation opened a context menu");
    }
    gestureScenario="Resize returning to start and minimum bounds";
    EditorHarness h;arrangeResizeFixture(h);const auto grip=canvasAt(h,254,224);
    h.move(grip);h.button(ImGuiMouseButton_Left,true);h.move(add(grip,ImVec2(96,64)));h.move(grip);h.button(ImGuiMouseButton_Left,false);
    require(findNode(h.graph(),1)->width==0&&findNode(h.graph(),1)->height==0&&editor.undo.empty(),"Returning resize to its origin froze automatic size");
    h.fastDrag(grip,add(grip,ImVec2(-220,-200)));h.frame();const auto* node=findNode(h.graph(),1);
    require(node->width==Layout::MinWidth&&node->height==Layout::up(Layout::minimumHeight(*node)),"Resize did not clamp to usable minimum dimensions");
}
void testManualSizeSurvivesCollapseClipboardAndSaving()
{
    gestureScenario="Manual dimensions survive collapse, clipboard and autosave";
    EditorHarness h;arrangeResizeFixture(h);TemporaryWorkspace directory;
    const auto grip=canvasAt(h,254,224);h.move(grip);h.button(ImGuiMouseButton_Left,true);h.move(add(grip,ImVec2(96,96)));
    NodeEditor_Flush();require(!std::filesystem::exists(GraphFile),"An unconfirmed resize was saved to disk");h.button(ImGuiMouseButton_Left,false);
    const float width=findNode(h.graph(),1)->width,height=findNode(h.graph(),1)->height;
    collapseSelection(h.graph(),0);h.frame();
    require(findNode(h.graph(),1)->width==width&&findNode(h.graph(),1)->height==height&&cardHeight(*findNode(h.graph(),1))<height,"Collapse destroyed expanded dimensions");
    require(nodeResizeHit(h.graph(),*findNode(h.graph(),1),h.origin,canvasAt(h,64+width,64+cardHeight(*findNode(h.graph(),1))))==0,"A collapsed node exposes a height resizer");
    copySelection(h.graph(),0);editor.pastePosition[0]=ImVec2(704,320);const auto count=h.graph().nodes.size();pasteSelection(h.graph(),0);
    require(h.graph().nodes.size()==count+1,"Sized node could not be pasted");
    const auto* pasted=findNode(h.graph(),editor.selected[0]);require(pasted->width==width&&pasted->height==height&&pasted->collapsed,"Clipboard lost the node's expanded size");
    NodeEditor_Flush();Workspace loaded;std::string error;std::ifstream file(GraphFile);const std::string data((std::istreambuf_iterator<char>(file)),{});
    require(deserialize(data,loaded,error)&&loaded.graphs[0].nodes[0].width==width&&loaded.graphs[0].nodes[0].height==height,"Autosave lost manual dimensions");
}
void testResizePresetsUseCardSpaceAndKeepPortsAccessible()
{
    gestureScenario="Resize presets, content fitting and port priority";
    EditorHarness h;h.graph().nodes={defaultNode(NodeKind::Prediction,1,64,64)};edited();selectOne(0,1);h.frame();
    const auto original=settingsRect(h.graph(),h.origin,*findNode(h.graph(),1));const auto input=h.port(1,false);
    resizeSelection(h.graph(),0,NodeSizePreset::Wide);h.frame();
    require(findNode(h.graph(),1)->width==384&&settingsRect(h.graph(),h.origin,*findNode(h.graph(),1)).GetWidth()>original.GetWidth(),"Wide preset did not enlarge embedded controls");
    resizeSelection(h.graph(),0,NodeSizePreset::FitContent);h.frame();
    require(!hasNodeSettings(*findNode(h.graph(),1))&&findNode(h.graph(),1)->height==0,"Fit content reserved a second panel for input settings");
    require(distanceSquared(h.port(1,false),input)<0.001f,"Height resizing moved original input positions");
    resizeSelection(h.graph(),0,NodeSizePreset::Automatic);h.frame();require(findNode(h.graph(),1)->width==0&&findNode(h.graph(),1)->height==0,"Automatic reset retained manual dimensions");
    h.graph().nodes={defaultNode(NodeKind::Number,1,64,64),defaultNode(NodeKind::Clamp,2,416,64)};edited();selectOne(0,1);h.frame();
    h.move(h.port(1,true));h.button(ImGuiMouseButton_Left,true);require(editor.wireNode==1&&!editor.resizeNode,"Selected card resizer swallowed its output socket");h.button(ImGuiMouseButton_Right,true);h.button(ImGuiMouseButton_Right,false);h.button(ImGuiMouseButton_Left,false);
}

void testGroupDragAndResizeAcrossZoom()
{
    for (float zoom : GestureZooms)
    {
        gestureScenario="Group drag/resize at zoom "+std::to_string(zoom);
        EditorHarness harness;
        arrangeCompactFixture(harness, zoom);
        harness.graph().groups = {{1,"Gesture group",32,32,512,288,0x40ffcd60u,{1,2}}};
        harness.frame();
        const auto header = canvasAt(harness, 92, 46);
        harness.move(header); harness.button(ImGuiMouseButton_Left, true);
        require(editor.dragGroup == 1 && !editor.dragNode && !editor.wireNode,
            "Group header click was stolen by another mouse gesture");
        harness.move(add(header, ImVec2(40 * zoom, 40 * zoom)));
        harness.button(ImGuiMouseButton_Left, false);
        const auto* group = findGroup(harness.graph(), 1);
        require(group->x == 64 && group->y == 64,
            ("Group drag did not snap consistently across zoom; actual="+std::to_string(group->x)+","+
             std::to_string(group->y)+" zoom="+std::to_string(harness.graph().zoom)+
             " moved="+std::to_string(editor.moved)+" mouse="+std::to_string(ImGui::GetIO().MousePos.x)+","+
             std::to_string(ImGui::GetIO().MousePos.y)+" start="+std::to_string(editor.dragStartMouse.x)+","+
             std::to_string(editor.dragStartMouse.y)).c_str());
        require(findNode(harness.graph(),1)->x == 96 && findNode(harness.graph(),1)->y == 96 &&
            findNode(harness.graph(),2)->x == 352 && findNode(harness.graph(),2)->y == 96,
            "Group drag did not move every registered member by the same amount");
        require(findNode(harness.graph(),3)->x == 64 && findNode(harness.graph(),3)->y == 192,
            "Group drag also moved an unregistered node inside its frame");
        require(!editor.dragGroup && !editor.dragNode && !editor.wireNode,
            "Group drag left a mouse gesture active after release");

        // Bring the frame back into view before testing the bottom-right grip.
        harness.graph().groups[0].x = harness.graph().groups[0].y = 32;
        findNode(harness.graph(),1)->x = 64; findNode(harness.graph(),1)->y = 64;
        findNode(harness.graph(),2)->x = 320; findNode(harness.graph(),2)->y = 64;
        harness.frame();
        const auto grip = canvasAt(harness, 538, 314);
        harness.move(grip); harness.button(ImGuiMouseButton_Left, true);
        require(editor.resizeGroup == 1 && !editor.dragNode && !editor.wireNode,
            "Group resize grip was stolen by a node or port");
        harness.move(add(grip, ImVec2(40 * zoom, 40 * zoom)));
        harness.button(ImGuiMouseButton_Left, false);
        group = findGroup(harness.graph(),1);
        require(group->width == 544 && group->height == 320,
            "Group resize did not snap consistently across zoom");
        require(findNode(harness.graph(),1)->x == 64 && findNode(harness.graph(),2)->x == 320,
            "Resizing a group moved its members");
        const auto shrink = canvasAt(harness, group->x + group->width - 6, group->y + group->height - 6);
        harness.drag(shrink, add(shrink, ImVec2(-400 * zoom, -400 * zoom)));
        group = findGroup(harness.graph(),1);
        require(group->width >= 512 && group->height >= 160,
            "Group resize allowed registered nodes to escape its bounds");
    }
}

void testAdditiveMarqueeAndMultiDragAcrossZoom()
{
    for (float zoom : GestureZooms)
    {
        gestureScenario="Multiple selection at zoom "+std::to_string(zoom);
        EditorHarness harness;
        arrangeCompactFixture(harness, zoom);
        harness.click(harness.body(3));
        harness.key(ImGuiMod_Shift, true);
        harness.drag(canvasAt(harness,40,40),canvasAt(harness,530,160),ImGuiMouseButton_Right);
        require(editor.selection[0].size() == 3 && isSelected(0,1) && isSelected(0,2) && isSelected(0,3),
            "Shift marquee did not add nodes to the existing selection");
        require(!editor.marquee && !editor.wireNode && !editor.dragNode,
            "Marquee selection did not finish cleanly");
        const auto from = harness.body(1);
        harness.drag(from, add(from, ImVec2(40 * zoom, 40 * zoom)));
        harness.key(ImGuiMod_Shift, false);
        require(editor.selection[0].size() == 3,
            "Shift-drag removed its selected anchor instead of moving the selection");
        require(findNode(harness.graph(),1)->x == 96 && findNode(harness.graph(),1)->y == 96 &&
            findNode(harness.graph(),2)->x == 352 && findNode(harness.graph(),2)->y == 96 &&
            findNode(harness.graph(),3)->x == 96 && findNode(harness.graph(),3)->y == 224,
            "Multiple selected nodes did not move together on the grid");
        const auto current = harness.body(2);
        harness.drag(current, add(current, ImVec2(40 * zoom, 40 * zoom)));
        require(findNode(harness.graph(),1)->x == 128 && findNode(harness.graph(),2)->x == 384 &&
            findNode(harness.graph(),3)->x == 128,
            "Plain dragging a selected node lost the rest of the selection");
    }
}

void testRerouteBodyAndPortsAcrossZoom()
{
    for (float zoom : GestureZooms)
    {
        for (float grab : {24.0f,40.0f})
        {
            gestureScenario="Reroute at zoom "+std::to_string(zoom)+", grab "+std::to_string(grab);
            EditorHarness harness;
            arrangeCompactFixture(harness,zoom);
            harness.graph().nodes[2] = defaultNode(NodeKind::RerouteBoolean,3,224,192);
            harness.frame();
            const auto body = canvasAt(harness,224 + grab,208);
            harness.move(body); harness.button(ImGuiMouseButton_Left,true);
            require(editor.dragNode == 3 && !editor.wireNode,
                "Reroute port halo swallowed its drag handle at a small zoom");
            harness.move(add(body,ImVec2(40 * zoom,40 * zoom)));
            harness.button(ImGuiMouseButton_Left,false);
            require(findNode(harness.graph(),3)->x == 256 && findNode(harness.graph(),3)->y == 224,
                "Reroute body did not drag on the snap grid");
            harness.drag(harness.port(1,true),harness.port(3,false));
            harness.drag(harness.port(3,true),harness.port(2,false,0));
            require(harness.graph().links.size() == 2,
                "Reroute ports became inaccessible while reserving its drag handle");
            require(harness.graph().links[0].toNode == 3 && harness.graph().links[1].fromNode == 3 &&
                harness.graph().links[1].toNode == 2 && harness.graph().links[1].toPort == 0,
                "Reroute wiring connected to the body or wrong compact input");
        }
    }
}

void testSemanticZoomKeepsCardAndPortGeometry()
{
    gestureScenario="Semantic zoom preserves original card dimensions and wire anchors";
    EditorHarness h;h.graph().nodes={defaultNode(NodeKind::Number,1,64,64),defaultNode(NodeKind::Clamp,2,480,64),
        defaultNode(NodeKind::Prediction,3,64,320)};h.graph().links={{1,0,2,0}};edited();h.frame();
    const auto revision=service().revision(),rebuilds=editor.canvasRoutes[0].rebuilds;
    for(int mode:{0,1,2})for(float zoom:{0.25f,0.45f,0.64f,0.65f,1.0f,1.75f})
    {
        editor.viewMode=mode;h.graph().zoom=zoom;h.frame();
        for(const auto& node:h.graph().nodes)
        {
            require(renderHeight(h.graph(),node)==Layout::height(node,true)&&cardWidth(node)==Layout::width(node),
                "Semantic zoom shrank a card instead of hiding its details");
            const auto& def=definition(node.kind);
            for(int side=0;side<2;++side)for(std::size_t pin=0;pin<(side?def.outputs:def.inputs).size();++pin)
            {
                if(!side&&!Layout::inputVisible(node,pin))continue;
                const auto point=h.port(node.id,side!=0,static_cast<int>(pin));
                const float x=(point.x-h.origin.x-h.graph().panX)/zoom-node.x,y=(point.y-h.origin.y-h.graph().panY)/zoom-node.y;
                const auto row=side?pin:Layout::inputRow(node,pin);
                require(std::abs(x-(side?cardWidth(node):0))<0.001f&&std::abs(y-(HeaderHeight+15+row*PinSpacing))<0.001f,
                    "Changing detail level relocated a port inside its card");
            }
        }
        require(editor.canvasRoutes[0].rebuilds==rebuilds&&service().revision()==revision,"A detail-only zoom rebuilt routes or changed execution");
    }
}
void testOverviewCaptionsFitAndReuseTheirLayout()
{
    gestureScenario="Overview captions are large, bounded and cached";
    EditorHarness h;
    for(const auto& def:definitions())if(!reroute(def.kind))for(bool collapsed:{false,true})
    {
        auto node=defaultNode(def.kind,1);node.collapsed=collapsed;NodeDisplay display;
        for(float zoom:{0.25f,0.45f,0.64f,1.0f,1.75f})
        {
            const auto& caption=overviewCaption(display,node,zoom,ImGui::GetFont(),ImGui::GetFontSize());
            require(caption.fontSize>=8&&caption.fontSize<=28&&!caption.text.empty(),"An overview caption became unreadable or missing");
            require(caption.extent.x<=caption.available.x+0.1f&&caption.extent.y<=caption.available.y+0.1f,"An overview caption spills out of its card");
            require(std::none_of(caption.text.begin(),caption.text.end(),[](char c){return c>='a'&&c<='z';}),"Overview caption was not capitalized");
            const auto layouts=caption.layouts;
            for(int repeat=0;repeat<10;++repeat)overviewCaption(display,node,zoom,ImGui::GetFont(),ImGui::GetFontSize());
            require(display.caption.layouts==layouts,"Idle frames repeatedly measured overview text");
        }
    }
    NodeDisplay display;auto node=defaultNode(NodeKind::Prediction,1);
    const auto& caption=overviewCaption(display,node,0.45f,ImGui::GetFont(),ImGui::GetFontSize());
    require(caption.fontSize>ImGui::GetFontSize()*0.45f*2,"A distant node retained a tiny header-sized caption");
    const auto layouts=caption.layouts;const float width=caption.available.x;
    node.width=384;overviewCaption(display,node,0.45f,ImGui::GetFont(),ImGui::GetFontSize());
    require(display.caption.layouts==layouts+1&&display.caption.available.x>width,"Manual width did not invalidate the caption layout");
}
void testOverviewBodyRemainsSelectableAndDraggable()
{
    gestureScenario="Overview retains the complete node hit area and manual collapse";
    EditorHarness h;h.graph().nodes={defaultNode(NodeKind::Prediction,1,64,64)};h.graph().zoom=0.4f;h.frame();
    const auto body=canvasAt(h,128,64+cardHeight(h.graph().nodes[0])-30);
    h.click(body);require(editor.selected[0]==1&&editor.panButton<0,"The lower overview body was treated as empty canvas");
    h.drag(body,add(body,ImVec2(64*0.4f,32*0.4f)));
    require(h.graph().nodes[0].x==128&&h.graph().nodes[0].y==96,"Dragging the full overview body panned the graph");
    const float expanded=renderHeight(h.graph(),h.graph().nodes[0]);h.key(ImGuiKey_Space,true);h.key(ImGuiKey_Space,false);
    require(h.graph().nodes[0].collapsed&&renderHeight(h.graph(),h.graph().nodes[0])<expanded,"Manual collapse stopped changing the card's size");
    const float folded=renderHeight(h.graph(),h.graph().nodes[0]);h.graph().zoom=1;h.frame();
    require(h.graph().nodes[0].collapsed&&renderHeight(h.graph(),h.graph().nodes[0])==folded,"Zooming undid manual collapse");
}
void testCollapseAndCollapsedPortsAcrossZoom()
{
    for (float zoom : GestureZooms)
    {
        gestureScenario="Collapse at zoom "+std::to_string(zoom);
        EditorHarness harness;
        arrangeCompactFixture(harness,zoom);
        const auto chevron = canvasAt(harness,332,78);
        harness.click(chevron);
        require(findNode(harness.graph(),2)->collapsed && !findNode(harness.graph(),1)->collapsed,
            "Collapse chevron was swallowed by a nearby compact input");
        require(!editor.wireNode && !editor.dragNode && !editor.marquee,
            "Collapse click also started another mouse gesture");
        harness.drag(harness.port(1,true),harness.port(2,false,0));
        require(harness.graph().links.size() == 1 && harness.graph().links[0].toPort == 0,
            "A manually collapsed node lost its first input at a small zoom");
        for (float otherZoom : GestureZooms)
        {
            harness.graph().zoom = otherZoom; harness.frame();
            require(findNode(harness.graph(),2)->collapsed,
                "Automatic zoom changed a node's manual collapsed state");
        }
        harness.graph().zoom = zoom; harness.frame();
        harness.click(canvasAt(harness,332,78));
        require(!findNode(harness.graph(),2)->collapsed,
            "Manual expansion failed at a small zoom");
    }
}

void testGroupHeaderWinsOverWireDoubleClick()
{
    gestureScenario="Double click wire crossing group header";
    EditorHarness harness;
    harness.graph().nodes.resize(2);
    harness.graph().links = {{1,0,2,0}};
    harness.graph().groups = {{1,"Wire crosses header",288,96,128,192,0x40ffcd60u,{}}};
    ImGui::GetIO().MouseDoubleClickTime = 0.4f;
    harness.frame();
    const auto header = canvasAt(harness,336,107);
    harness.click(header); harness.click(header);
    require(harness.graph().nodes.size() == 2 && harness.graph().links.size() == 1,
        "A group-header double click cut a crossing wire");
    require(editor.selectedGroup[0] == 1,
        "A wire crossing the header stole group selection");
}

void testInsertedRerouteMovesWithItsCommonGroup()
{
    for (float zoom : GestureZooms)
    {
        gestureScenario="Inserted reroute group membership at zoom "+std::to_string(zoom);
        EditorHarness harness;
        arrangeCompactFixture(harness,zoom);
        harness.graph().groups = {{1,"Common endpoint group",32,32,512,160,0x40ffcd60u,{1,2}}};
        harness.graph().links = {{1,0,2,0}};
        harness.frame();
        const auto source=harness.port(1,true),destination=harness.port(2,false);
        const ImVec2 midpoint((source.x+destination.x)*0.5f,(source.y+destination.y)*0.5f);
        insertReroute(harness.graph(),0,ImVec2((midpoint.x-harness.origin.x-harness.graph().panX)/zoom,
            (midpoint.y-harness.origin.y-harness.graph().panY)/zoom),0);harness.frame();
        int rerouteId=0;
        for(const auto& node:harness.graph().nodes)if(node.kind==NodeKind::RerouteBoolean)rerouteId=node.id;
        require(rerouteId!=0&&harness.graph().links.size()==2,
            "Inserting a reroute did not split the typed connection");
        const auto* group=findGroup(harness.graph(),1);
        require(std::find(group->nodes.begin(),group->nodes.end(),rerouteId)!=group->nodes.end(),
            "Inserted reroute did not inherit the common endpoint group");
        const auto* reroute=findNode(harness.graph(),rerouteId);
        const float beforeX=reroute->x,beforeY=reroute->y;
        const auto header=canvasAt(harness,group->x+60,group->y+14);
        harness.drag(header,add(header,ImVec2(40*zoom,40*zoom)));
        reroute=findNode(harness.graph(),rerouteId);
        require(reroute->x==beforeX+32&&reroute->y==beforeY+32&&
            findNode(harness.graph(),1)->x==96&&findNode(harness.graph(),2)->x==352,
            "Dragging the common group left the inserted reroute behind");
    }

    gestureScenario="Inserted reroute expands common group frame";
    EditorHarness harness;
    arrangeCompactFixture(harness,1);
    harness.graph().groups={{1,"Small common frame",32,32,512,160,0x40ffcd60u,{1,2}}};
    harness.graph().links={{1,0,2,0}};
    insertReroute(harness.graph(),0,ImVec2(608,352),0);
    const auto* group=findGroup(harness.graph(),1);
    const auto* reroute=findNode(harness.graph(),editor.selected[0]);
    require(group->x<=reroute->x&&group->y<=reroute->y&&group->x+group->width>=reroute->x+64&&
        group->y+group->height>=reroute->y+32,"Inserted reroute was left outside its inherited group frame");
}

void testDoubleClickCutsWireAcrossZoom()
{
    for(float zoom:GestureZooms)
    {
        gestureScenario="Double click cuts a wire at zoom "+std::to_string(zoom);
        EditorHarness h;arrangeCompactFixture(h,zoom);h.graph().links={{1,0,2,0}};edited();h.frame();
        ImGui::GetIO().MouseDoubleClickTime=0.4f;
        const auto& path=editor.canvasRoutes[0].routes[0].path;
        const auto midpoint=wirePosition(path,wireLength(path)*0.5f);
        const auto nodes=h.graph().nodes.size();const auto historySize=editor.undo.size();
        h.click(midpoint);h.click(midpoint);
        require(h.graph().links.empty()&&h.graph().nodes.size()==nodes,"Double click did not cut only the connection");
        require(service().workspace().graphs[0].links.empty(),"Wire cut was not published immediately");
        require(editor.cutAnimations.size()==1&&editor.undo.size()==historySize+1,"Double click did not create one animated undoable cut");
        require(!editor.wireNode&&!editor.dragNode&&editor.panButton<0&&!editor.rightGesture,"Double click left an active mouse gesture");
        const auto& cut=editor.cutAnimations[0];
        const auto screenCut=screenPoint(h.graph(),h.origin,cut.pieces[0].points.back().x,cut.pieces[0].points.back().y);
        require(distanceSquared(screenCut,midpoint)<2.25f,"Wire animation split away from the clicked point");
        history(false);require(h.graph().links.size()==1&&h.graph().nodes.size()==nodes&&editor.cutAnimations.empty(),"Undo did not restore the cut wire");
    }
}

void testWireContextKeepsRerouteAvailable()
{
    gestureScenario="Wire context menu inserts a reroute";
    EditorHarness h;h.graph().links={{1,0,2,0}};h.frame();
    const auto& path=editor.canvasRoutes[0].routes[0].path;
    h.click(wirePosition(path,wireLength(path)*0.5f),ImGuiMouseButton_Right);h.frame();
    auto& stack=ImGui::GetCurrentContext()->OpenPopupStack;
    require(!stack.empty()&&stack.back().Window,"Right click on a wire did not open its menu");
    auto* popup=stack.back().Window;
    h.click(ImVec2(popup->Pos.x+65,popup->Pos.y+popup->Size.y-16));
    require(h.graph().nodes.size()==4&&h.graph().links.size()==2,"Reroute action in the wire menu did not split the connection");
    require(findNode(h.graph(),editor.selected[0])->kind==NodeKind::RerouteBoolean,"Wire menu inserted an incompatible reroute");
}

ImVec2 inlineField(EditorHarness& h,int id,float y=0,float x=70)
{
    const auto* node=findNode(h.graph(),id);const auto panel=settingsRect(h.graph(),h.origin,*node);
    return add(panel.Min,ImVec2(x*h.graph().zoom,(y+16.5f)*h.graph().zoom));
}
ImVec2 defaultField(EditorHarness& h,int id,int pin)
{
    const auto& node=*findNode(h.graph(),id);const auto& input=definition(node.kind).inputs[pin];
    return inputValueRect(h.graph(),node,pin,h.origin,ImGui::GetFontSize()*h.graph().zoom,inputEditorWidth(input,h.graph().zoom)).GetCenter();
}
ImVec2 optionField(EditorHarness& h,int id,const char* key)
{
    const auto& inputs=definition(findNode(h.graph(),id)->kind).inputs;
    for(std::size_t p=0;p<inputs.size();++p)if(inputs[p].defaultParameter&&std::strcmp(inputs[p].defaultParameter,key)==0)return defaultField(h,id,static_cast<int>(p));
    throw std::runtime_error("Setting input is missing");
}

void requireNoFieldGesture()
{
    require(!editor.dragNode&&!editor.resizeNode&&!editor.wireNode&&!editor.dragGroup&&!editor.marquee&&editor.panButton<0,
        "A field interaction also started a graph gesture");
}

void replaceInlineText(EditorHarness& h,ImVec2 point,const char* text,bool dragField=false)
{
    if(dragField)h.key(ImGuiMod_Ctrl,true);
    h.click(point);
    if(dragField)h.key(ImGuiMod_Ctrl,false);
    requireNoFieldGesture();require(editor.inlineActiveItem!=0,"Inline text field did not take input focus");
    h.key(ImGuiMod_Ctrl,true);h.key(ImGuiKey_A,true);h.key(ImGuiKey_A,false);h.key(ImGuiMod_Ctrl,false);
    ImGui::GetIO().AddInputCharactersUTF8(text);h.frame();
    h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);requireNoFieldGesture();
}

void testInlineSettingsApplyAndOwnMouse()
{
    for(float zoom:{0.65f,1.0f,1.75f})
    {
        gestureScenario="Inline settings and mouse ownership at zoom "+std::to_string(zoom);
        EditorHarness h;h.graph().zoom=zoom;h.frame();
        const auto beforeX=findNode(h.graph(),1)->x,beforeY=findNode(h.graph(),1)->y;
        h.click(inlineField(h,1,0,15));requireNoFieldGesture();
        require(findNode(h.graph(),1)->value==0&&service().workspace().graphs[0].nodes[0].value==0,"Inline boolean did not apply immediately");
        history(false);h.frame();require(findNode(h.graph(),1)->value==1,"Inline checkbox did not create an undo entry");
        const auto panel=settingsRect(h.graph(),h.origin,*findNode(h.graph(),1));
        const auto toggle=add(panel.Min,ImVec2((CardWidth-12-22)*zoom,16*zoom));
        h.move(canvasAt(h,850,400));
        ImGui::GetIO().AddMousePosEvent(toggle.x,toggle.y);ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left,true);h.frame();
        h.button(ImGuiMouseButton_Left,false);requireNoFieldGesture();
        require(findNode(h.graph(),1)->value==0,"A switch missed a press coalesced with mouse entry");
        require(editor.canvasRoutes[0].displays[0].settingsContent<=nodeSettingsHeight(*findNode(h.graph(),1)),"A single switch created unnecessary settings scrolling");
        replaceInlineText(h,inlineField(h,3,17),"42.5");
        require(findNode(h.graph(),3)->value==42.5&&service().workspace().graphs[0].nodes[2].value==42.5,"Inline constant did not publish its entered value");
        history(false);h.frame();require(findNode(h.graph(),3)->value==0,"Undo did not restore the inline constant");
        require(findNode(h.graph(),1)->x==beforeX&&findNode(h.graph(),1)->y==beforeY,"Editing a setting moved its node");
    }
    gestureScenario="Inline numeric node-owned parameter";
    EditorHarness h;h.graph().nodes={defaultNode(NodeKind::RecoilStrength,1,64,64)};h.frame();
    replaceInlineText(h,optionField(h,1,"strength"),"17.25",true);
    require(parameter(*findNode(h.graph(),1),"strength")==17.25&&parameter(service().workspace().graphs[0].nodes[0],"strength")==17.25,
        "A node-owned numeric setting was not editable inside the card");
    history(false);h.frame();require(parameter(*findNode(h.graph(),1),"strength")==5,"Parameter editing did not preserve its undo transaction");
}

void testIntegerInlineAndTypedConnections()
{
    gestureScenario="Integer fields, typed wires and quick-add compatibility";
    EditorHarness h;h.graph().nodes={defaultNode(NodeKind::Integer,1,64,64),defaultNode(NodeKind::ToFloat,2,416,64),
        defaultNode(NodeKind::Float,3,64,288),defaultNode(NodeKind::IntegerAdd,4,416,288)};
    h.graph().links.clear();h.graph().groups.clear();edited();h.frame();
    replaceInlineText(h,inlineField(h,1,17),"2147483647");
    require(findNode(h.graph(),1)->value==MaxInteger&&findNode(service().workspace().graphs[0],1)->value==MaxInteger,"Int field lost precision or failed to publish immediately");
    require(editor.canvasRoutes[0].displays[0].text=="2147483647","Int value was formatted with decimals or throttled after an edit");
    history(false);h.frame();require(findNode(h.graph(),1)->value==0,"Integer field was not undoable");
    h.drag(h.port(1,true),h.port(2,false),ImGuiMouseButton_Left);
    require(h.graph().links.size()==1&&h.graph().links[0].fromNode==1,"Int sockets could not be connected with the mouse");
    h.drag(h.port(3,true),h.port(4,false),ImGuiMouseButton_Left);
    require(h.graph().links.size()==1&&!editor.message.empty(),"Float connected to an Int arithmetic socket without conversion");
    Link connection;editor.catalogWireNode=1;editor.catalogWirePort=0;editor.catalogWireOutput=true;
    require(catalogConnection(h.graph(),defaultNode(NodeKind::Add,9),connection),"Quick-add failed to offer a Float input to an Int output");
    require(catalogConnection(h.graph(),defaultNode(NodeKind::IntegerAdd,9),connection),"Quick-add failed to offer integer math");
    editor.catalogWireNode=3;
    require(!catalogConnection(h.graph(),defaultNode(NodeKind::IntegerAdd,9),connection)&&catalogConnection(h.graph(),defaultNode(NodeKind::ToInteger,9),connection),"Float quick-add did not require explicit Int conversion");
    editor.catalogWireNode=2;editor.catalogWireOutput=false;
    require(catalogConnection(h.graph(),defaultNode(NodeKind::Integer,9),connection)&&!catalogConnection(h.graph(),defaultNode(NodeKind::Float,9),connection),"Reverse quick-add ignored integer input compatibility");
    editor.catalogWireNode=0;
}
void testSignalTypeColorsSurviveWireStates()
{
    gestureScenario="Every signal retains its own wire color";
    const ValueType types[]={ValueType::Boolean,ValueType::Float,ValueType::Integer,ValueType::Point,ValueType::Detections,
        ValueType::Target,ValueType::Movement,ValueType::Frame,ValueType::Depth,ValueType::Mask};
    std::vector<ImU32> colors;
    for(const auto type:types)
    {
        const ImU32 rgb=pinColor(type)&0x00ffffff;
        require(std::find(colors.begin(),colors.end(),rgb)==colors.end(),"Two signal types share the same color");colors.push_back(rgb);
        for(bool active:{false,true})for(bool disabled:{false,true})
            require((wireColor(type,active,disabled)&0x00ffffff)==rgb,"Wire state replaced its signal type color");
        require((wireColor(type,false,false)>>24)>=150,"Inactive signal colors are too faint to identify");
    }
    Value integer;integer.type=ValueType::Integer;integer.integer=MinInteger;
    require(valueText(&integer)=="-2147483648"&&rerouteKind(ValueType::Integer)==NodeKind::RerouteInteger,"Int formatting or reroute type is incorrect");
}
void testInputSettingsAndSemanticZoom()
{
    gestureScenario="Input settings replace the old scrolling panel";
    EditorHarness h;h.graph().nodes={defaultNode(NodeKind::Prediction,1,64,64)};h.frame();
    require(!hasNodeSettings(*findNode(h.graph(),1))&&nodeSettingsHeight(*findNode(h.graph(),1))==0,"A second parameter panel remains below the inputs");
    h.move(optionField(h,1,"kalman_max_velocity"));const auto zoom=h.graph().zoom,panX=h.graph().panX,panY=h.graph().panY;
    ImGui::GetIO().AddMouseWheelEvent(0,-2);h.frame();
    require(h.graph().zoom==zoom&&h.graph().panX==panX&&h.graph().panY==panY,"Wheel over an input field zoomed or panned the graph");
    requireNoFieldGesture();
    const auto field=optionField(h,1,"kalman_max_velocity");h.move(canvasAt(h,900,500));
    ImGui::GetIO().AddMousePosEvent(field.x,field.y);ImGui::GetIO().AddMouseWheelEvent(0,-1);h.frame();
    require(h.graph().zoom==zoom&&h.graph().panX==panX&&h.graph().panY==panY,"Wheel input coalesced with mouse entry escaped from settings");
    replaceInlineText(h,field,"6000",true);
    require(parameter(*findNode(h.graph(),1),"kalman_max_velocity")==6000,"A former filtered setting is not editable at its input");
    findNode(h.graph(),1)->collapsed=true;h.frame();
    require(editor.inlineActiveItem==0,"Collapsing a node retained a hidden setting's focus");
    findNode(h.graph(),1)->collapsed=false;h.graph().zoom=0.45f;h.frame();
    h.move(canvasAt(h,160,480));ImGui::GetIO().AddMouseWheelEvent(0,-1);h.frame();
    require(h.graph().zoom<0.45f,"Hidden settings intercepted compact-view zoom");
}

void testStarterLayoutAccommodatesSettings()
{
    gestureScenario="Starter layouts reserve expanded settings and fit their stage frames";
    const auto fresh=defaults();
    for(const auto& graph:fresh.graphs)
    {
        for(std::size_t i=0;i<graph.nodes.size();++i)
            for(std::size_t j=i+1;j<graph.nodes.size();++j)
            {
                const auto& a=graph.nodes[i];const auto& b=graph.nodes[j];
                require(a.x+cardWidth(a)<=b.x||b.x+cardWidth(b)<=a.x||a.y+cardHeight(a)<=b.y||b.y+cardHeight(b)<=a.y,
                    "Expanded starter cards overlap");
            }
        for(const auto& group:graph.groups)for(int id:group.nodes)
        {
            const auto* node=findNode(graph,id);
            require(node&&node->x>=group.x+32&&node->y>=group.y+64&&node->x+cardWidth(*node)<=group.x+group.width-32&&
                node->y+Layout::height(*node,true)<=group.y+group.height-32,"An expanded node extends into its group header or outside its frame");
        }
        auto again=graph;require(!Layout::arrange(again),"Repeated arrangement changed an already ordered graph");
        require(!migrateStarterLayout(again),"A new layout was mistaken for a legacy starter");
    }
}

void testArrangeGraphIsUndoableAndKeepsExecution()
{
    gestureScenario="Arrange follows dependencies without changing execution or settings";
    EditorHarness h;h.graph().nodes[0].x=h.graph().nodes[1].x=h.graph().nodes[2].x=64;
    h.graph().nodes[0].y=h.graph().nodes[1].y=h.graph().nodes[2].y=64;
    h.graph().links={{1,0,2,0}};findNode(h.graph(),3)->value=25.5;edited();h.frame();
    const auto revision=service().revision();h.click(h.body(2));h.key(ImGuiMod_Alt,true);h.key(ImGuiKey_L,true);h.key(ImGuiKey_L,false);h.key(ImGuiMod_Alt,false);
    require(editor.undo.size()==1&&service().revision()==revision,"Arrangement restarted execution or did not create one undo step");
    require(findNode(h.graph(),2)->x>findNode(h.graph(),1)->x+cardWidth(*findNode(h.graph(),1)),"Arrangement placed a consumer before its source");
    require(findNode(h.graph(),3)->value==25.5&&h.graph().links.size()==1,"Arrangement changed parameters or connections");
    requireNoFieldGesture();history(false);h.frame();
    require(findNode(h.graph(),1)->x==64&&findNode(h.graph(),2)->x==64&&findNode(h.graph(),3)->value==25.5,"Undo did not restore the previous arrangement");
}
void testLegacyCustomLayoutExpandsOnce()
{
    gestureScenario="Older custom workspaces gain space without losing their graph";
    EditorHarness h;TemporaryWorkspace directory;
    const char* old="SUNONE_NODE_GRAPH 4\nGRAPH 0 40 40 1 2 0 1\nNODE 1 13 64 64 7 0 1 0 0\nNODE 2 13 64 64 9 0 0 0 0\nGROUP 1 \"Custom constants\" 32 32 256 192 1090506080 2 1 2\nGRAPH 1 40 40 1 0 0 0\nGRAPH 2 40 40 1 0 0 0\nGRAPH 3 40 40 1 0 0 0\nEND\n";
    {std::ofstream file(GraphFile);file<<old;}
    NodeEditor_Load();h.frame();
    const auto graph=h.graph();const auto* first=findNode(graph,1);const auto* second=findNode(graph,2);
    require(!editor.loadFailed&&first&&second&&first->value==7&&first->collapsed&&second->value==9,"Legacy spacing migration changed node state");
    require(first->x!=second->x||first->y!=second->y,"An older custom workspace retained overlapping cards");
    require(graph.groups.size()==1&&graph.groups[0].title=="Custom constants"&&graph.links.empty(),"Legacy layout migration replaced custom groups or wiring");
    NodeEditor_Flush();NodeEditor_Load();h.frame();
    require(findNode(h.graph(),1)->x==first->x&&findNode(h.graph(),1)->y==first->y&&findNode(h.graph(),2)->x==second->x&&
        findNode(h.graph(),2)->y==second->y,"Reopening a migrated workspace arranged it a second time");
}

void testInlinePathsAndDrivenSettings()
{
    {
        gestureScenario="Inline model path is stored on its own node";
        EditorHarness h;h.graphIndex=3;h.graph().nodes={defaultNode(NodeKind::DepthInference,1,64,64),defaultNode(NodeKind::DepthInference,2,416,64)};
        h.frame();h.frame();const auto second=textParameter(*findNode(h.graph(),2),"model_path");
        replaceInlineText(h,optionField(h,1,"model_path"),"models/depth.onnx");
        require(textParameter(*findNode(h.graph(),1),"model_path")=="models/depth.onnx"&&
            textParameter(service().workspace().graphs[3].nodes[0],"model_path")=="models/depth.onnx","Inline model path was not published immediately");
        require(textParameter(*findNode(h.graph(),2),"model_path")==second,"Editing one path retuned another node");
        history(false);h.frame();require(textParameter(*findNode(h.graph(),1),"model_path").empty(),"Undo did not restore the node's path");
    }
    {
        gestureScenario="Connected inputs protect their fallback settings";
        EditorHarness h;h.graph().nodes={defaultNode(NodeKind::Number,1,64,64),defaultNode(NodeKind::Center,2,320,64),defaultNode(NodeKind::Prediction,3,576,64)};
        h.graph().nodes[0].value=0.5;h.graph().links={{1,0,3,1},{2,0,3,0}};edited();h.frame();
        h.key(ImGuiMod_Ctrl,true);h.click(optionField(h,3,"lookahead"));h.key(ImGuiMod_Ctrl,false);
        ImGui::GetIO().AddInputCharactersUTF8("1.25");h.frame();requireNoFieldGesture();
        require(!editor.inlineActiveItem&&parameter(*findNode(h.graph(),3),"lookahead")==0&&h.graph().links.size()==2,
            "A driven fallback field accepted edits or stole a wire gesture");
    }
}

void testOverlappingGroupControlsUseOneTopmostTarget()
{
    for(float zoom:GestureZooms)
    {
        gestureScenario="Overlapping group header/resize at zoom "+std::to_string(zoom);
        EditorHarness harness;
        harness.graph().nodes.clear();harness.graph().zoom=zoom;
        harness.graph().groups={{1,"Lower resize grip",32,32,256,160,0x40ffcd60u,{}},
            {2,"Upper header",256,176,288,160,0x40ff93adu,{}}};
        harness.frame();
        const auto overlap=canvasAt(harness,282,186);
        harness.move(overlap);harness.button(ImGuiMouseButton_Left,true);
        require(editor.dragGroup==2&&editor.resizeGroup==0&&editor.selectedGroup[0]==2,
            "Overlapping controls mixed one group's header with another group's resize grip");
        harness.move(add(overlap,ImVec2(40*zoom,40*zoom)));
        harness.button(ImGuiMouseButton_Left,false);
        const auto* lower=findGroup(harness.graph(),1);const auto* upper=findGroup(harness.graph(),2);
        require(lower->x==32&&lower->y==32&&lower->width==256&&lower->height==160&&
            upper->x==288&&upper->y==208&&upper->width==288&&upper->height==160,
            "Dragging the topmost group header resized or moved the wrong group");
    }
}

void testCoalescedReleaseFinalizesEveryDrag()
{
    for(float zoom:GestureZooms)
    {
        gestureScenario="Coalesced group move/release at zoom "+std::to_string(zoom);
        {
            EditorHarness harness;arrangeCompactFixture(harness,zoom);
            harness.graph().groups={{1,"Fast group",32,32,512,288,0x40ffcd60u,{1,2}}};harness.frame();
            const auto from=canvasAt(harness,92,46);
            harness.fastDrag(from,add(from,ImVec2(40*zoom,40*zoom)));
            const auto* group=findGroup(harness.graph(),1);
            require(group->x==64&&group->y==64&&findNode(harness.graph(),1)->x==96&&
                findNode(harness.graph(),2)->x==352,
                "Group ignored a destination move delivered with button-up");
        }
        gestureScenario="Coalesced multi-node move/release at zoom "+std::to_string(zoom);
        {
            EditorHarness harness;arrangeCompactFixture(harness,zoom);
            harness.click(harness.body(1));harness.key(ImGuiMod_Shift,true);
            harness.click(harness.body(2));harness.key(ImGuiMod_Shift,false);
            const auto from=harness.body(1);
            harness.fastDrag(from,add(from,ImVec2(40*zoom,40*zoom)));
            require(editor.selection[0].size()==2&&findNode(harness.graph(),1)->x==96&&
                findNode(harness.graph(),1)->y==96&&findNode(harness.graph(),2)->x==352&&
                findNode(harness.graph(),2)->y==96,
                "Selected nodes ignored a destination move delivered with button-up");
        }
        gestureScenario="Coalesced reroute move/release at zoom "+std::to_string(zoom);
        {
            EditorHarness harness;arrangeCompactFixture(harness,zoom);
            harness.graph().nodes[2]=defaultNode(NodeKind::RerouteBoolean,3,224,192);harness.frame();
            const auto from=canvasAt(harness,248,208);
            harness.fastDrag(from,add(from,ImVec2(40*zoom,40*zoom)));
            require(findNode(harness.graph(),3)->x==256&&findNode(harness.graph(),3)->y==224,
                "Reroute ignored a destination move delivered with button-up");
        }
        gestureScenario="Coalesced group resize/release at zoom "+std::to_string(zoom);
        {
            EditorHarness harness;arrangeCompactFixture(harness,zoom);
            harness.graph().groups={{1,"Fast resize",32,32,512,288,0x40ffcd60u,{1,2}}};harness.frame();
            const auto from=canvasAt(harness,538,314);
            harness.fastDrag(from,add(from,ImVec2(40*zoom,40*zoom)));
            const auto* group=findGroup(harness.graph(),1);
            require(group->width==544&&group->height==320,
                "Group resize ignored a destination move delivered with button-up");
        }
    }
}

void testReleaseBackAtOriginRestoresDrag()
{
    for(float zoom:GestureZooms)
    {
        gestureScenario="Group returns to origin on release at zoom "+std::to_string(zoom);
        {
            EditorHarness harness;arrangeCompactFixture(harness,zoom);
            harness.graph().groups={{1,"Return group",32,32,512,288,0x40ffcd60u,{1,2}}};harness.frame();
            const auto from=canvasAt(harness,92,46);
            harness.move(from);harness.button(ImGuiMouseButton_Left,true);
            harness.move(add(from,ImVec2(40*zoom,40*zoom)));harness.releaseAt(from);
            const auto* group=findGroup(harness.graph(),1);
            require(group->x==32&&group->y==32&&findNode(harness.graph(),1)->x==64&&
                findNode(harness.graph(),2)->x==320,
                "Group retained the previous frame's offset after releasing at its origin");
        }
        gestureScenario="Multiple nodes return to origin on release at zoom "+std::to_string(zoom);
        {
            EditorHarness harness;arrangeCompactFixture(harness,zoom);
            harness.click(harness.body(1));harness.key(ImGuiMod_Shift,true);
            harness.click(harness.body(2));harness.key(ImGuiMod_Shift,false);
            const auto from=harness.body(1);
            harness.move(from);harness.button(ImGuiMouseButton_Left,true);
            harness.move(add(from,ImVec2(40*zoom,40*zoom)));harness.releaseAt(from);
            require(findNode(harness.graph(),1)->x==64&&findNode(harness.graph(),1)->y==64&&
                findNode(harness.graph(),2)->x==320&&findNode(harness.graph(),2)->y==64,
                "Selected nodes retained the previous frame's offset after returning to origin");
        }
        gestureScenario="Reroute returns to origin on release at zoom "+std::to_string(zoom);
        {
            EditorHarness harness;arrangeCompactFixture(harness,zoom);
            harness.graph().nodes[2]=defaultNode(NodeKind::RerouteBoolean,3,224,192);harness.frame();
            const auto from=canvasAt(harness,248,208);
            harness.move(from);harness.button(ImGuiMouseButton_Left,true);
            harness.move(add(from,ImVec2(40*zoom,40*zoom)));harness.releaseAt(from);
            require(findNode(harness.graph(),3)->x==224&&findNode(harness.graph(),3)->y==192,
                "Reroute retained the previous frame's offset after returning to origin");
        }
        gestureScenario="Group resize returns to original size on release at zoom "+std::to_string(zoom);
        {
            EditorHarness harness;arrangeCompactFixture(harness,zoom);
            harness.graph().groups={{1,"Return resize",32,32,512,288,0x40ffcd60u,{1,2}}};harness.frame();
            const auto from=canvasAt(harness,538,314);
            harness.move(from);harness.button(ImGuiMouseButton_Left,true);
            harness.move(add(from,ImVec2(40*zoom,40*zoom)));harness.releaseAt(from);
            const auto* group=findGroup(harness.graph(),1);
            require(group->width==512&&group->height==288,
                "Group resize retained the previous frame's size after returning to origin");
        }
    }
}

void testComboAnimationAdvancesOnceAndRespectsParentAlpha()
{
    gestureScenario="Combo animation timing and inherited opacity";
    EditorHarness harness;
    auto& io=ImGui::GetIO();
    io.AddMousePosEvent(-100,-100);
    const float parentAlpha=0.4f;
    const float decay=std::exp(-22.0f*io.DeltaTime);
    auto comboFrame=[&](bool opening,bool closing,float expected)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20,20),ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(600,400),ImGuiCond_Always);
        ImGui::Begin("Combo animation regression",nullptr,ImGuiWindowFlags_NoDecoration|
            ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,parentAlpha);
        auto* parent=ImGui::GetCurrentWindow();
        const auto popupId=ImHashStr("##ComboPopup",0,ImGui::GetID("Animated model"));
        const auto animationId=ImHashStr("##combo_open",0,popupId);
        if(closing)ImGui::ClosePopupToLevel(0,true);
        if(opening)ImGui::OpenPopupEx(popupId,ImGuiPopupFlags_None);
        const bool visible=ImGui::BeginCombo("Animated model","First model");
        if(!closing)
        {
            require(visible,"Programmatically opened combo did not display");
            const float expectedAlpha=parentAlpha*std::max(0.35f,expected);
            require(std::abs(ImGui::GetStyle().Alpha-expectedAlpha)<0.0001f,
                "Combo popup ignored its parent's opacity or used a second animation step");
        }
        if(visible)
        {
            ImGui::Selectable("First model");
            ImGui::EndCombo();
        }
        require(std::abs(parent->DC.StateStorage->GetFloat(animationId)-expected)<0.0001f,
            "Combo opening/closing advanced more than once in a rendered frame");
        require(std::abs(ImGui::GetStyle().Alpha-parentAlpha)<0.0001f,
            "Combo popup did not restore the surrounding opacity");
        ImGui::PopStyleVar();
        ImGui::End();
        ImGui::Render();
    };
    const float first=1.0f-decay;
    comboFrame(true,false,first);
    const float second=1.0f-(1.0f-first)*decay;
    comboFrame(false,false,second);
    comboFrame(false,true,second*decay);
}

void testRerouteIsRenderedAsOnePoint()
{
    for(float zoom:GestureZooms)
    {
        gestureScenario="Reroute point geometry at zoom "+std::to_string(zoom);
        EditorHarness harness;
        harness.graph().nodes={defaultNode(NodeKind::RerouteBoolean,3,224,192)};
        harness.graph().links.clear();harness.graph().groups.clear();harness.graph().zoom=zoom;
        harness.move(ImVec2(-100,-100));
        const auto* node=findNode(harness.graph(),3);
        const auto center=rerouteCenter(harness.graph(),harness.origin,*node);
        const float radius=rerouteRadius(zoom);
        const auto rgb=pinColor(ValueType::Boolean)&0x00ffffff;
        int coloredVertices=0;
        for(const auto& vertex:harness.vertices)
        {
            if((vertex.col&0x00ffffff)!=rgb)continue;
            ++coloredVertices;
            require(distanceSquared(vertex.pos,center)<=(radius+6)*(radius+6),
                "Unhovered reroute still draws a card, bar or separate endpoint pins");
        }
        require(coloredVertices>0,"Reroute point did not render its typed color");
        selectOne(0,3);harness.frame();
        const auto sideVertices=[&]()
        {
            return std::count_if(harness.vertices.begin(),harness.vertices.end(),[&](const ImDrawVert& vertex){
                return (vertex.col&0x00ffffff)==rgb&&(vertex.col>>24)>4&&distanceSquared(vertex.pos,center)>(radius+6)*(radius+6);});
        };
        require(sideVertices()==0,"Selection kept reroute handles visible with the cursor far away");
        harness.move(center);for(int frame=0;frame<20;++frame)harness.frame();
        require(sideVertices()>0,"Approaching a reroute did not reveal its side handles");
        harness.move(ImVec2(-100,-100));require(sideVertices()>0,"Reroute handles disappeared abruptly instead of fading");
        for(int frame=0;frame<60;++frame)harness.frame();
        require(isSelected(0,3)&&sideVertices()==0,"Selected reroute handles did not fade away after the cursor left");

        ImDrawList draw(ImGui::GetDrawListSharedData());
        draw._ResetForNewFrame();
        draw.PushClipRect(ImVec2(-1000,-1000),ImVec2(1000,1000));
        drawReroutePoint(&draw,ImVec2(100,100),radius,pinColor(ValueType::Boolean),false,0);
        float left=1e9f,right=-1e9f,top=1e9f,bottom=-1e9f;
        for(const auto& vertex:draw.VtxBuffer)
        {
            require(distanceSquared(vertex.pos,ImVec2(100,100))<=(radius+6)*(radius+6),
                "Reroute helper renders geometry outside a compact circular point");
            left=std::min(left,vertex.pos.x);right=std::max(right,vertex.pos.x);
            top=std::min(top,vertex.pos.y);bottom=std::max(bottom,vertex.pos.y);
        }
        require(!draw.VtxBuffer.empty()&&std::abs((right-left)-(bottom-top))<2,
            "Reroute's silhouette is elongated instead of round");
        draw.PopClipRect();
    }
}

void testReroutePointAcceptsBothWireDirections()
{
    for(float zoom:GestureZooms)
    {
        gestureScenario="Reroute point drop target at zoom "+std::to_string(zoom);
        EditorHarness harness;arrangeCompactFixture(harness,zoom);
        harness.graph().nodes[2]=defaultNode(NodeKind::RerouteBoolean,3,224,192);harness.frame();
        auto center=rerouteCenter(harness.graph(),harness.origin,*findNode(harness.graph(),3));
        harness.drag(harness.port(1,true),center);
        harness.drag(harness.port(2,false,0),center);
        require(harness.graph().links.size()==2&&harness.graph().links[0].fromNode==1&&
            harness.graph().links[0].toNode==3&&harness.graph().links[1].fromNode==3&&
            harness.graph().links[1].toNode==2,
            "Dropping on the visible point did not resolve the compatible input/output side");
        require(!editor.wireNode&&!editor.dragNode,"Point wire drop left another gesture active");

        if(zoom>=0.45f)
        {
            harness.graph().links.clear();clearSelection(0);harness.frame();
            harness.click(canvasAt(harness,256,192));
            require(!isSelected(0,3)&&!editor.wireNode,
                "The old invisible reroute card still intercepts a background click");
        }
    }
}

void testWireAnimationIsContinuousAcrossGeometryAndZoom()
{
    gestureScenario="Real wire events, geometry continuity and bounded lifetime";
    EditorHarness harness;
    WireAnimation animation;
    advanceWireAnimation(animation,200,1,{},1,0);
    require(animation.activity==0,"An idle wire animated without a runtime signal");
    const SignalEvent event{1,0.02f,1};
    advanceWireAnimation(animation,200,1,event,1,1);
    const double first=animation.phase;const float firstActivity=animation.activity;
    require(first>0&&first<0.2&&firstActivity>0&&firstActivity<1,
        "A real data event did not start a short wire pulse");
    advanceWireAnimation(animation,201,1,{},2,1);
    require(animation.phase==first&&animation.activity==firstActivity,
        "Drawing the same wire twice advanced its animation twice in one frame");
    advanceWireAnimation(animation,201,1,event,1,2);
    require(animation.phase==first,"Changing a wire's geometry caused a phase jump without elapsed time");
    advanceWireAnimation(animation,201,1,event,1.01,3);
    require(animation.phase>first&&animation.phase-first<0.1,
        "Changing wire length reversed or jumped its pulse phase");

    WireAnimation normal,small,large;
    advanceWireAnimation(normal,200,1,event,1,1);
    advanceWireAnimation(small,50,0.25f,event,1,1);
    advanceWireAnimation(large,350,1.75f,event,1,1);
    require(std::abs(normal.phase-small.phase)<0.000001&&std::abs(normal.phase-large.phase)<0.000001,
        "Zoom changed the pulse speed relative to the graph");
    advanceWireAnimation(animation,201,1,event,2,4);
    require(animation.activity==0&&animation.phase==1,"A signal looped or survived its bounded lifetime");
    advanceWireAnimation(animation,201,1,event,3,5);
    require(animation.activity==0,"Drawing an unchanged value emitted another packet");
    advanceWireAnimation(animation,201,1,{2,0,1},3,6);
    const auto born=animation.born;
    advanceWireAnimation(animation,201,1,{3,0,1},3.02,7);
    require(animation.born==born&&animation.phase>0,"A fast data stream restarted an in-flight pulse");
    WireAnimation stale,limited;
    advanceWireAnimation(stale,400,1,{4,1,1},5,1);
    advanceWireAnimation(limited,400,1,event,5,1,false);
    require(stale.activity==0&&limited.activity==0,"Stale data or the animation budget produced a new packet");
    WireAnimation hot;int launches=0;
    for(int frame=0;frame<240;++frame)
    {
        const double now=frame/240.0;const auto before=hot.born;
        advanceWireAnimation(hot,64,1,{static_cast<std::uint64_t>(frame+1),0.03f,1},now,frame);
        launches+=hot.born!=before;
    }
    require(launches>=7&&launches<=8,"Delayed samples froze packets or bypassed the eight-per-second launch limit");
    const auto now=std::chrono::steady_clock::now();NodeValue source,target;
    source.signalSequence=5;source.signaledAt=now;source.signalActive=true;
    require(wireSignal(&source,&target,1,now).strength==0,"A wire flashed although its destination never consumed that input");
    target.readInputs=1u<<1;
    require(wireSignal(&source,&target,1,now).strength==1,"A consumed data signal was not shown");
    require(signalGlow(&source,now)>0&&signalGlow(&source,now+std::chrono::seconds(1))==0,"Node data highlighting did not expire");
    editor.live.graphSampledAt[0]=now;editor.live.graphRevisions[0]=editor.observedRevision;
    require(telemetryFresh(0,now),"A current completed graph sample was rejected");
    --editor.live.graphRevisions[0];
    require(!telemetryFresh(0,now),"A late result from the previous graph version was highlighted as current data");
}

void testWireSplinesAreSmoothAndScaleInvariant()
{
    gestureScenario="Spline-only forward, return and obstacle routes";
    struct Example{ImVec2 start,end;float detour;};
    const float automatic=std::numeric_limits<float>::quiet_NaN();
    const Example examples[]={{{0,0},{240,80},automatic},{{240,0},{0,80},automatic},
        {{240,0},{0,0},automatic},{{0,0},{0,240},automatic},{{0,0},{6,80},automatic},
        {{0,0},{240,80},-90},{{0,0},{1000,400},-240},{{1000,0},{0,0},automatic}};
    for(const auto& example:examples)
    {
        const auto path=wirePath(example.start,example.end,1,example.detour);const float length=wireLength(path);
        require(distanceSquared(path.front(),example.start)<0.001f&&distanceSquared(path.back(),example.end)<0.001f,"Spline detached from its ports");
        require(path[1].x>path.front().x&&path.back().x>path[path.size()-2].x,"Spline enters or leaves a port in the wrong direction");
        for(std::size_t i=1;i<path.size();++i)
        {
            require(path[i].x>=std::min(example.start.x,example.end.x)-96&&path[i].x<=std::max(example.start.x,example.end.x)+96,"A return spline extends too far past its ports");
            const float segment=std::sqrt(distanceSquared(path[i-1],path[i]));
            require(std::isfinite(segment)&&segment<length*0.08f,"Wire has a long rectilinear leg instead of a sampled spline");
            if(i>1)
            {
                const ImVec2 previous(path[i-1].x-path[i-2].x,path[i-1].y-path[i-2].y),next(path[i].x-path[i-1].x,path[i].y-path[i-1].y);
                const float product=std::sqrt(dot(previous,previous)*dot(next,next));
                require(product>0&&dot(previous,next)/product>0.75f,"Spline contains a sharp corner or reverses at a join");
            }
        }
        for(const float zoom:GestureZooms)
        {
            const ImVec2 origin(37,-19);
            const auto scaled=wirePath(add(origin,multiply(example.start,zoom)),add(origin,multiply(example.end,zoom)),zoom,
                std::isfinite(example.detour)?origin.y+example.detour*zoom:automatic);
            require(scaled.size()==path.size(),"Zoom changed the spline route type");
            for(std::size_t i=0;i<path.size();++i)
                require(distanceSquared(scaled[i],add(origin,multiply(path[i],zoom)))<0.01f,"Zoom changed the spline shape");
        }
    }
}

void testWirePositionsFollowTheDrawnArcLength()
{
    gestureScenario="Wire pulse path and arc-length traversal";
    EditorHarness harness;
    const std::vector<ImVec2> corner={{0,0},{100,0},{100,100}};
    require(std::abs(wireLength(corner)-200)<0.001f,"Wire length does not include every path segment");
    require(distanceSquared(wirePosition(corner,75),ImVec2(75,0))<0.001f&&
        distanceSquared(wirePosition(corner,125),ImVec2(100,25))<0.001f,
        "Wire pulses accelerate or skip the corner instead of following distance along the path");
    const std::vector<ImVec2> repeated={{1,1},{1,1},{3,1}};
    require(distanceSquared(wirePosition(repeated,1),ImVec2(2,1))<0.001f,
        "Zero-length path segments break pulse positions");
    for(int variant=0;variant<3;++variant)
    {
        const ImVec2 from=variant==1?ImVec2(240,0):ImVec2(0,0);
        const ImVec2 to=variant==1?ImVec2(0,80):ImVec2(240,80);
        const auto path=wirePath(from,to,1,variant==2?-90.0f:std::numeric_limits<float>::quiet_NaN());
        const float length=wireLength(path);
        require(path.size()>1&&std::isfinite(length)&&length>0,"Wire route has no finite animated path");
        require(distanceSquared(wirePosition(path,0),from)<0.001f&&
            distanceSquared(wirePosition(path,length),to)<0.001f,
            "Wire pulse endpoints differ from the actual connection endpoints");
        for(int sample=0;sample<=20;++sample)
        {
            const auto point=wirePosition(path,length*sample/20);
            require(std::isfinite(point.x)&&std::isfinite(point.y),"Wire route produced a non-finite pulse position");
            float nearest=1e9f;
            for(size_t segment=1;segment<path.size();++segment)
            {
                const auto a=path[segment-1],b=path[segment];
                const float dx=b.x-a.x,dy=b.y-a.y,squared=dx*dx+dy*dy;
                const float fraction=squared>0?std::clamp(((point.x-a.x)*dx+(point.y-a.y)*dy)/squared,0.0f,1.0f):0;
                nearest=std::min(nearest,distanceSquared(point,ImVec2(a.x+fraction*dx,a.y+fraction*dy)));
            }
            require(nearest<0.001f,"A pulse left the visible wire during forward/return/detour routing");
        }
    }
}
}

namespace
{
void testSelectionClipboardIsPortableAndTransactional()
{
    gestureScenario="Selection clipboard remapping and isolation";
    EditorHarness h;h.graph().links={{1,0,2,0}};
    h.graph().nodes[1].collapsed=true;
    h.graph().groups={{1,"Pair",32,32,608,192,0x40ffcd60u,{1,2}}};
    editor.selection[0]={1,2};editor.selected[0]=2;
    std::string copied,error;require(selectionText(h.graph(),0,copied,error),error.c_str());
    require(pasteSelectionText(h.graph(),0,copied,ImVec2(640,384),error),error.c_str());
    require(h.graph().nodes.size()==5&&h.graph().links.size()==2&&h.graph().groups.size()==2,"Clipboard lost nodes, internal links or a group");
    const auto& selected=editor.selection[0];require(selected.size()==2,"Pasted nodes were not selected");
    const auto* first=findNode(h.graph(),selected[0]);const auto* second=findNode(h.graph(),selected[1]);
    require(first->kind==NodeKind::Boolean&&first->value==1&&second->collapsed,"Clipboard changed values or presentation state");
    require(h.graph().links.back().fromNode==first->id&&h.graph().links.back().toNode==second->id,"Clipboard did not remap link identities");
    require(h.graph().groups.back().nodes==selected,"Pasted group references old node identities");
    require(std::fmod(first->x,32)==0&&std::fmod(first->y,32)==0,"Clipboard placement is off the snap grid");
    const auto before=h.graph().nodes.size(),history=editor.undo.size();
    require(!pasteSelectionText(h.graph(),0,"unrelated clipboard text",ImVec2(),error),"Unrelated clipboard text was accepted");
    require(h.graph().nodes.size()==before&&editor.undo.size()==history,"Invalid paste partially mutated the graph");
    auto& vision=editor.draft.graphs[3];auto model=defaultNode(NodeKind::Inference,9);
    model.textParameters["model_path"]="models/custom model.onnx";model.parameters["confidence_threshold"]=0.42;
    vision.nodes.push_back(model);editor.selection[3]={9};
    require(selectionText(vision,3,copied,error),error.c_str());
    require(!pasteSelectionText(h.graph(),0,copied,ImVec2(),error),"Vision providers were pasted into a control graph");
    require(h.graph().nodes.size()==before,"Rejected cross-graph paste changed the destination");
    require(pasteSelectionText(vision,3,copied,ImVec2(256,256),error),error.c_str());
    const auto* restored=findNode(vision,editor.selected[3]);
    require(textParameter(*restored,"model_path")=="models/custom model.onnx"&&parameter(*restored,"confidence_threshold")==0.42,"Node-owned settings did not survive the clipboard");
}
void testQuickAddConnectsBothDirectionsAndKeepsOldInputUntilSuccess()
{
    gestureScenario="Create a compatible node from a dangling wire";
    {
        EditorHarness h;
        h.drag(h.port(1,true),canvasAt(h,760,400));
        require(editor.catalogWireNode==1&&editor.catalogWireOutput,"Dropping a wire on canvas did not open compatible-node creation");
        const auto before=h.graph().nodes.size();std::string error;
        require(!addCatalogNode(h.graph(),0,NodeKind::MakePoint,error),"Incompatible node passed the contextual catalog");
        require(h.graph().nodes.size()==before&&h.graph().links.empty(),"A rejected catalog choice changed the graph");
        require(addCatalogNode(h.graph(),0,NodeKind::Not,error),error.c_str());
        require(h.graph().links.size()==1&&h.graph().links[0].fromNode==1&&h.graph().links[0].toNode==editor.selected[0],"New processing node was not connected immediately");
    }
    {
        EditorHarness h;h.graph().links={{1,0,2,0}};h.frame();
        h.drag(h.port(2,false),canvasAt(h,760,400));
        require(editor.catalogWireNode==2&&!editor.catalogWireOutput&&h.graph().links[0].fromNode==1,"Opening a source-node catalog disconnected the old source");
        std::string error;require(addCatalogNode(h.graph(),0,NodeKind::Boolean,error),error.c_str());
        require(h.graph().links.size()==1&&h.graph().links[0].fromNode==editor.selected[0]&&h.graph().links[0].toNode==2,"New source did not replace the occupied input atomically");
    }
}
void testPropertiesPanelAndSelectionFocus()
{
    gestureScenario="Persistent properties and focused keyboard routing";
    EditorHarness h;selectOne(0,2);const float fullWidth=editor.canvasSize[0].x,zoom=h.graph().zoom,panX=h.graph().panX,panY=h.graph().panY;
    openProperties(0);h.frame();
    require(editor.inspectorOpen&&editor.canvasSize[0].x<fullWidth-290,"Properties did not reserve a separate pane");
    require(h.graph().zoom==zoom,"Opening properties unexpectedly changed zoom");
    require(h.graph().panX==panX&&h.graph().panY==panY,"Opening properties moved an already visible node");
    auto* pane=ImGui::FindWindowByName("Properties###node_properties");require(pane&&pane->Active,"The persistent properties window is missing");
    const bool collapsed=findNode(h.graph(),2)->collapsed;
    ImGui::FocusWindow(pane);h.key(ImGuiKey_Space,true);h.key(ImGuiKey_Space,false);
    require(findNode(h.graph(),2)->collapsed==collapsed,"A properties keyboard action leaked into the graph");
    const auto revision=service().revision();frameSelection(h.graph(),0,editor.canvasSize[0],true);
    require(service().revision()==revision,"Framing a selection changed execution state");
    const auto* node=findNode(h.graph(),2);const auto center=screenPoint(h.graph(),h.origin,node->x+cardWidth(*node)*0.5f,node->y+cardHeight(*node)*0.5f);
    require(distanceSquared(center,add(h.origin,ImVec2(editor.canvasSize[0].x*0.5f,editor.canvasSize[0].y*0.5f)))<1,"Frame selection did not center the selected node");
}
void testFailedLoadCannotAutosaveOverTheOriginal()
{
    gestureScenario="Unreadable workspace preservation";
    EditorHarness h;
    TemporaryWorkspace directory;
    const std::string broken="SUNONE_NODE_GRAPH broken\nkeep original bytes\n";
    {std::ofstream file(GraphFile,std::ios::binary);file<<broken;}
    NodeEditor_Load();h.frame();
    require(editor.loadFailed&&editor.error&&!editor.dirty,"Load failure was cleared by fitting the canvas");
    for(const auto& graph:service().workspace().graphs)require(graph.nodes.empty(),"An unreadable workspace activated an implicit default pipeline");
    h.graph().nodes.push_back(defaultNode(NodeKind::Number,1));edited();editor.dirty=true;NodeEditor_Flush();
    std::ifstream saved(GraphFile,std::ios::binary);const std::string actual((std::istreambuf_iterator<char>(saved)),{});saved.close();
    require(actual==broken,"Autosave overwrote an unreadable workspace");
    std::string valid,error;require(serialize(defaults(),valid,error),error.c_str());
    {std::ofstream file(GraphFile,std::ios::binary|std::ios::trunc);file<<valid;}
    NodeEditor_Load();require(!editor.loadFailed&&!service().workspace().graphs[0].nodes.empty(),"A repaired workspace did not recover normally");
    require(!editor.error&&editor.message.empty()&&editor.loadError.empty(),"Successful loading retained an obsolete file error");
}
void testPropertiesRemainUsableInNarrowEditor()
{
    gestureScenario="Narrow properties overlay and input ownership";
    EditorHarness h;h.windowWidth=540;h.frame();selectOne(0,2);openProperties(0);h.frame();h.frame();
    auto* pane=ImGui::FindWindowByName("Properties###node_properties");
    require(pane&&pane->Active,"Properties vanished in a narrow editor");
    const auto size=editor.canvasSize[0];
    require(size.x>500&&pane->Size.x>=290,"Narrow properties consumed the entire canvas");
    require(pane->Pos.x>=h.origin.x&&pane->Pos.x+pane->Size.x<=h.origin.x+size.x+1,"The properties panel extends outside the editor");
    const float zoom=h.graph().zoom,panX=h.graph().panX,panY=h.graph().panY;
    h.move(add(pane->Pos,ImVec2(pane->Size.x*0.5f,pane->Size.y*0.5f)));
    ImGui::GetIO().AddMouseWheelEvent(0,1);h.frame();
    require(h.graph().zoom==zoom&&h.graph().panX==panX&&h.graph().panY==panY,"Properties scrolling leaked into canvas navigation");
}
void testImportedLayoutSnapsWithoutShrinkingGroups()
{
    gestureScenario="Imported node and group grid normalization";
    EditorHarness h;auto& graph=h.graph();graph.nodes[0].x=17.25f;graph.nodes[0].y=-17.5f;
    graph.groups={{1,"Imported",-17.3f,-1.7f,400.2f,190.1f,0x40ffcd60u,{1,2}}};
    const auto previous=graph.groups[0];const auto revision=service().revision();
    require(snapLayout(graph),"An off-grid imported layout was not normalized");
    for(const auto& node:graph.nodes)require(std::fmod(node.x,32)==0&&std::fmod(node.y,32)==0,"An imported node stayed off-grid");
    const auto& group=graph.groups[0];
    require(std::fmod(group.x,32)==0&&std::fmod(group.y,32)==0&&std::fmod(group.width,32)==0&&std::fmod(group.height,32)==0,"An imported group stayed off-grid");
    require(group.x<=previous.x&&group.y<=previous.y&&group.x+group.width>=previous.x+previous.width&&group.y+group.height>=previous.y+previous.height,"Grid normalization clipped an imported group");
    require(!snapLayout(graph)&&service().revision()==revision,"Grid normalization is not idempotent or changed execution");
}
void testNavigationUsesTheEntireGestureAndRightClickRelease()
{
    for(const int button:{ImGuiMouseButton_Left,ImGuiMouseButton_Middle})for(const float zoom:GestureZooms)
    {
        gestureScenario="Absolute canvas navigation at zoom "+std::to_string(zoom);
        EditorHarness h;h.graph().zoom=zoom;h.frame();
        const auto from=add(h.origin,ImVec2(850,450));const float panX=h.graph().panX,panY=h.graph().panY;
        const auto node=*findNode(h.graph(),1);const auto revision=service().revision();
        h.move(from);h.button(button,true);
        require(!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel),"Press opened a menu before a navigation gesture could begin");
        h.move(add(from,ImVec2(3,2)));
        require(h.graph().panX==panX&&h.graph().panY==panY,"A click below the navigation threshold moved the canvas");
        h.move(add(from,ImVec2(12,9)));
        require(h.graph().panX==panX+12&&h.graph().panY==panY+9,"Panning lost the initial movement before its threshold");
        ImGui::GetIO().AddMousePosEvent(from.x+43,from.y+37);ImGui::GetIO().AddMouseButtonEvent(button,false);h.frame();
        require(h.graph().panX==panX+43&&h.graph().panY==panY+37,"Panning lost a coalesced final move and release");
        require(!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel),"Dragging the canvas also opened a context menu");
        require(findNode(h.graph(),1)->x==node.x&&findNode(h.graph(),1)->y==node.y&&editor.selection[0].empty(),"Canvas navigation moved or selected a node");
        require(service().revision()==revision&&editor.undo.empty(),"Navigation changed execution or consumed edit history");
    }
    gestureScenario="Right context click opens on release";
    EditorHarness h;h.move(h.body(2));h.button(ImGuiMouseButton_Right,true);
    require(!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel),"Context menu opened on mouse-down");
    h.button(ImGuiMouseButton_Right,false);
    require(ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel)&&editor.selected[0]==2,"Context click did not open actions for its original node");
}
void testRightSelectionNeverPansOrMovesNodes()
{
    for(const float zoom:GestureZooms)
    {
        gestureScenario="Right-button selection at zoom "+std::to_string(zoom);
        EditorHarness h;arrangeCompactFixture(h,zoom);
        const auto from=h.body(1),to=canvasAt(h,530,160);const float panX=h.graph().panX,panY=h.graph().panY;
        h.move(from);h.button(ImGuiMouseButton_Right,true);
        require(editor.rightGesture&&!editor.dragNode&&!editor.wireNode&&editor.panButton<0,"Right press started a movement or connection gesture");
        ImGui::GetIO().AddMouseWheelEvent(0,1);h.frame();
        require(h.graph().zoom==zoom,"Zoom changed coordinates during right selection");
        ImGui::GetIO().AddMousePosEvent(to.x,to.y);ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right,false);h.frame();
        require(isSelected(0,1)&&isSelected(0,2)&&!isSelected(0,3)&&editor.selection[0].size()==2,"Right drag did not finish its selection on a coalesced release");
        require(h.graph().panX==panX&&h.graph().panY==panY&&findNode(h.graph(),1)->x==64&&findNode(h.graph(),2)->x==320,"Right selection moved the canvas or a node");
        require(!editor.marquee&&!editor.rightGesture&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel),"Right selection also opened a context menu or stayed active");
        const auto drag=h.body(1);h.drag(drag,add(drag,ImVec2(40*zoom,40*zoom)));
        require(findNode(h.graph(),1)->x==96&&findNode(h.graph(),2)->x==352,"Left drag did not move the right-selected nodes together");
        const auto background=add(h.origin,ImVec2(850,450));h.drag(background,add(background,ImVec2(24,18)));
        require(h.graph().panX==panX+24&&h.graph().panY==panY+18&&editor.selection[0].size()==2&&findNode(h.graph(),1)->x==96&&findNode(h.graph(),2)->x==352,
            "Left canvas drag moved the selected nodes or discarded their selection");
    }
}
void testCatalogSubmenusOpenOnHoverAndCreateANode()
{
    gestureScenario="Hierarchical catalog hover navigation";
    EditorHarness h;h.click(add(h.origin,ImVec2(750,430)),ImGuiMouseButton_Right);h.frame();h.frame();
    auto& context=*ImGui::GetCurrentContext();
    require(context.OpenPopupStack.Size==1&&context.OpenPopupStack[0].Window,"The root node catalog did not open");
    const auto* root=context.OpenPopupStack[0].Window;
    const float firstRow=root->DC.CursorStartPos.y+ImGui::GetTextLineHeightWithSpacing()+ImGui::GetFrameHeightWithSpacing()+1+ImGui::GetStyle().ItemSpacing.y+ImGui::GetTextLineHeight()*0.5f;
    h.move(ImVec2(root->Pos.x+70,firstRow));for(int frame=0;frame<20;++frame)h.frame();
    require(context.OpenPopupStack.Size>=2&&context.OpenPopupStack[1].Window,"Hovering Add node did not open its category menu");
    const auto* categories=context.OpenPopupStack[1].Window;
    h.move(add(categories->DC.CursorStartPos,ImVec2(55,ImGui::GetTextLineHeight()*0.5f)));for(int frame=0;frame<20;++frame)h.frame();
    require(context.OpenPopupStack.Size>=3&&context.OpenPopupStack[2].Window,"Hovering Inputs did not open its category submenu");
    const auto* group=context.OpenPopupStack[2].Window;
    h.move(add(group->DC.CursorStartPos,ImVec2(55,ImGui::GetTextLineHeight()*0.5f)));for(int frame=0;frame<20;++frame)h.frame();
    require(context.OpenPopupStack.Size>=4&&context.OpenPopupStack[3].Window,"Hovering Control buttons did not open its node submenu");
    const auto* submenu=context.OpenPopupStack[3].Window;const auto before=h.graph().nodes.size();
    h.click(add(submenu->DC.CursorStartPos,ImVec2(35,ImGui::GetTextLineHeight()*0.5f)));
    require(h.graph().nodes.size()==before+1&&findNode(h.graph(),editor.selected[0])->kind==NodeKind::AimHeld,"Choosing a nested node did not create it");
    require(context.OpenPopupStack.empty(),"Choosing a nested node left a context menu open");
}
void arrangeTieFixture(EditorHarness& h,float zoom)
{
    auto& graph=h.graph();graph.nodes={defaultNode(NodeKind::Boolean,1,32,32),defaultNode(NodeKind::Not,2,608,32),
        defaultNode(NodeKind::Number,3,32,160),defaultNode(NodeKind::Add,4,608,160),
        defaultNode(NodeKind::Boolean,5,32,288),defaultNode(NodeKind::Not,6,608,288)};
    graph.links={{1,0,2,0},{3,0,4,0},{5,0,6,0}};graph.groups.clear();graph.ties.clear();graph.panX=graph.panY=0;graph.zoom=zoom;
    edited();h.frame();
}
void testWireTieGestureMovementRotationAndClipboard()
{
    for(const float zoom:GestureZooms)
    {
        gestureScenario="Wire ties across zoom "+std::to_string(zoom);
        EditorHarness h;arrangeTieFixture(h,zoom);const auto revision=service().revision();
        h.key(ImGuiKey_Z,true);h.fastDrag(canvasAt(h,320,16),canvasAt(h,320,384));h.key(ImGuiKey_Z,false);
        require(h.graph().ties.size()==1&&h.graph().nodes.size()==6&&h.graph().links.size()==3,"Z stroke did not create one visual tie for crossing wires");
        int id=h.graph().ties[0].id;for(const auto& link:h.graph().links)require(link.ties==std::vector<int>{id},"A crossed wire was not attached to the tie");
        require(service().revision()==revision,"Creating a tie changed execution state");
        const auto initial=*findTie(h.graph(),id);const auto from=canvasAt(h,initial.x,initial.y);
        h.fastDrag(from,add(from,ImVec2(40*zoom,40*zoom)));
        auto* tie=findTie(h.graph(),id);require(tie->x==initial.x+32&&tie->y==initial.y+32,"Tie movement did not finalize on the snap grid");
        h.key(ImGuiMod_Shift,true);const auto pivot=canvasAt(h,tie->x,tie->y);h.fastDrag(pivot,add(pivot,ImVec2(40,0)));h.key(ImGuiMod_Shift,false);
        tie=findTie(h.graph(),id);require(std::abs(tie->angle-30)<0.01f&&tie->x==initial.x+32&&tie->y==initial.y+32,"Shift drag moved the tie instead of rotating it");
        for(const auto& link:h.graph().links)
        {
            const auto* a=findNode(h.graph(),link.fromNode);const auto* b=findNode(h.graph(),link.toNode);
            const auto route=tiedWirePath(h.graph(),link,h.origin,h.port(a->id,true),h.port(b->id,false));
            const auto lane=tieLane(h.graph(),link,*tie,h.origin);
            require(wireDistance(route.path,lane)<0.001f&&route.tieDistances.size()==1,"A tied wire or its pulse route missed the strap");
        }
        h.key(ImGuiKey_Delete,true);h.key(ImGuiKey_Delete,false);
        require(h.graph().ties.empty()&&h.graph().links.size()==3,"Deleting a tie also deleted signal connections");
        for(const auto& link:h.graph().links)require(link.ties.empty(),"Deleting a tie left a dangling waypoint");
        history(false);require(h.graph().ties.size()==1&&h.graph().ties[0].angle==30,"Undo did not restore tie membership and rotation");
        selectAll(h.graph(),0);std::string clipboard,error;
        require(selectionText(h.graph(),0,clipboard,error)&&pasteSelectionText(h.graph(),0,clipboard,ImVec2(32,416),error),error.c_str());
        require(h.graph().ties.size()==2&&h.graph().links.size()==6&&editor.tieSelection[0].size()==1,"Clipboard did not remap and select copied ties");
        const int copied=h.graph().ties.back().id;
        require(copied!=id&&h.graph().links[3].ties==std::vector<int>{copied}&&h.graph().ties.back().angle==30,"Pasted wires share the original strap or lost its rotation");
    }
}
void testTieCancellationAndOrderedRerouteSplit()
{
    gestureScenario="Tie cancellation does not leak mouse actions";
    {
        EditorHarness h;arrangeTieFixture(h,1);h.key(ImGuiKey_Z,true);
        h.move(canvasAt(h,320,16));h.button(ImGuiMouseButton_Left,true);h.move(canvasAt(h,320,384));
        h.button(ImGuiMouseButton_Right,true);h.button(ImGuiMouseButton_Right,false);h.button(ImGuiMouseButton_Left,false);h.key(ImGuiKey_Z,false);
        require(h.graph().ties.empty()&&!editor.tieStroke&&!editor.tieMode&&!editor.rightGesture&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel),"Cancelling a tie also created a selection or menu");
    }
    gestureScenario="Multiple ties preserve route order through inserted reroutes";
    EditorHarness h;arrangeTieFixture(h,1);std::vector<TieHit> hits;std::string error;
    for(const auto& link:h.graph().links)hits.push_back({linkKey(link),0,{}});
    require(createWireTie(h.graph(),0,hits,ImVec2(416,192),0,error),error.c_str());const int first=h.graph().ties[0].id;
    require(createWireTie(h.graph(),0,hits,ImVec2(288,192),15,error),error.c_str());const int second=h.graph().ties[1].id;
    require(h.graph().links[0].ties==std::vector<int>({second,first}),"New tie did not enter the ordered wire route");
    insertReroute(h.graph(),0,ImVec2(352,192),0,1);
    require(h.graph().links[0].ties==std::vector<int>{second}&&h.graph().links.back().ties==std::vector<int>{first},"Inserting a reroute lost or reordered existing ties");
    std::string encoded;Workspace restored;
    require(serialize(editor.draft,encoded,error)&&deserialize(encoded,restored,error)&&restored.graphs[0].ties.size()==2,"Ties and reroute did not survive export/import together");
    h.graph().links.clear();edited();require(h.graph().ties.empty(),"Disconnecting the last tied wires left an orphan strap");
}
void testGroupMoveAndRightSelectionIncludeTies()
{
    gestureScenario="Group movement and selection retain wire ties";
    EditorHarness h;arrangeTieFixture(h,1);std::vector<TieHit> hits;std::string error;
    for(const auto& link:h.graph().links)hits.push_back({linkKey(link),0,{}});
    require(createWireTie(h.graph(),0,hits,ImVec2(320,192),0,error),error.c_str());
    h.graph().groups={{1,"Tied group",0,0,832,416,0x40ffcd60u,{1,2,3,4,5,6}}};edited(false);h.frame();
    const auto header=canvasAt(h,120,14);h.fastDrag(header,add(header,ImVec2(40,40)));
    require(h.graph().ties[0].x==352&&h.graph().ties[0].y==224&&findNode(h.graph(),1)->x==64,"Moving the common group left its tie behind");
    h.drag(canvasAt(h,340,212),canvasAt(h,364,236),ImGuiMouseButton_Right);
    require(editor.selection[0].empty()&&editor.tieSelection[0]==std::vector<int>{h.graph().ties[0].id},"Right selection could not select a tie independently of nodes");
}
void testTieRotationUsesFifteenDegreeSteps()
{
    gestureScenario="Tie angle snap without an extra modifier";
    EditorHarness h;arrangeTieFixture(h,1);std::vector<TieHit> hits;std::string error;
    for(const auto& link:h.graph().links)hits.push_back({linkKey(link),0,{}});
    require(createWireTie(h.graph(),0,hits,ImVec2(320,192),13,error),error.c_str());
    require(h.graph().ties[0].angle==15,"Creating a tie did not snap its angle");
    h.frame();h.key(ImGuiMod_Shift,true);auto from=canvasAt(h,320,192);h.fastDrag(from,add(from,ImVec2(20,0)));
    require(h.graph().ties[0].angle==30,"Rotating a tie required Ctrl to snap to fifteen degrees");
    h.fastDrag(from,add(from,ImVec2(7,0)));h.key(ImGuiMod_Shift,false);
    require(h.graph().ties[0].angle==30,"A small drag left a tie between angle steps");
}
void chord(EditorHarness& h,ImGuiKey modifier,ImGuiKey key)
{h.key(modifier,true);h.key(key,true);h.key(key,false);h.key(modifier,false);}
void testLibrarySelectionAndNodeStateShortcuts()
{
    gestureScenario="Library, selection, bypass and disable keyboard shortcuts";
    EditorHarness h;h.click(h.body(2));h.key(ImGuiKey_A,true);h.key(ImGuiKey_A,false);
    require(editor.selection[0].size()==3,"A did not select the whole graph");
    chord(h,ImGuiMod_Alt,ImGuiKey_A);require(editor.selection[0].empty(),"Alt+A did not clear selection");
    selectOne(0,2);chord(h,ImGuiMod_Ctrl,ImGuiKey_I);
    require(isSelected(0,1)&&isSelected(0,3)&&!isSelected(0,2),"Ctrl+I did not invert selection");
    chord(h,ImGuiMod_Ctrl,ImGuiKey_M);
    require(findNode(h.graph(),1)->disabled&&findNode(h.graph(),3)->disabled&&!findNode(h.graph(),2)->disabled,"Ctrl+M did not disable only the selected nodes");
    chord(h,ImGuiMod_Ctrl,ImGuiKey_M);require(!findNode(h.graph(),1)->disabled,"Ctrl+M did not re-enable a node");
    selectOne(0,2);chord(h,ImGuiMod_Ctrl,ImGuiKey_B);require(findNode(h.graph(),2)->bypassed,"Ctrl+B did not toggle bypass");
    h.key(ImGuiKey_B,true);h.key(ImGuiKey_B,false);require(editor.boxSelectMode,"B did not arm box selection");
    h.drag(canvasAt(h,40,40),canvasAt(h,650,160));
    require(editor.selection[0].size()==2&&isSelected(0,1)&&isSelected(0,2)&&!editor.rightGesture,"Keyboard box selection conflicted with left panning");
    chord(h,ImGuiMod_Shift,ImGuiKey_A);
    require(ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel),"Shift+A did not open the node library");
    const bool disabled=findNode(h.graph(),2)->disabled;chord(h,ImGuiMod_Ctrl,ImGuiKey_M);
    require(findNode(h.graph(),2)->disabled==disabled,"A shortcut leaked through the catalog search field");
}
void testKeyboardMoveAndDuplicateAreTransactional()
{
    gestureScenario="G and Shift+D keyboard movement transactions";
    EditorHarness h;h.click(h.body(1));const auto pointer=add(h.origin,ImVec2(760,400));h.move(pointer);
    h.key(ImGuiKey_G,true);h.key(ImGuiKey_G,false);h.move(add(pointer,ImVec2(43,47)));
    require(editor.keyboardMove&&findNode(h.graph(),1)->x==96&&editor.undo.empty(),"G did not preview a snapped move as one transaction");
    h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);
    require(!editor.keyboardMove&&findNode(h.graph(),1)->x==64&&editor.undo.empty(),"Escape did not restore a keyboard move cleanly");
    h.move(pointer);h.key(ImGuiKey_G,true);h.key(ImGuiKey_G,false);h.move(add(pointer,ImVec2(64,32)));h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);
    require(!editor.keyboardMove&&!editor.inspectorOpen&&findNode(h.graph(),1)->x==128&&editor.undo.size()==1,"Enter did not confirm one keyboard move");
    history(false);selectOne(0,1);h.move(pointer);
    chord(h,ImGuiMod_Shift,ImGuiKey_D);h.move(add(pointer,ImVec2(43,0)));
    require(editor.keyboardMove&&h.graph().nodes.size()==4,"Shift+D did not duplicate and start cursor movement");
    h.button(ImGuiMouseButton_Right,true);h.button(ImGuiMouseButton_Right,false);
    require(!editor.keyboardMove&&h.graph().nodes.size()==3&&isSelected(0,1)&&!editor.rightGesture,"Right-click did not cancel duplication without opening selection");
    chord(h,ImGuiMod_Shift,ImGuiKey_D);h.move(add(ImGui::GetIO().MousePos,ImVec2(32,0)));h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);
    require(h.graph().nodes.size()==4&&editor.undo.size()==1,"Duplicate confirmation did not create a single history entry");
    history(false);require(h.graph().nodes.size()==3,"Undo did not remove the complete duplicate transaction");
}
void testCutCrossedConnectionsWithControlRightDrag()
{
    gestureScenario="Ctrl+RMB cuts wires without moving or selecting nodes";
    EditorHarness h;arrangeTieFixture(h,1);h.key(ImGuiMod_Ctrl,true);
    h.drag(canvasAt(h,320,16),canvasAt(h,320,384),ImGuiMouseButton_Right);h.key(ImGuiMod_Ctrl,false);
    require(h.graph().links.empty()&&h.graph().nodes.size()==6&&!editor.cutStroke&&!editor.rightGesture,"Cut gesture did not remove only the crossed wires");
    require(editor.cutAnimations.size()==3,"A crossed wire did not split into a cut animation");
    for(const auto& animation:editor.cutAnimations)
        require(animation.pieces[0].points.size()<=49&&animation.pieces[1].points.size()<=49&&distanceSquared(animation.pieces[0].points.back(),animation.pieces[1].points.back())<0.001f,
            "Cut pieces did not meet at the actual intersection or exceeded their geometry budget");
    require(!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel),"Cut gesture also opened a context menu");
    history(false);require(h.graph().links.size()==3&&editor.cutAnimations.empty(),"Undo did not restore wires and clear their fading fragments");
}
void testCutAnimationRetractsAndHasAFixedBudget()
{
    gestureScenario="Cut animation geometry, lifetime and fixed cost";
    EditorHarness h;arrangeTieFixture(h,1);RoutedWire route;route.path=wirePath(h.port(1,true),h.port(2,false),1);
    const auto cut=wirePosition(route.path,wireLength(route.path)*0.5f);const auto revision=service().revision();
    for(int i=0;i<40;++i)queueCutAnimation(h.graph(),0,h.graph().links[0],route,cut);
    require(editor.cutAnimations.size()==MaxCutAnimations,"Cut animation count grew beyond its fixed budget");
    const auto animation=editor.cutAnimations.back();std::vector<ImVec2> first,second,vanished;
    cutPiecePath(animation.pieces[0],0.3f,h.graph(),h.origin,first);cutPiecePath(animation.pieces[1],0.3f,h.graph(),h.origin,second);
    require(first.size()>1&&second.size()>1&&first.back().x<cut.x&&second.back().x>cut.x,"Wire halves did not retract in opposite directions");
    require(distanceSquared(first.front(),h.port(1,true))<0.01f&&distanceSquared(second.front(),h.port(2,false))<0.01f,"Cut recoil detached the surviving endpoint");
    cutPiecePath(animation.pieces[0],1,h.graph(),h.origin,vanished);require(vanished.empty(),"A finished cut animation still generated geometry");
    for(int i=0;i<30;++i)h.frame();
    require(editor.cutAnimations.empty()&&service().revision()==revision,"Cut fragments did not expire or changed graph execution");
}
void testMouseConfirmsKeyboardMoveAndHelpOpens()
{
    gestureScenario="Mouse confirmation and shortcut reference";
    EditorHarness h;h.click(h.body(1));const auto pointer=add(h.origin,ImVec2(760,400));h.move(pointer);
    h.key(ImGuiKey_G,true);h.key(ImGuiKey_G,false);h.move(add(pointer,ImVec2(32,0)));h.click(ImGui::GetIO().MousePos);
    require(!editor.keyboardMove&&findNode(h.graph(),1)->x==96&&editor.undo.size()==1,"Left click did not confirm a keyboard move cleanly");
    chord(h,ImGuiMod_Shift,ImGuiKey_Slash);
    require(ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel),"Question-mark shortcut did not open the in-editor reference");
}
void testGroupKeyboardMoveDuplicateAndClipboard()
{
    gestureScenario="Keyboard editing retains group frames and their wires";
    EditorHarness h;h.graph().links={{1,0,2,0,{1}}};h.graph().ties={{1,320,96,15}};
    h.graph().groups={{7,"Pair",32,32,640,192,0x40ffcd60u,{1,2}}};edited();h.frame();
    const auto header=canvasAt(h,120,46);h.click(header);h.key(ImGuiKey_G,true);h.key(ImGuiKey_G,false);
    h.move(add(header,ImVec2(43,47)));h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);
    require(findGroup(h.graph(),7)->x==64&&findNode(h.graph(),1)->x==96&&h.graph().ties[0].x==352,"G did not move a selected group, its nodes and its strap together");
    cutSelection(h.graph(),0);
    require(h.graph().nodes.size()==1&&h.graph().groups.empty(),"Cutting a group after G left an empty frame behind");
    history(false);require(findGroup(h.graph(),7)&&findGroup(h.graph(),7)->x==64,"Undo did not restore the moved group after cutting");
    history(false);h.click(canvasAt(h,120,46));const auto pointer=add(h.origin,ImVec2(850,450));h.move(pointer);
    chord(h,ImGuiMod_Shift,ImGuiKey_D);h.move(add(pointer,ImVec2(64,32)));h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);
    require(h.graph().groups.size()==2&&h.graph().nodes.size()==5&&h.graph().ties.size()==2,"Shift+D did not duplicate the complete group");
    const auto& copied=h.graph().groups.back();require(copied.x==128&&copied.y==96&&copied.nodes[0]!=1&&h.graph().ties.back().x==416,"Duplicated group frame or routing did not follow the keyboard move");
    history(false);clearSelection(0);editor.selectedGroup[0]=7;std::string text,error;
    require(selectionText(h.graph(),0,text,error),"A selected group frame could not be copied");
    require(pasteSelectionText(h.graph(),0,text,ImVec2(640,384),error),error.c_str());
    require(h.graph().groups.size()==2&&h.graph().nodes.size()==5&&h.graph().ties.size()==2,"Pasting a group lost its membership or tie");
    history(false);clearSelection(0);editor.selectedGroup[0]=7;cutSelection(h.graph(),0);
    require(h.graph().nodes.size()==1&&h.graph().groups.empty()&&h.graph().links.empty()&&!h.clipboard.empty(),"Cutting a selected group removed only its frame");
    history(false);require(h.graph().nodes.size()==3&&h.graph().groups.size()==1&&h.graph().ties.size()==1,"Group cut was not one undoable operation");
}
void testLeavingEditorCancelsPreviewAndDefersAutosave()
{
    gestureScenario="Pending keyboard actions cannot escape their editor";
    EditorHarness h;TemporaryWorkspace directory;h.click(h.body(1));chord(h,ImGuiMod_Shift,ImGuiKey_D);
    require(editor.keyboardMove&&h.graph().nodes.size()==4,"Duplicate preview did not start");
    NodeEditor_Flush();require(!std::filesystem::exists(GraphFile),"Autosave persisted an unconfirmed keyboard preview");
    NodeEditor_EndInteraction();NodeEditor_Flush();
    Workspace loaded;std::string error;std::ifstream file(GraphFile,std::ios::binary);const std::string saved((std::istreambuf_iterator<char>(file)),{});
    require(!editor.keyboardMove&&h.graph().nodes.size()==3&&deserialize(saved,loaded,error)&&loaded.graphs[0].nodes.size()==3,"Closing the editor retained an unfinished duplicate");
    h.frame();h.click(h.body(1));h.key(ImGuiKey_G,true);h.key(ImGuiKey_G,false);h.move(add(ImGui::GetIO().MousePos,ImVec2(64,0)));
    h.graphIndex=1;h.frame();
    require(!editor.keyboardMove&&findNode(editor.draft.graphs[0],1)->x==64&&editor.undo.empty(),"Switching graph tabs silently committed a move without history");
}
void testKeyboardPortsConnectFollowAndDisconnect()
{
    gestureScenario="Keyboard port focus and typed connections";
    EditorHarness h;h.click(h.body(1));h.key(ImGuiKey_Tab,true);h.key(ImGuiKey_Tab,false);
    require(editor.portFocus[0].node==1&&editor.portFocus[0].output,"Tab did not focus the selected node's output");
    h.key(ImGuiKey_Space,true);h.key(ImGuiKey_Space,false);require(editor.keyboardWire&&editor.wireNode==1,"Space did not start a keyboard connection");
    h.key(ImGuiKey_Tab,true);h.key(ImGuiKey_Tab,false);
    require(editor.portFocus[0].node==2&&!editor.portFocus[0].output,"Tab did not find the next compatible input");
    h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);
    require(h.graph().links.size()==1&&h.graph().links[0].fromNode==1&&h.graph().links[0].toNode==2&&!editor.keyboardWire&&!editor.inspectorOpen,"Enter did not connect the focused ports");
    h.key(ImGuiKey_LeftArrow,true);h.key(ImGuiKey_LeftArrow,false);
    require(editor.portFocus[0].node==1&&findNode(h.graph(),2)->x==416,"Left arrow moved a node instead of following its upstream edge");
    h.key(ImGuiKey_RightArrow,true);h.key(ImGuiKey_RightArrow,false);h.key(ImGuiKey_Backspace,true);h.key(ImGuiKey_Backspace,false);
    require(h.graph().links.empty()&&h.graph().nodes.size()==3,"Backspace removed a focused node instead of disconnecting its input");
    h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);chord(h,ImGuiMod_Shift,ImGuiKey_Tab);h.key(ImGuiKey_Space,true);h.key(ImGuiKey_Space,false);
    require(h.graph().links.size()==1&&h.graph().links[0].fromNode==1,"Reverse keyboard connection did not resolve a compatible source");
    h.key(ImGuiKey_Escape,true);h.key(ImGuiKey_Escape,false);h.key(ImGuiKey_RightArrow,true);h.key(ImGuiKey_RightArrow,false);
    require(!editor.portFocus[0].node&&!editor.keyboardWire&&findNode(h.graph(),1)->x==96,"Escape did not return from port navigation to snapped node movement");
}
void testControlSaveWorksFromProperties()
{
    gestureScenario="Ctrl+S flushes while properties own keyboard focus";
    EditorHarness h;TemporaryWorkspace directory;h.click(h.body(2));openProperties(0);h.frame();
    auto* pane=ImGui::FindWindowByName("Properties###node_properties");require(pane&&pane->Active,"Properties panel missing");ImGui::FocusWindow(pane);
    editor.dirty=true;chord(h,ImGuiMod_Ctrl,ImGuiKey_S);
    require(std::filesystem::exists(GraphFile)&&!editor.dirty,"Ctrl+S was ignored while properties had focus");
}
void testCatalogCanBeTraversedWithKeyboard()
{
    gestureScenario="Native submenu navigation works without stealing canvas keys";
    EditorHarness h;h.click(h.body(1));chord(h,ImGuiMod_Shift,ImGuiKey_A);
    require((ImGui::GetIO().ConfigFlags&ImGuiConfigFlags_NavEnableKeyboard)!=0,"Popup keyboard navigation was not enabled");
    h.key(ImGuiKey_Tab,true);h.key(ImGuiKey_Tab,false);h.frame();
    for(int depth=0;depth<3;++depth){h.key(ImGuiKey_RightArrow,true);h.key(ImGuiKey_RightArrow,false);h.frame();}
    auto& context=*ImGui::GetCurrentContext();
    require(context.OpenPopupStack.Size>=4,"Arrow keys did not open the catalog's category and subcategory");
    const auto before=h.graph().nodes.size();h.key(ImGuiKey_Enter,true);h.key(ImGuiKey_Enter,false);h.frame();
    require(h.graph().nodes.size()==before+1&&context.OpenPopupStack.empty(),"Enter did not create a node from the focused submenu");
    require((ImGui::GetIO().ConfigFlags&ImGuiConfigFlags_NavEnableKeyboard)==0,"Popup navigation continued to capture canvas arrow keys after closing");
}
void testZGestureRemovesCrossedTiesWithoutCuttingWires()
{
    gestureScenario="Z gesture removes an existing strap while preserving data links";
    EditorHarness h;arrangeTieFixture(h,1);std::vector<TieHit> hits;std::string error;for(const auto& link:h.graph().links)hits.push_back({linkKey(link),0,{}});
    require(createWireTie(h.graph(),0,hits,ImVec2(320,192),0,error),error.c_str());h.frame();h.key(ImGuiKey_Z,true);
    h.fastDrag(canvasAt(h,320,16),canvasAt(h,320,384));h.key(ImGuiKey_Z,false);
    require(h.graph().ties.empty()&&h.graph().links.size()==3,"Z over a strap created another tie or cut its wires");
    history(false);require(h.graph().ties.size()==1&&h.graph().links[0].ties.size()==1,"Removing a strap was not undoable");
}
void testRouteCacheInvalidationAndAffineNavigation()
{
    gestureScenario="Route caching survives idle frames and follows every geometry change";
    EditorHarness h;h.graph().links={{1,0,2,0}};h.frame();auto& cache=editor.canvasRoutes[0];const auto built=cache.rebuilds;
    for(int i=0;i<30;++i)h.frame();
    require(cache.rebuilds==built,"Idle rendering rebuilt all wire routes");
    h.graph().panX+=37;h.graph().panY-=13;h.frame();
    require(cache.rebuilds==built&&distanceSquared(cache.routes[0].path.front(),h.port(1,true))<0.001f,"Panning rebuilt or detached a cached wire");
    h.graph().zoom=1.4f;h.frame();
    require(cache.rebuilds==built&&distanceSquared(cache.routes[0].path.back(),h.port(2,false))<0.001f,"Zooming rebuilt routes or displaced an input endpoint");
    const auto rebuiltAtZoom=cache.routes[0].path;
    cache.key.clear();h.frame();require(cache.routes[0].path.size()==rebuiltAtZoom.size(),"Cached and freshly routed zoom produced different geometry");
    for(std::size_t i=0;i<rebuiltAtZoom.size();++i)require(distanceSquared(cache.routes[0].path[i],rebuiltAtZoom[i])<0.002f,"Affine zoom changed the visible wire shape");
    auto previous=cache.rebuilds;findNode(h.graph(),2)->collapsed=true;h.frame();
    require(cache.rebuilds==previous+1&&distanceSquared(cache.routes[0].path.back(),h.port(2,false))<0.001f,"Collapsing a node left its old cached port position");
    previous=cache.rebuilds;findNode(h.graph(),3)->y+=32;h.frame();require(cache.rebuilds==previous+1,"Moving an obstacle failed to invalidate routing");
    h.graph().ties={{1,320,192,0}};h.graph().links[0].ties={1};h.frame();previous=cache.rebuilds;
    h.graph().ties[0].angle=15;h.frame();require(cache.rebuilds==previous+1&&cache.routes[0].tieDistances.size()==1,"Rotating a strap did not update the cached route");
    require(cache.connections[0][1]==1&&cache.connections[1][0]==1,"Cached pin connectivity is incorrect");
    h.graph().links.clear();h.frame();
    require(cache.routes.empty()&&cache.connections[0][1]==0&&editor.wireAnimations.empty(),"Removing a link left a cached path, pin or packet behind");
    h.graph().nodes={defaultNode(NodeKind::Number,1,64,64),defaultNode(NodeKind::Clamp,2,260,288)};
    h.graph().links={{1,0,2,0}};h.graph().ties.clear();h.graph().zoom=0.25f;h.frame();previous=cache.rebuilds;
    for(const float zoom:GestureZooms)
    {
        h.graph().zoom=zoom;h.frame();
        require(cache.rebuilds==previous,"A nearly vertical spline was rerouted during zoom");
    }
    const auto cached=cache.routes[0].path;cache.key.clear();h.frame();
    require(cached.size()==cache.routes[0].path.size(),"Rebuilding changed a nearly vertical spline's shape");
    for(std::size_t i=0;i<cached.size();++i)
        require(distanceSquared(cached[i],cache.routes[0].path[i])<0.01f,"Cached and rebuilt spline geometry differ after zoom");
}
void testHotNodeDisplayIsThrottledAndEditsAreImmediate()
{
    gestureScenario="Rapid data is coalesced without delaying edits or errors";
    NodeDisplay display;auto node=defaultNode(NodeKind::Clamp,1);NodeValue sample;sample.value.type=ValueType::Number;
    sample.signalActive=sample.active=true;const auto origin=std::chrono::steady_clock::now();
    for(int frame=0;frame<480;++frame)
    {
        const double time=frame/240.0;sample.signalSequence=static_cast<std::uint64_t>(time*1000)+1;
        const auto now=origin+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(time));
        sample.signaledAt=now;sample.value.number=time*1000;
        const float glow=updateNodeDisplay(display,node,&sample,true,true,time,now);
        if(frame>30)require(glow>0&&glow<=0.161f,"A very hot node kept flashing at full brightness");
    }
    require(display.formats<=21&&display.flashes<=2,"A hot node formatted values or restarted flashes on every render frame");
    const auto now=origin+std::chrono::seconds(3);require(updateNodeDisplay(display,node,&sample,true,true,3,now)==0,"A stopped data burst kept its activity highlight alive");
    node.kind=NodeKind::Number;node.value=3;updateNodeDisplay(display,node,&sample,true,true,3.01,now);
    node.value=4;updateNodeDisplay(display,node,&sample,true,true,3.011,now);
    require(display.text=="4.000","Value throttling delayed an explicit constant edit");
    node.kind=NodeKind::Clamp;sample.value.valid=false;sample.value.error="Provider failed";updateNodeDisplay(display,node,&sample,true,true,3.012,now);
    require(display.text=="ERROR","Throttling hid a newly reported error");
    sample.value.valid=true;sample.value.error.clear();sample.value.number=42;updateNodeDisplay(display,node,&sample,true,true,3.013,now);
    require(display.text=="42.000","Error recovery waited for the display timer");
    updateNodeDisplay(display,node,nullptr,true,true,3.014,now);require(display.text.empty(),"An unavailable value remained on screen until the next timer tick");
}
void testLargeGraphAnimationAndTextBudgets()
{
    gestureScenario="Maximum graphs bound effects and skip invisible value formatting";
    EditorHarness h;auto& graph=h.graph();graph.nodes.clear();graph.links.clear();graph.zoom=0.3f;
    for(int i=0;i<static_cast<int>(MaxNodes);++i)graph.nodes.push_back(defaultNode(i<8?NodeKind::Number:NodeKind::Clamp,i+1,(i%8)*224.0f,(i/8)*128.0f));
    for(int i=8;i<static_cast<int>(MaxNodes)&&graph.links.size()<MaxLinks;++i)
        for(int pin=0;pin<3&&graph.links.size()<MaxLinks;++pin)graph.links.push_back({1+((i*7+pin*11)%i),0,i+1,pin});
    editor.canvasRoutes[0]=CanvasRoutes{};
    edited();editor.live.graphs[0].clear();editor.liveIndex[0].clear();editor.live.graphRevisions[0]=editor.observedRevision;
    for(int i=0;i<static_cast<int>(MaxNodes);++i)
    {
        NodeValue sample;sample.nodeId=i+1;sample.value.type=ValueType::Number;sample.active=sample.signalActive=true;sample.readInputs=7;
        editor.live.graphs[0].push_back(sample);editor.liveIndex[0].emplace_back(i+1,i);
    }
    for(int frame=0;frame<25;++frame)
    {
        const auto now=std::chrono::steady_clock::now();editor.live.graphSampledAt[0]=now;editor.telemetryAt=ImGui::GetTime()+1;
        for(auto& sample:editor.live.graphs[0]){sample.signalSequence=frame+1;sample.signaledAt=now;sample.value.number=frame;}
        h.frame();require(editor.drawnSignalPulses<=16,"A zoomed-out large graph exceeded its packet budget");
    }
    std::uint64_t formats=0;for(const auto& display:editor.canvasRoutes[0].displays)formats+=display.formats;
    require(formats==0,"Invisible compact values were formatted at render frequency");
    graph.zoom=0.65f;h.frame();require(editor.drawnSignalPulses<=32,"A maximum graph exceeded its reduced packet budget");
}
void testDisplayCacheRefreshesAfterRevisionAndReopen()
{
    gestureScenario="Static cached values cannot survive a program replacement or menu reopen";
    EditorHarness h;findNode(h.graph(),2)->kind=NodeKind::Clamp;h.frame();
    auto& display=editor.canvasRoutes[0].displays[1];const auto& node=*findNode(h.graph(),2);NodeValue sample;
    sample.value.type=ValueType::Number;sample.value.number=1;sample.signalSequence=1;sample.signalActive=true;
    const auto now=std::chrono::steady_clock::now();sample.signaledAt=now;
    updateNodeDisplay(display,node,&sample,true,true,1,now);require(display.text=="1.000","Static display fixture did not initialize");
    editor.live.graphRevisions[0]=std::numeric_limits<std::uint64_t>::max();refreshTelemetry();
    sample.value.number=2;updateNodeDisplay(display,node,&sample,true,true,1.001,now);
    require(display.text=="2.000","A reset runtime counter reused text from the previous program");
    const auto rebuilds=editor.canvasRoutes[0].rebuilds;NodeEditor_EndInteraction();sample.value.number=3;
    updateNodeDisplay(display,node,&sample,true,true,1.002,now);
    require(display.text=="3.000"&&editor.canvasRoutes[0].rebuilds==rebuilds,"Reopening reused a stale value or discarded the routing cache");
}
}

#include "node_editor_benchmark.h"

#include "node_editor_binding_tests.h"
int main(int argc,char** argv)
{
    try
    {
        if(argc>1&&std::string(argv[1])=="--benchmark")return benchmarkNodeEditor(argc>2?argv[2]:"benchmark_results/node_editor.csv");
        testOptionEditorBindings();testOutputToInputAndPreview(); testShortPreviewDoesNotLoop(); testInputToOutputAndReplacement();
        testNearestCompactPin(); testPortWinsOverLaterBody();
        testCancelInvalidAndOutside(); testRightClickCancelsAndWheelDoesNotMoveGesture();
        testAltDisconnect(); testShiftSelectionAndSnappedDrag(); testMiddlePanAndWheelAnchor(); testPopupBlocksGraphShortcuts();
        testGroupDragAndResizeAcrossZoom(); testAdditiveMarqueeAndMultiDragAcrossZoom();
        testRerouteBodyAndPortsAcrossZoom(); testCollapseAndCollapsedPortsAcrossZoom();
        testNodeResizeEdgesAndCoalescedRelease();testResizeCancellationMinimumAndGroupBounds();testManualSizeSurvivesCollapseClipboardAndSaving();testResizePresetsUseCardSpaceAndKeepPortsAccessible();
        testSemanticZoomKeepsCardAndPortGeometry();testOverviewCaptionsFitAndReuseTheirLayout();testOverviewBodyRemainsSelectableAndDraggable();
        testGroupHeaderWinsOverWireDoubleClick();
        testInsertedRerouteMovesWithItsCommonGroup();testOverlappingGroupControlsUseOneTopmostTarget();
        testDoubleClickCutsWireAcrossZoom();testWireContextKeepsRerouteAvailable();
        testInlineSettingsApplyAndOwnMouse();testIntegerInlineAndTypedConnections();testSignalTypeColorsSurviveWireStates();testInputSettingsAndSemanticZoom();testStarterLayoutAccommodatesSettings();
        testArrangeGraphIsUndoableAndKeepsExecution();
        testLegacyCustomLayoutExpandsOnce();
        testInlinePathsAndDrivenSettings();
        testCoalescedReleaseFinalizesEveryDrag();testReleaseBackAtOriginRestoresDrag();
        testComboAnimationAdvancesOnceAndRespectsParentAlpha();
        testRerouteIsRenderedAsOnePoint();testReroutePointAcceptsBothWireDirections();
        testWireAnimationIsContinuousAcrossGeometryAndZoom();testWireSplinesAreSmoothAndScaleInvariant();testWirePositionsFollowTheDrawnArcLength();
        testSelectionClipboardIsPortableAndTransactional();
        testQuickAddConnectsBothDirectionsAndKeepsOldInputUntilSuccess();
        testPropertiesPanelAndSelectionFocus();testPropertiesRemainUsableInNarrowEditor();
        testImportedLayoutSnapsWithoutShrinkingGroups();testFailedLoadCannotAutosaveOverTheOriginal();
        testNavigationUsesTheEntireGestureAndRightClickRelease();testRightSelectionNeverPansOrMovesNodes();
        testCatalogSubmenusOpenOnHoverAndCreateANode();
        testWireTieGestureMovementRotationAndClipboard();testTieCancellationAndOrderedRerouteSplit();
        testGroupMoveAndRightSelectionIncludeTies();
        testTieRotationUsesFifteenDegreeSteps();
        testLibrarySelectionAndNodeStateShortcuts();testKeyboardMoveAndDuplicateAreTransactional();
        testCutCrossedConnectionsWithControlRightDrag();
        testCutAnimationRetractsAndHasAFixedBudget();
        testMouseConfirmsKeyboardMoveAndHelpOpens();
        testGroupKeyboardMoveDuplicateAndClipboard();testLeavingEditorCancelsPreviewAndDefersAutosave();
        testKeyboardPortsConnectFollowAndDisconnect();testControlSaveWorksFromProperties();
        testCatalogCanBeTraversedWithKeyboard();testZGestureRemovesCrossedTiesWithoutCuttingWires();
        testRouteCacheInvalidationAndAffineNavigation();testHotNodeDisplayIsThrottledAndEditsAreImmediate();testLargeGraphAnimationAndTextBudgets();
        testDisplayCacheRefreshesAfterRevisionAndReopen();
        std::cout << "Node editor input tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n'; return 1;
    }
}
