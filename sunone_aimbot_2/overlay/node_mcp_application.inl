// Included after McpJson helpers. Only application-owned settings belong here;
// capture, inference and control values remain owned by workflow nodes.
#include "mcp/application_control.h"
#include "overlay/model_export_control.h"
#include <functional>
#include <thread>

namespace
{
namespace McpJson
{
struct ApplicationField
{
    std::string key,group,type;
    bool restart=false;
    double minimum=0,maximum=0;
    std::vector<std::string> choices;
    std::function<IJsonValue(const Config&)> read;
    std::function<void(Config&,const IJsonValue&)> write;
};
template<class T>
ApplicationField applicationField(const char* key,const char* group,T Config::* member,double minimum=0,double maximum=0,bool restart=false,std::vector<std::string> choices={})
{
    if constexpr(std::is_same_v<T,float>){minimum=static_cast<float>(minimum);maximum=static_cast<float>(maximum);}
    ApplicationField field;field.key=key;field.group=group;field.restart=restart;field.minimum=minimum;field.maximum=maximum;field.choices=choices;
    if constexpr(std::is_same_v<T,bool>)field.type="boolean";
    else if constexpr(std::is_same_v<T,int>)field.type="integer";
    else if constexpr(std::is_same_v<T,float>)field.type="number";
    else if constexpr(std::is_same_v<T,std::string>)field.type="string";
    else field.type="array";
    field.read=[member](const Config& source)->IJsonValue{
        if constexpr(std::is_same_v<T,bool>)return JsonValue::CreateBooleanValue(source.*member);
        else if constexpr(std::is_arithmetic_v<T>)return JsonValue::CreateNumberValue(source.*member);
        else if constexpr(std::is_same_v<T,std::string>)return JsonValue::CreateStringValue(winrt::to_hstring(source.*member));
        else {JsonArray result;for(const auto& key:source.*member)result.Append(JsonValue::CreateStringValue(winrt::to_hstring(key)));return result.as<IJsonValue>();}
    };
    field.write=[member,minimum,maximum,choices](Config& target,const IJsonValue& value){
        if constexpr(std::is_same_v<T,bool>)target.*member=value.GetBoolean();
        else if constexpr(std::is_arithmetic_v<T>)
        {
            const double number=value.GetNumber();
            if(!std::isfinite(number)||number<minimum||number>maximum||(std::is_integral_v<T>&&std::trunc(number)!=number))throw std::invalid_argument("Setting is outside its advertised range.");
            target.*member=static_cast<T>(number);
        }
        else if constexpr(std::is_same_v<T,std::string>)
        {
            auto next=winrt::to_string(value.GetString());
            if(next.size()>1024||next.find_first_of(std::string("\0\r\n",3))!=std::string::npos)throw std::invalid_argument("Setting text must be at most 1024 bytes without NUL or line breaks.");
            if(!choices.empty()&&std::find(choices.begin(),choices.end(),next)==choices.end())throw std::invalid_argument("Setting is not one of its advertised choices.");
            target.*member=std::move(next);
        }
        else
        {
            std::vector<std::string> next;const auto array=value.GetArray();
            if(array.Size()<1||array.Size()>16)throw std::invalid_argument("Use 1..16 key names, or [\"None\"] to clear a binding.");
            for(const auto& item:array){auto key=winrt::to_string(item.GetString());if(std::find(key_names.begin(),key_names.end(),key)==key_names.end())throw std::invalid_argument("Unknown hotkey name. Use key_choices from get_application.");next.push_back(std::move(key));}
            target.*member=std::move(next);
        }
    };return field;
}
const std::vector<ApplicationField>& applicationFields()
{
    static const auto fields=[] {
        std::vector<ApplicationField> result;
#define FIELD(name,group,...) result.push_back(applicationField(#name,group,&Config::name,__VA_ARGS__))
        FIELD(input_method,"input",0,0,false,(std::vector<std::string>{"WIN32","GHUB","RAZER","ARDUINO","RP2350","TEENSY41","TEENSY41_HID","KMBOX_NET","KMBOX_A","MAKCU"}));
        FIELD(arduino_baudrate,"input",9600,921600);FIELD(arduino_port,"input",0,0);FIELD(arduino_16_bit_mouse,"input",0,0);FIELD(arduino_enable_keys,"input",0,0);
        FIELD(rp2350_baudrate,"input",9600,921600);FIELD(rp2350_port,"input",0,0);FIELD(rp2350_16_bit_mouse,"input",0,0);FIELD(rp2350_enable_keys,"input",0,0);
        FIELD(teensy_hid_serial,"input",0,0);FIELD(teensy_hid_vid_filter,"input",0,0);FIELD(teensy_hid_pid_filter,"input",0,0);
        FIELD(teensy_hid_usage_page,"input",0,65535);FIELD(teensy_hid_usage_id,"input",0,65535);FIELD(teensy_hid_open_index,"input",0,255);
        FIELD(teensy_hid_packet_timeout_ms,"input",0,60000);FIELD(teensy_hid_reconnect_interval_ms,"input",0,60000);
        FIELD(kmbox_net_ip,"input",0,0);FIELD(kmbox_net_port,"input",0,0);FIELD(kmbox_net_uuid,"input",0,0);FIELD(kmbox_a_pidvid,"input",0,0);
        FIELD(makcu_baudrate,"input",9600,921600);FIELD(makcu_port,"input",0,0);FIELD(makcu_enable_keys,"input",0,0);
        FIELD(button_targeting,"hotkeys",0,0);FIELD(button_shoot,"hotkeys",0,0);FIELD(button_zoom,"hotkeys",0,0);FIELD(button_exit,"hotkeys",0,0);
        FIELD(button_pause,"hotkeys",0,0);FIELD(button_reload_config,"hotkeys",0,0);FIELD(button_open_overlay,"hotkeys",0,0);FIELD(enable_arrows_settings,"hotkeys",0,0);
        FIELD(overlay_opacity,"overlay",252,255);FIELD(overlay_ui_scale,"overlay",0.85,1.35);FIELD(overlay_sidebar_collapsed,"overlay",0,0);FIELD(overlay_exclude_from_capture,"overlay",0,0);
        FIELD(local_mcp_enabled,"agents",0,0);FIELD(verbose,"diagnostics",0,0);
        FIELD(cpuCoreReserveCount,"resources",0,std::max(0,static_cast<int>(std::thread::hardware_concurrency())-1),true);
        MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
        FIELD(systemMemoryReserveMB,"resources",0,GlobalMemoryStatusEx(&memory)?static_cast<double>(memory.ullTotalPhys/(1024*1024)):1048576,true);
#ifdef USE_CUDA
        FIELD(gpuMemoryReserveMB,"resources",0,65536,true);FIELD(enableGpuExclusiveMode,"resources",0,0,true);
#endif
#undef FIELD
        return result;
    }();return fields;
}
JsonObject applicationValues(const Config& source)
{
    JsonObject values;for(const auto& field:applicationFields())values.Insert(winrt::to_hstring(field.key),field.read(source));return values;
}
JsonObject applicationSnapshot()
{
    JsonObject result;put(result,L"ok",true);const auto values=applicationValues(config);put(result,L"settings",values);put(result,L"settings_version",version(encode(values)));
    JsonArray fields,keys,tabs;
    for(const auto& field:applicationFields())
    {
        JsonObject item;put(item,L"key",field.key);put(item,L"group",field.group);put(item,L"type",field.type);put(item,L"requires_restart",field.restart);
        if(field.type=="integer"||field.type=="number"){put(item,L"minimum",field.minimum);put(item,L"maximum",field.maximum);}
        if(!field.choices.empty()){JsonArray choices;for(const auto& choice:field.choices)choices.Append(JsonValue::CreateStringValue(winrt::to_hstring(choice)));put(item,L"choices",choices);}
        fields.Append(item);
    }
    for(const auto& key:key_names)keys.Append(JsonValue::CreateStringValue(winrt::to_hstring(key)));
    for(const char* tab:{"input","hotkeys","overlay","system","export","stats","debug"})tabs.Append(JsonValue::CreateStringValue(winrt::to_hstring(tab)));
    put(result,L"fields",fields);put(result,L"key_choices",keys);put(result,L"tabs",tabs);
    const auto state=Overlay_AgentState();JsonObject overlay;put(overlay,L"visible",state.visible);put(overlay,L"paused",state.paused);put(overlay,L"preview",state.preview);put(overlay,L"window_mode",state.windowMode);put(overlay,L"tab",state.tab);put(overlay,L"workflow",state.workflow);put(result,L"state",overlay);
#ifdef USE_CUDA
    put(result,L"backend","CUDA");put(result,L"model_export_supported",true);
#else
    put(result,L"backend","DML");put(result,L"model_export_supported",false);
#endif
    return result;
}
JsonObject applicationEdit(const JsonObject& command)
{
    const auto before=applicationSnapshot();if(text(command,L"expected_settings_version")!=text(before,L"settings_version"))return error("Settings version conflict. Read get_application and retry; no settings were applied.");
    Config next=config;const auto changes=command.GetNamedObject(L"settings");if(changes.Size()==0)return error("Provide at least one setting.");
    JsonArray restart;
    for(const auto& entry:changes)
    {
        const auto key=winrt::to_string(entry.Key());const auto& fields=applicationFields();const auto found=std::find_if(fields.begin(),fields.end(),[&](const auto& field){return field.key==key;});
        if(found==fields.end())return error("Unknown application setting: "+key+". Graph-owned settings must be edited through nodes.");
        found->write(next,entry.Value());if(found->restart&&found->read(config).Stringify()!=found->read(next).Stringify())restart.Append(JsonValue::CreateStringValue(entry.Key()));
    }
    if(command.GetNamedBoolean(L"dry_run",false))
    {auto result=before;put(result,L"applied",false);put(result,L"proposed_settings",applicationValues(next));put(result,L"restart_required",restart);return result;}
    const Config previous=config;config=std::move(next);Overlay_ApplicationSettingsChanged(previous);
    const bool saved=config.saveConfig();auto result=applicationSnapshot();put(result,L"applied",true);put(result,L"saved",saved);put(result,L"restart_required",restart);
    if(!saved)put(result,L"warning","Settings are active, but config.ini could not be written.");return result;
}
JsonObject modelExport(const JsonObject& command,bool userEditing)
{
    const auto action=text(command,L"action",L"status");
    if(action!="status"&&userEditing)return error("The user is editing. Retry after the current interaction.");
    if(action=="cancel")ModelExport::cancel();
    else if(action=="start")
    {
        const auto options=command.GetNamedObject(L"options",JsonObject{});ModelExport::Options settings;
        settings.fp16=options.GetNamedBoolean(L"fp16",true);settings.fp8=options.GetNamedBoolean(L"fp8",false);settings.fixed=options.GetNamedBoolean(L"fixed",false);settings.rebuild=options.GetNamedBoolean(L"rebuild",false);
        const auto dimension=[&](const wchar_t* key,int fallback){const auto number=options.GetNamedNumber(key,fallback);if(!std::isfinite(number)||std::trunc(number)!=number||number<32||number>8192)throw std::invalid_argument("Export dimensions must be whole numbers from 32 to 8192.");return static_cast<int>(number);};
        settings.resolution=dimension(L"resolution",320);settings.minimum=dimension(L"minimum",160);settings.optimal=dimension(L"optimal",320);settings.maximum=dimension(L"maximum",640);
        std::string issue;if(!ModelExport::start(text(command,L"path"),command.GetNamedBoolean(L"depth",false),settings,issue))return error(issue);
    }
    else if(action!="status")return error("Unknown model export action.");
    const auto snapshot=ModelExport::status();const char* names[]={"idle","running","complete","failed","cancelled"};JsonObject result;
    put(result,L"ok",true);put(result,L"state",names[static_cast<int>(snapshot.state)]);put(result,L"input",snapshot.input);put(result,L"output",snapshot.output);put(result,L"message",snapshot.message);return result;
}
}
}
