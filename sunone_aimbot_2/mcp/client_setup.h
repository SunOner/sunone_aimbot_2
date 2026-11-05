#pragma once
#include <iomanip>
#include <sstream>
#include <string>

namespace WorkflowMcp
{
enum class Client { ClaudeCode, ClaudeDesktop, Codex, Generic };
inline std::string shellQuote(const std::string& text)
{
    std::string result="'";for(char c:text){result+=c;if(c=='\'')result+='\'';}return result+"'";
}
inline std::string connectionText(Client client,const std::string& script,const std::string& instance,bool installCommand=false)
{
    const std::string name=instance=="default"?"sunone-workflows":"sunone-workflows-"+instance;
    std::ostringstream output;
    if(installCommand)
    {
        output<<(client==Client::Codex?"codex mcp add ":"claude mcp add --scope user --transport stdio ")<<shellQuote(name)
              <<" -- python -u "<<shellQuote(script)<<" --instance "<<shellQuote(instance);return output.str();
    }
    if(client==Client::Codex)
    {
        output<<"[mcp_servers."<<name<<"]\ncommand = \"python\"\nargs = [\"-u\", "<<std::quoted(script)<<", \"--instance\", "<<std::quoted(instance)<<"]\n";
        return output.str();
    }
    output<<"{\n  \"mcpServers\": {\n    "<<std::quoted(name)<<": {\n";
    if(client==Client::ClaudeCode)output<<"      \"type\": \"stdio\",\n";
    output<<"      \"command\": \"python\",\n      \"args\": [\"-u\", "<<std::quoted(script)<<", \"--instance\", "<<std::quoted(instance)<<"]\n    }\n  }\n}\n";
    return output.str();
}
}
