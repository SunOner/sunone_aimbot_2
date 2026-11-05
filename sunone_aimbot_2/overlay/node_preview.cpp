#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "overlay/node_preview.h"
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <map>

namespace NodePreview
{
namespace
{
using Microsoft::WRL::ComPtr;
struct Texture
{
    ComPtr<ID3D11Texture2D> image;
    ComPtr<ID3D11ShaderResourceView> view;
    NodeGraph::Preview uploaded;
    ID3D11Device* device=nullptr;
    int width=0,height=0;
};
std::map<int,Texture> textures;
}
ImTextureID texture(int node,const NodeGraph::Preview& image,ID3D11Device* device,ID3D11DeviceContext* context)
{
    if(image&&image->width==0){textures.erase(node);return ImTextureID(0);}
    if(!device||!context||!image||image->width<1||image->height<1||image->width>640||image->height>640||
        image->rgba.size()!=static_cast<std::size_t>(image->width)*image->height*4)return ImTextureID(0);
    if(textures.size()>=NodeGraph::MaxLivePreviews&&!textures.count(node))return ImTextureID(0);
    auto& state=textures[node];
    if(state.device!=device||state.width!=image->width||state.height!=image->height)
    {
        state=Texture{};D3D11_TEXTURE2D_DESC desc{};desc.Width=image->width;desc.Height=image->height;desc.MipLevels=desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DYNAMIC;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&state.image))||FAILED(device->CreateShaderResourceView(state.image.Get(),nullptr,&state.view)))return ImTextureID(0);
        state.device=device;state.width=image->width;state.height=image->height;
    }
    if(state.uploaded!=image)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(context->Map(state.image.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped)))return ImTextureID(0);
        for(int y=0;y<image->height;++y)std::memcpy(static_cast<std::uint8_t*>(mapped.pData)+static_cast<std::size_t>(y)*mapped.RowPitch,
            image->rgba.data()+static_cast<std::size_t>(y)*image->width*4,static_cast<std::size_t>(image->width)*4);
        context->Unmap(state.image.Get(),0);state.uploaded=image;
    }
    return reinterpret_cast<ImTextureID>(state.view.Get());
}
void retain(const std::vector<int>& nodes)
{
    for(auto it=textures.begin();it!=textures.end();)if(std::find(nodes.begin(),nodes.end(),it->first)==nodes.end())it=textures.erase(it);else++it;
}
void clear(){textures.clear();}
}
