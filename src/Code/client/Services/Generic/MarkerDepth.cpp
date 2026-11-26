#include <TiltedOnlinePCH.h>
#include <Services/MarkerDepth.h>
#include <BSGraphics/BSGraphicsRenderer.h>
#include <imgui.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace MarkerDepth
{
namespace
{
using Microsoft::WRL::ComPtr;
ComPtr<ID3D11PixelShader> shader;
ComPtr<ID3D11Buffer> constants;
ComPtr<ID3D11ShaderResourceView> previousDepth;
ComPtr<ID3D11Buffer> previousConstants;
ID3D11Device* lastDevice{};
struct Parameters { float width, height, depth, sign; };
Parameters parameters;
BSGraphics::RendererData* renderer{};
ID3D11ShaderResourceView* depthView{};
ComPtr<ID3D11Resource> copySource;
ComPtr<ID3D11Texture2D> depthCopy;
ComPtr<ID3D11ShaderResourceView> depthCopyView;
D3D11_TEXTURE2D_DESC copyDesc{};

bool Prepare(ID3D11Device* device)
{
    if (lastDevice == device) return shader && constants;
    lastDevice = device;
    shader.Reset();
    constants.Reset();
    copySource.Reset();
    depthCopy.Reset();
    depthCopyView.Reset();
    copyDesc = {};
    constexpr char source[] = R"(
        Texture2D font : register(t0); SamplerState fontSampler : register(s0);
        Texture2D<float> scene : register(t1);
        cbuffer Frame : register(b1) { float2 display; float z; float signZ; }
        float4 main(float4 pos: SV_POSITION, float4 color: COLOR0,float2 uv: TEXCOORD0) : SV_Target {
            uint w,h; scene.GetDimensions(w,h);
            int2 pixel = clamp(int2(pos.xy/display*float2(w,h)),int2(0,0),int2(w-1,h-1));
            clip((scene.Load(int3(pixel,0))-z)*signZ);
            return color*font.Sample(fontSampler,uv);
        })";
    ComPtr<ID3DBlob> compiled;
    if (FAILED(D3DCompile(source, sizeof(source)-1, nullptr, nullptr, nullptr, "main", "ps_4_0", 0,0,&compiled,nullptr)) ||
        FAILED(device->CreatePixelShader(compiled->GetBufferPointer(),compiled->GetBufferSize(),nullptr,&shader))) return false;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(Parameters);
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device->CreateBuffer(&desc,nullptr,&constants));
}
void Bind(const ImDrawList*, const ImDrawCmd*)
{
    auto* ctx = renderer->pContext;
    ctx->PSGetShaderResources(1,1,previousDepth.ReleaseAndGetAddressOf());
    if (copySource && depthCopy) ctx->CopyResource(depthCopy.Get(),copySource.Get());
    ctx->PSGetConstantBuffers(1,1,previousConstants.ReleaseAndGetAddressOf());
    ctx->UpdateSubresource(constants.Get(),0,nullptr,&parameters,0,0);
    ctx->PSSetConstantBuffers(1,1,constants.GetAddressOf());
    ctx->PSSetShaderResources(1,1,&depthView);
    ctx->PSSetShader(shader.Get(),nullptr,0);
}
void Restore(const ImDrawList*, const ImDrawCmd*)
{
    auto* ctx = renderer->pContext;
    ctx->PSSetShaderResources(1,1,previousDepth.GetAddressOf());
    ctx->PSSetConstantBuffers(1,1,previousConstants.GetAddressOf());
    previousConstants.Reset();
    previousDepth.Reset();
    copySource.Reset();
}
}
bool Begin(ImDrawList* draw, float width, float height, float depth, bool reversed)
{
    renderer = BSGraphics::GetRendererData();
    if (!renderer || !renderer->pDepthStencilTargetsA[0].pSRViewDepth || !std::isfinite(depth) || depth < 0 || depth > 1)
        return false;
    if (!renderer->pContext || !renderer->pForwarder || !Prepare(renderer->pForwarder)) return false;
    auto* sourceView = renderer->pDepthStencilTargetsA[0].pSRViewDepth;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    sourceView->GetDesc(&view);
    if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return false;
    ComPtr<ID3D11Resource> source;
    sourceView->GetResource(&source);
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(source.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1) return false;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    if (!depthCopy || desc.Width != copyDesc.Width || desc.Height != copyDesc.Height ||
        desc.Format != copyDesc.Format || desc.MipLevels != copyDesc.MipLevels)
    {
        depthCopyView.Reset();
        depthCopy.Reset();
        if (FAILED(renderer->pForwarder->CreateTexture2D(&desc,nullptr,&depthCopy))) return false;
        copyDesc = desc;
    }
    // Copy the actual primary depth instead of sampling an unrelated secondary target.
    // The copy has no writable depth binding, so it cannot conflict with the game DSV.
    if (FAILED(renderer->pForwarder->CreateShaderResourceView(depthCopy.Get(),&view,depthCopyView.ReleaseAndGetAddressOf())))
        return false;
    copySource = source;
    depthView = depthCopyView.Get();
    parameters = {width,height,depth,reversed ? -1.f : 1.f};
    draw->AddCallback(Bind,nullptr);
    return true;
}
void End(ImDrawList* draw)
{
    draw->AddCallback(Restore,nullptr);
    draw->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
}
}
