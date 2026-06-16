#pragma once
#include "video_output.h"
#include "../util/log.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

struct D3D11Vertex { float pos[2]; float uv[2]; };

class D3D11VideoOutput : public VideoOutput {
public:
    D3D11VideoOutput() = default;
    ~D3D11VideoOutput() override;

    D3D11VideoOutput(const D3D11VideoOutput&) = delete;
    D3D11VideoOutput& operator=(const D3D11VideoOutput&) = delete;

    bool Initialize(HWND hwnd, int w, int h);

    void Render(const uint8_t* data, int src_w, int src_h, int data_size, PixelFormat fmt) override;
    void Resize(int w, int h) override;

private:
    static constexpr const char* VS_SRC =
        "struct VS_IN { float2 pos:POSITION; float2 uv:TEXCOORD; };"
        "struct VS_OUT { float4 pos:SV_POSITION; float2 uv:TEXCOORD; };"
        "VS_OUT main(VS_IN v) {"
        "  VS_OUT o;"
        "  o.pos = float4(v.pos, 0, 1);"
        "  o.uv = v.uv;"
        "  return o;"
        "}";

    static constexpr const char* PS_NV12_SRC =
        "Texture2D texY : register(t0);"
        "Texture2D texUV : register(t1);"
        "SamplerState sam : register(s0);"
        "float4 main(float4 pos:SV_POSITION, float2 uv:TEXCOORD) : SV_TARGET {"
        "  float Y = texY.Sample(sam, uv).r - 0.0625;"
        "  float2 uv_samp = texUV.Sample(sam, uv).rg - 0.5;"
        "  Y *= 1.164;"
        "  return float4("
        "    Y + 1.596 * uv_samp.g,"
        "    Y - 0.391 * uv_samp.r - 0.813 * uv_samp.g,"
        "    Y + 2.018 * uv_samp.r,"
        "    1);"
        "}";

    static constexpr const char* PS_RGB_SRC =
        "Texture2D tex : register(t0);"
        "SamplerState sam : register(s0);"
        "float4 main(float4 pos:SV_POSITION, float2 uv:TEXCOORD) : SV_TARGET {"
        "  return tex.Sample(sam, uv);"
        "}";

    HWND hwnd_ = nullptr;
    int win_w_ = 0;
    int win_h_ = 0;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain* swap_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;

    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_nv12_ = nullptr;
    ID3D11PixelShader* ps_rgb_ = nullptr;
    ID3D11InputLayout* layout_ = nullptr;
    ID3D11Buffer* vbuf_ = nullptr;
    ID3D11SamplerState* sam_ = nullptr;

    ID3D11Texture2D* tex_y_ = nullptr;
    ID3D11ShaderResourceView* srv_y_ = nullptr;
    ID3D11Texture2D* tex_uv_ = nullptr;
    ID3D11ShaderResourceView* srv_uv_ = nullptr;

    ID3D11Texture2D* tex_rgb_ = nullptr;
    ID3D11ShaderResourceView* srv_rgb_ = nullptr;

    int tex_w_ = 0;
    int tex_h_ = 0;
    int tex_is_nv12_ = 0;

    static HRESULT CompileShader(const char* src, const char* target, ID3DBlob** blob);
    void ReleaseD3D11();
    void EnsureTextures(int w, int h, int is_nv12);
    void UploadNV12(const uint8_t* data, int w, int h, int stride);
    void UploadRGB32(const uint8_t* data, int w, int h);
};
