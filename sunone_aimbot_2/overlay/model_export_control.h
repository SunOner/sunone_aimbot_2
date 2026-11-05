#pragma once
#include "export_job.h"
namespace ModelExport
{
struct Options
{
    bool fp16=true,fp8=false,fixed=false,rebuild=false;
    int resolution=320,minimum=160,optimal=320,maximum=640;
};
Result status();
bool start(const std::string& input,bool depth,const Options& options,std::string& error);
void cancel();
}
