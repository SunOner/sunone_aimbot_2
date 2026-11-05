#pragma once
#include <string>
class Config;
struct AgentApplicationState
{
    bool visible=false,paused=false,preview=false;
    std::string windowMode,tab;
    int workflow=0;
};
AgentApplicationState Overlay_AgentState();
bool Overlay_AgentAction(const std::string& action,const std::string& tab,std::string& error);
void Overlay_ApplicationSettingsChanged(const Config& before);
