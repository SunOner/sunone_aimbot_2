#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <sddl.h>
#include "workflow_host.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace WorkflowMcp
{
namespace
{
constexpr DWORD MaxMessage=16*1024*1024;
struct Request { std::string text; std::promise<std::string> reply; std::atomic<int> state{0}; };
struct Host
{
    std::mutex mutex;
    std::deque<std::shared_ptr<Request>> requests;
    std::function<std::string(const std::string&)> handler;
    std::thread thread;
    HANDLE stopped=nullptr;
    Status state;
    ~Host(){if(thread.joinable()){SetEvent(stopped);thread.join();}if(stopped)CloseHandle(stopped);}
} host;
std::string utf8(const std::wstring& value)
{
    const int size=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(size,'\0');if(size)WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),size,nullptr,nullptr);return result;
}
std::wstring userSid()
{
    HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return {};
    DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<unsigned char> bytes(size);
    const bool valid=GetTokenInformation(token,TokenUser,bytes.data(),size,&size)!=FALSE;CloseHandle(token);
    LPWSTR sid=nullptr;if(!valid||!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid))return {};
    std::wstring result(sid);LocalFree(sid);return result;
}
bool transfer(HANDLE pipe,void* buffer,DWORD bytes,bool write)
{
    auto* cursor=static_cast<unsigned char*>(buffer);
    while(bytes)
    {
        OVERLAPPED operation{};operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!operation.hEvent)return false;
        DWORD count=0;
        BOOL done=write?WriteFile(pipe,cursor,bytes,&count,&operation):ReadFile(pipe,cursor,bytes,&count,&operation);
        bool success=done!=FALSE;
        if(!success&&GetLastError()==ERROR_IO_PENDING)
        {
            HANDLE events[]{host.stopped,operation.hEvent};
            success=WaitForMultipleObjects(2,events,FALSE,5000)==WAIT_OBJECT_0+1;
            if(success)success=GetOverlappedResult(pipe,&operation,&count,FALSE)!=FALSE;
            else {CancelIoEx(pipe,&operation);GetOverlappedResult(pipe,&operation,&count,TRUE);}
        }
        CloseHandle(operation.hEvent);
        if(!success||!count)return false;
        cursor+=count;bytes-=count;
    }
    return true;
}
void serve(HANDLE first,std::wstring endpoint,PSECURITY_DESCRIPTOR descriptor)
{
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};HANDLE pipe=first;
    while(WaitForSingleObject(host.stopped,0)!=WAIT_OBJECT_0)
    {
        OVERLAPPED connection{};connection.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!connection.hEvent)break;
        BOOL connected=ConnectNamedPipe(pipe,&connection);DWORD error=connected?ERROR_SUCCESS:GetLastError();
        if(!connected&&error==ERROR_IO_PENDING)
        {
            HANDLE events[]{host.stopped,connection.hEvent};
            if(WaitForMultipleObjects(2,events,FALSE,INFINITE)==WAIT_OBJECT_0+1){DWORD ignored;connected=GetOverlappedResult(pipe,&connection,&ignored,FALSE);}
            else {CancelIoEx(pipe,&connection);DWORD ignored;GetOverlappedResult(pipe,&connection,&ignored,TRUE);}
        }
        else if(error==ERROR_PIPE_CONNECTED)connected=TRUE;
        CloseHandle(connection.hEvent);
        if(connected)
        {
            DWORD length=0;
            if(transfer(pipe,&length,sizeof(length),false)&&length>0&&length<=MaxMessage)
            {
                auto request=std::make_shared<Request>();request->text.resize(length);
                if(transfer(pipe,request->text.data(),length,false))
                {
                    auto reply=request->reply.get_future();
                    {std::lock_guard<std::mutex> lock(host.mutex);while(!host.requests.empty()&&host.requests.front()->state.load()==2)host.requests.pop_front();if(host.requests.size()<4)host.requests.push_back(request);else request->reply.set_value("{\"ok\":false,\"error\":\"Editor request queue is full; no changes were applied.\"}");}
                    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
                    bool cancelled=false;
                    while(reply.wait_for(std::chrono::milliseconds(20))!=std::future_status::ready)
                    {
                        int queued=0;
                        if(WaitForSingleObject(host.stopped,0)==WAIT_OBJECT_0){request->state.compare_exchange_strong(queued,2);cancelled=true;break;}
                        if(std::chrono::steady_clock::now()>=deadline&&request->state.compare_exchange_strong(queued,2)){cancelled=true;break;}
                    }
                    std::string response=cancelled?"{\"ok\":false,\"error\":\"Editor is unavailable; the request was not applied.\"}":reply.get();
                    length=static_cast<DWORD>(response.size());
                    if(length<=MaxMessage&&transfer(pipe,&length,sizeof(length),true)&&transfer(pipe,response.data(),length,true))
                    {
                        // DisconnectNamedPipe discards unread buffered bytes.
                        // A bounded client acknowledgement drains the response
                        // without an uncancellable FlushFileBuffers wait.
                        unsigned char acknowledged=0;transfer(pipe,&acknowledged,1,false);
                    }
                }
            }
        }
        DisconnectNamedPipe(pipe);CloseHandle(pipe);pipe=INVALID_HANDLE_VALUE;
        if(WaitForSingleObject(host.stopped,0)==WAIT_OBJECT_0)break;
        pipe=CreateNamedPipeW(endpoint.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,&attributes);
        if(pipe==INVALID_HANDLE_VALUE)break;
    }
    if(pipe!=INVALID_HANDLE_VALUE)CloseHandle(pipe);
    LocalFree(descriptor);
    std::lock_guard<std::mutex> lock(host.mutex);host.state.running=false;
}
}
bool start(std::function<std::string(const std::string&)> handler)
{
    stop();
    wchar_t environment[65]{};DWORD length=GetEnvironmentVariableW(L"SUNONE_MCP_INSTANCE",environment,65);
    std::wstring instance=length?std::wstring(environment,std::min<DWORD>(length,64)):L"default";
    const bool valid=length<65&&!instance.empty()&&std::all_of(instance.begin(),instance.end(),[](wchar_t c){return (c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'-'||c==L'_';});
    const auto sid=userSid();PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!valid||sid.empty()||!ConvertStringSecurityDescriptorToSecurityDescriptorW((L"D:P(A;;GA;;;"+sid+L")").c_str(),SDDL_REVISION_1,&descriptor,nullptr))
    {std::lock_guard<std::mutex> lock(host.mutex);host.state.message="Cannot create a private MCP endpoint (check SUNONE_MCP_INSTANCE).";return false;}
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
    const auto endpoint=L"\\\\.\\pipe\\sunone.workflows."+sid+L"."+instance;
    HANDLE pipe=CreateNamedPipeW(endpoint.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,&attributes);
    if(pipe==INVALID_HANDLE_VALUE){LocalFree(descriptor);std::lock_guard<std::mutex> lock(host.mutex);host.state.message="MCP instance is already in use or unavailable.";return false;}
    host.stopped=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!host.stopped){CloseHandle(pipe);LocalFree(descriptor);return false;}
    {std::lock_guard<std::mutex> lock(host.mutex);host.handler=std::move(handler);host.state={true,"Local connection ready",utf8(instance),{},0};}
    host.thread=std::thread(serve,pipe,endpoint,descriptor);return true;
}
void stop()
{
    if(host.stopped)SetEvent(host.stopped);
    if(host.thread.joinable())host.thread.join();
    if(host.stopped){CloseHandle(host.stopped);host.stopped=nullptr;}
    std::lock_guard<std::mutex> lock(host.mutex);host.requests.clear();host.handler={};host.state.running=false;host.state.message="Local MCP is disabled";
}
void process()
{
    std::shared_ptr<Request> request;
    {std::lock_guard<std::mutex> lock(host.mutex);if(host.requests.empty())return;request=std::move(host.requests.front());host.requests.pop_front();}
    int queued=0;if(!request->state.compare_exchange_strong(queued,1))return;
    try{request->reply.set_value(host.handler(request->text));}
    catch(...){request->reply.set_value("{\"ok\":false,\"error\":\"MCP request failed.\"}");}
    std::lock_guard<std::mutex> lock(host.mutex);++host.state.calls;
}
Status status(){std::lock_guard<std::mutex> lock(host.mutex);return host.state;}
std::string clientConfiguration(Client client,bool installCommand)
{
    wchar_t path[32768]{};const DWORD size=GetModuleFileNameW(nullptr,path,32768);std::wstring script(path,size);
    script=script.substr(0,script.find_last_of(L"\\/"))+L"\\workflow_mcp.py";
    return connectionText(client,utf8(script),status().instance,installCommand);
}
}
