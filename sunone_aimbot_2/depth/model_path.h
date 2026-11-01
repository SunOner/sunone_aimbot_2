#pragma once

#include <filesystem>
#include <string>

namespace depth_anything
{
inline bool resolveModelPath(const std::string& value,std::string& result,std::string& error)
{
    namespace fs=std::filesystem;
    result.clear();error.clear();
    if(value.empty()){error="Select a model for this Depth inference node.";return false;}
    try
    {
        const auto requested=fs::u8path(value);
        const auto regular=[](const fs::path& path){std::error_code ec;return fs::is_regular_file(path,ec);};
        auto selected=requested;
        if(!regular(selected)&&!requested.is_absolute()&&!requested.has_parent_path())
        {
            if(regular(fs::path("depth_models")/requested))selected=fs::path("depth_models")/requested;
            else if(regular(fs::path("models")/requested))selected=fs::path("models")/requested;
        }
        if(!regular(selected)){error="Depth model file not found: "+value;return false;}
        std::error_code ec;const auto absolute=fs::absolute(selected,ec);
        if(ec){error="Cannot resolve depth model: "+value;return false;}
        result=absolute.lexically_normal().u8string();return true;
    }
    catch(const std::exception&){error="Invalid depth model path: "+value;return false;}
}
}
