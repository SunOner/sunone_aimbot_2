#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include "keyboard/graph_key_state.h"
#include "keyboard/keycodes.h"
#include "mouse/MouseInput.h"
#include "runtime/input_state.h"
#include <array>
#include <vector>
extern std::unique_ptr<IMouseInput> activeMouseInputOwner;
namespace GraphInput
{
bool resolveKey(int key,IMouseInput* input,bool systemDown)
{
    if(key<=0||key>=256)return false;
    if(input&&input->isOpen())
    {
        static const auto names=[]{std::array<std::vector<std::string>,256> result;
            for(const auto& entry:KeyCodes::key_code_map)if(entry.second>0&&entry.second<256)result[entry.second].push_back(entry.first);return result;}();
        for(const auto& name:names[key])if(input->keyPressed(name))return true;
        const bool mouse=key==VK_LBUTTON||key==VK_RBUTTON||key==VK_MBUTTON||key==VK_XBUTTON1||key==VK_XBUTTON2;
        if(mouse&&input->hasPhysicalButtonState())return false;
    }
    return systemDown;
}
bool keyDown(int key)
{
    if(key<=0||key>=256||editorInputCaptured.load())return false;
    const bool systemDown=(GetAsyncKeyState(key)&0x8000)!=0;
    std::lock_guard<std::mutex> lock(inputDevicesMutex);
    return !editorInputCaptured.load()&&resolveKey(key,activeMouseInputOwner.get(),systemDown);
}
}
