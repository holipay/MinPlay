#define INITGUID
#include "d3d11_renderer.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <initguid.h>
#include "../util/log.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static const char g_vs_source[] =
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOut main(uint vid : SV_VertexID) {\n"
    "    VSOut o;\n"
    "    o.uv = float2((vid << 1) & 2, vid & 2);\n"
    "    o.pos = float4(o.uv * float2(2,-1) + float2(-1,1), 0, 1);\n"
    "    return o;\n"
    "}\n";

static const char g_ps_source[] =
    "Texture2D tex : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "float4 main(float2 uv : TEXCOORD0) : SV_Target {\n"
    "    return tex.Sample(samp, uv);\n"
    "}\n";

struct D3D11Renderer {
    ID3D11Device*           device;
    ID3D11DeviceContext*    context;
    IDXGISwapChain*         swapchain;
    ID3D11RenderTargetView* rtv;
    ID3D11VertexShader*     vs;
    ID3D11PixelShader*      ps;
    ID3D11SamplerState*     sampler;
    ID3D11Texture2D*        tex;
    ID3D11ShaderResourceView* srv;
    int                     tex_w;
    int                     tex_h;
    int                     win_w;
    int                     win_h;
};

D3D11Renderer* d3d11_create(HWND hwnd) {
    D3D11Renderer* r = (D3D11Renderer*)calloc(1, sizeof(D3D11Renderer));

    RECT rc;
    GetClientRect(hwnd, &rc);
    r->win_w = rc.right;
    r->win_h = rc.bottom;

    DXGI_SWAP_CHAIN_DESC scd = {0};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = r->win_w > 0 ? r->win_w : 1280;
    scd.BufferDesc.Height = r->win_h > 0 ? r->win_h : 720;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL feature_level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        NULL, 0, D3D11_SDK_VERSION,
        &scd, &r->swapchain, &r->device, &feature_level, &r->context);
    if (FAILED(hr)) {
        LOG_ERROR("D3D11 create failed: 0x%08lX", hr);
        free(r);
        return NULL;
    }
    LOG_INFO("D3D11: feature level %u", feature_level);

    ID3D11Texture2D* backbuf = NULL;
    r->swapchain->lpVtbl->GetBuffer(r->swapchain, 0, &IID_ID3D11Texture2D, (void**)&backbuf);
    r->device->lpVtbl->CreateRenderTargetView(r->device, (ID3D11Resource*)backbuf, NULL, &r->rtv);
    backbuf->lpVtbl->Release(backbuf);

    ID3DBlob* vs_blob = NULL, *ps_blob = NULL, *err = NULL;

    hr = D3DCompile(g_vs_source, strlen(g_vs_source), NULL, NULL, NULL,
                     "main", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_blob, &err);
    if (FAILED(hr)) {
        LOG_ERROR("VS compile failed");
        if (err) { err->lpVtbl->Release(err); }
        d3d11_destroy(r);
        return NULL;
    }
    r->device->lpVtbl->CreateVertexShader(r->device,
        vs_blob->lpVtbl->GetBufferPointer(vs_blob),
        vs_blob->lpVtbl->GetBufferSize(vs_blob), NULL, &r->vs);
    vs_blob->lpVtbl->Release(vs_blob);

    hr = D3DCompile(g_ps_source, strlen(g_ps_source), NULL, NULL, NULL,
                     "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_blob, &err);
    if (FAILED(hr)) {
        LOG_ERROR("PS compile failed");
        if (err) { err->lpVtbl->Release(err); }
        d3d11_destroy(r);
        return NULL;
    }
    r->device->lpVtbl->CreatePixelShader(r->device,
        ps_blob->lpVtbl->GetBufferPointer(ps_blob),
        ps_blob->lpVtbl->GetBufferSize(ps_blob), NULL, &r->ps);
    ps_blob->lpVtbl->Release(ps_blob);

    D3D11_SAMPLER_DESC sd = {0};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    r->device->lpVtbl->CreateSamplerState(r->device, &sd, &r->sampler);

    LOG_INFO("D3D11: renderer initialized");
    return r;
}

static void ensure_texture(D3D11Renderer* r, int w, int h) {
    if (r->tex && r->tex_w == w && r->tex_h == h) return;

    if (r->tex) { r->tex->lpVtbl->Release(r->tex); r->tex = NULL; }
    if (r->srv) { r->srv->lpVtbl->Release(r->srv); r->srv = NULL; }

    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    r->device->lpVtbl->CreateTexture2D(r->device, &td, NULL, &r->tex);

    r->device->lpVtbl->CreateShaderResourceView(r->device,
        (ID3D11Resource*)r->tex, NULL, &r->srv);

    r->tex_w = w;
    r->tex_h = h;
}

void d3d11_render(D3D11Renderer* r, const uint8_t* rgba, int width, int height, int stride) {
    if (!r || !rgba) return;

    ensure_texture(r, width, height);

    r->context->lpVtbl->UpdateSubresource(r->context,
        (ID3D11Resource*)r->tex, 0, NULL,
        rgba, stride, 0);

    float clear[4] = {0, 0, 0, 1};
    r->context->lpVtbl->ClearRenderTargetView(r->context, r->rtv, clear);
    r->context->lpVtbl->OMSetRenderTargets(r->context, 1, &r->rtv, NULL);

    D3D11_VIEWPORT vp = {0, 0, (float)r->win_w, (float)r->win_h, 0, 1};
    r->context->lpVtbl->RSSetViewports(r->context, 1, &vp);
    r->context->lpVtbl->IASetPrimitiveTopology(r->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r->context->lpVtbl->VSSetShader(r->context, r->vs, NULL, 0);
    r->context->lpVtbl->PSSetShader(r->context, r->ps, NULL, 0);
    r->context->lpVtbl->PSSetShaderResources(r->context, 0, 1, &r->srv);
    r->context->lpVtbl->PSSetSamplers(r->context, 0, 1, &r->sampler);
    r->context->lpVtbl->Draw(r->context, 3, 0);

    r->swapchain->lpVtbl->Present(r->swapchain, 1, 0);
}

void d3d11_clear(D3D11Renderer* r) {
    if (!r) return;
    float clear[4] = {0, 0, 0, 1};
    r->context->lpVtbl->ClearRenderTargetView(r->context, r->rtv, clear);
    r->swapchain->lpVtbl->Present(r->swapchain, 1, 0);
}

void d3d11_resize(D3D11Renderer* r, int w, int h) {
    if (!r || w <= 0 || h <= 0) return;
    r->win_w = w;
    r->win_h = h;

    if (r->rtv) { r->rtv->lpVtbl->Release(r->rtv); r->rtv = NULL; }
    r->swapchain->lpVtbl->ResizeBuffers(r->swapchain, 0, w, h, DXGI_FORMAT_UNKNOWN, 0);

    ID3D11Texture2D* backbuf = NULL;
    r->swapchain->lpVtbl->GetBuffer(r->swapchain, 0, &IID_ID3D11Texture2D, (void**)&backbuf);
    r->device->lpVtbl->CreateRenderTargetView(r->device, (ID3D11Resource*)backbuf, NULL, &r->rtv);
    backbuf->lpVtbl->Release(backbuf);
}

void d3d11_destroy(D3D11Renderer* r) {
    if (!r) return;
    if (r->sampler) r->sampler->lpVtbl->Release(r->sampler);
    if (r->ps) r->ps->lpVtbl->Release(r->ps);
    if (r->vs) r->vs->lpVtbl->Release(r->vs);
    if (r->rtv) r->rtv->lpVtbl->Release(r->rtv);
    if (r->srv) r->srv->lpVtbl->Release(r->srv);
    if (r->tex) r->tex->lpVtbl->Release(r->tex);
    if (r->swapchain) r->swapchain->lpVtbl->Release(r->swapchain);
    if (r->context) r->context->lpVtbl->Release(r->context);
    if (r->device) r->device->lpVtbl->Release(r->device);
    free(r);
}
