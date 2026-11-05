#pragma once
#include <functional>
#include <string>
#include "client_setup.h"

namespace WorkflowMcp
{
struct Status { bool running=false; std::string message,instance,lastTool; unsigned long long calls=0; };
bool start(std::function<std::string(const std::string&)> handler);
void stop();
// Called only by the editor thread, including while its window is hidden.
void process();
Status status();
std::string clientConfiguration(Client client=Client::Generic,bool installCommand=false);
}
