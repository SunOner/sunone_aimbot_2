#include "nodes/preview_image.h"
#include "nodes/vision_runtime.h"
#include <opencv2/imgproc.hpp>
#ifdef USE_CUDA
#include <opencv2/cudawarping.hpp>
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace NodeVision
{
NodeGraph::Preview makePreview(const NodeGraph::Value& value,const NodeGraph::DetectionStream& detections,const NodeGraph::Node& settings)
{
    using namespace NodeGraph;
    const int originalWidth=value.mask?value.mask->width:value.frame?value.frame->width:0;
    const int originalHeight=value.mask?value.mask->height:value.frame?value.frame->height:0;
    if(originalWidth<1||originalHeight<1)return {};
    const int limit=static_cast<int>(std::clamp(parameter(settings,"max_size"),96.0,640.0));
    const double scale=std::min(1.0,static_cast<double>(limit)/std::max(originalWidth,originalHeight));
    const cv::Size size(std::max(1,static_cast<int>(std::lround(originalWidth*scale))),std::max(1,static_cast<int>(std::lround(originalHeight*scale))));
    cv::Mat pixels;
    if(value.mask&&value.mask->storage)pixels=std::static_pointer_cast<const MaskStorage>(value.mask->storage)->mask;
    else if(value.frame&&value.frame->storage)
    {
        const auto native=std::static_pointer_cast<const FrameStorage>(value.frame->storage);
        std::lock_guard<std::mutex> lock(native->cpuMutex);
#ifdef USE_CUDA
        if(!native->gpu.empty())
        {
            cv::cuda::GpuMat thumbnail;
            cv::cuda::resize(native->gpu,thumbnail,size,0,0,cv::INTER_LINEAR);
            thumbnail.download(pixels);
        }
        else
#endif
            pixels=native->image;
    }
    if(pixels.empty())return {};
    cv::Mat small,rgba;
    if(pixels.size()!=size)cv::resize(pixels,small,size,0,0,cv::INTER_AREA);else small=pixels;
    if(value.type==ValueType::Depth)
    {
        if(small.channels()!=1)throw std::runtime_error("Depth preview requires a single-channel image.");
        cv::Mat values;small.convertTo(values,CV_32F);
        float minimum=std::numeric_limits<float>::max(),maximum=std::numeric_limits<float>::lowest();
        for(int y=0;y<values.rows;++y)for(int x=0;x<values.cols;++x)
        {const auto v=values.at<float>(y,x);if(std::isfinite(v)){minimum=std::min(minimum,v);maximum=std::max(maximum,v);}}
        cv::Mat gray(values.size(),CV_8UC1,cv::Scalar(0));
        if(maximum>minimum)for(int y=0;y<values.rows;++y)for(int x=0;x<values.cols;++x)
        {const auto v=values.at<float>(y,x);if(std::isfinite(v))gray.at<std::uint8_t>(y,x)=cv::saturate_cast<std::uint8_t>((static_cast<double>(v)-minimum)*255/(static_cast<double>(maximum)-minimum));}
        const int colors=static_cast<int>(parameter(settings,"colormap"));
        if(colors==1)cv::cvtColor(gray,rgba,cv::COLOR_GRAY2RGBA);
        else{cv::Mat color;cv::applyColorMap(gray,color,colors==2?cv::COLORMAP_INFERNO:cv::COLORMAP_TURBO);cv::cvtColor(color,rgba,cv::COLOR_BGR2RGBA);}
    }
    else
    {
        cv::Mat bytes;if(small.depth()!=CV_8U)small.convertTo(bytes,CV_8U);else bytes=small;
        if(bytes.channels()==1)cv::cvtColor(bytes,rgba,cv::COLOR_GRAY2RGBA);
        else if(bytes.channels()==3)cv::cvtColor(bytes,rgba,cv::COLOR_BGR2RGBA);
        else if(bytes.channels()==4)
        {
            cv::cvtColor(bytes,rgba,cv::COLOR_BGRA2RGBA);
            for(int y=0;y<rgba.rows;++y)for(int x=0;x<rgba.cols;++x)rgba.at<cv::Vec4b>(y,x)[3]=255;
        }
        else throw std::runtime_error("Preview requires a grayscale, BGR or BGRA image.");
    }
    const auto generation=value.mask?value.mask->generation:value.frame->generation;
    const auto version=value.mask?value.mask->version:value.frame->version;
    if(detections&&parameter(settings,"draw_boxes")!=0&&detections->generation==generation&&detections->version==version)
    {
        const double sx=rgba.cols/(detections->width>0?detections->width:originalWidth),sy=rgba.rows/(detections->height>0?detections->height:originalHeight);
        for(const auto& d:detections->objects)
        {
            if(!std::isfinite(d.x)||!std::isfinite(d.y)||!std::isfinite(d.width)||!std::isfinite(d.height)||d.width<=0||d.height<=0)continue;
            const auto coord=[](double v,int limit){return static_cast<int>(std::clamp(v,0.0,static_cast<double>(limit-1)));};
            const cv::Point a(coord(d.x*sx,rgba.cols),coord(d.y*sy,rgba.rows)),b(coord((d.x+d.width)*sx,rgba.cols),coord((d.y+d.height)*sy,rgba.rows));
            cv::rectangle(rgba,a,b,cv::Scalar(102,225,169,255),1,cv::LINE_AA);
        }
    }
    auto result=std::make_shared<PreviewImage>();result->width=rgba.cols;result->height=rgba.rows;
    result->sourceWidth=originalWidth;result->sourceHeight=originalHeight;result->sourceType=value.type;result->version=version;result->generation=generation;
    result->rgba.resize(static_cast<std::size_t>(rgba.cols)*rgba.rows*4);
    for(int y=0;y<rgba.rows;++y)std::memcpy(result->rgba.data()+static_cast<std::size_t>(y)*rgba.cols*4,rgba.ptr(y),static_cast<std::size_t>(rgba.cols)*4);
    return result;
}
}
