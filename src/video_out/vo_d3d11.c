#define INITGUID
#include "video_out.h"
#include "../util/log.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <stdlib.h>
#include <string.h>
#include <initguid.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static const char g_vs_src[] =
    "struct VS_IN { float2 pos:POSITION; float2 uv:TEXCOORD; };"
    "struct VS_OUT { float4 pos:SV_POSITION; float2 uv:TEXCOORD; };"
    "VS_OUT main(VS_IN v) {"
    "  VS_OUT o;"
    "  o.pos = float4(v.pos, 0, 1);"
    "  o.uv = v.uv;"
    "  return o;"
    "}";

static const char g_ps_nv12_src[] =
    "Texture2D texY : register(t0);"
    "Texture2D texUV : register(t1);"
    "SamplerState sam : register(s0);"
    "float4 main(float4 pos:SV_POSITION, float2 uv:TEXCOORD) : SV_TARGET {"
    "  float Y = texY.Sample(sam, uv).r - 0.0625;"
    "  float2 cr_cb = texUV.Sample(sam, uv).rg - 0.5;"
    "  Y *= 1.164;"
    "  return float4("
    "    Y + 1.596 * cr_cb.r,"
    "    Y - 0.391 * cr_cb.g - 0.813 * cr_cb.r,"
    "    Y + 2.018 * cr_cb.g,"
    "    1);"
    "}";

static const char g_ps_rgb_src[] =
    "Texture2D tex : register(t0);"
    "SamplerState sam : register(s0);"
    "float4 main(float4 pos:SV_POSITION, float2 uv:TEXCOORD) : SV_TARGET {"
    "  return tex.Sample(sam, uv);"
    "}";

typedef struct {
    float pos[2];
    float uv[2];
} Vertex;

static const Vertex g_quad[] = {
    {{-1,  1}, {0, 0}},
    {{ 1,  1}, {1, 0}},
    {{-1, -1}, {0, 1}},
    {{ 1, -1}, {1, 1}},
};

struct VideoOut {
    HWND                    hwnd;
    int                     win_w, win_h;

    ID3D11Device*           device;
    ID3D11DeviceContext*     ctx;
    IDXGISwapChain*         swap;
    ID3D11RenderTargetView*  rtv;

    ID3D11VertexShader*     vs;
    ID3D11PixelShader*      ps_nv12;
    ID3D11PixelShader*      ps_rgb;
    ID3D11InputLayout*      layout;
    ID3D11Buffer*           vbuf;
    ID3D11SamplerState*     sam;

    ID3D11Texture2D*        tex_y;
    ID3D11ShaderResourceView* srv_y;
    ID3D11Texture2D*        tex_uv;
    ID3D11ShaderResourceView* srv_uv;

    ID3D11Texture2D*        tex_rgb;
    ID3D11ShaderResourceView* srv_rgb;

    ID3D11Buffer*           cb;
    int                     tex_w;
    int                     tex_h;
};

static HRESULT compile_shader(const char* src, const char* target,
                              ID3DBlob** blob) {
    ID3DBlob* err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), NULL, NULL, NULL,
                            "main", target, 0, 0, blob, &err);
    if (err) { err->lpVtbl->Release(err); }
    return hr;
}

static void release_d3d11(VideoOut* vo) {
    #define REL(x) if (x) { (x)->lpVtbl->Release(x); (x) = NULL; }
    REL(vo->srv_y);
    REL(vo->srv_uv);
    REL(vo->tex_y);
    REL(vo->tex_uv);
    REL(vo->srv_rgb);
    REL(vo->tex_rgb);
    REL(vo->sam);
    REL(vo->cb);
    REL(vo->vbuf);
    REL(vo->layout);
    REL(vo->ps_rgb);
    REL(vo->ps_nv12);
    REL(vo->vs);
    REL(vo->rtv);
    REL(vo->swap);
    REL(vo->ctx);
    REL(vo->device);
    #undef REL
}

VideoOut* vo_create(HWND hwnd, int w, int h) {
    VideoOut* vo = (VideoOut*)calloc(1, sizeof(VideoOut));
    if (!vo) return NULL;
    vo->hwnd = hwnd;
    vo->win_w = w;
    vo->win_h = h;

    HRESULT hr;

    DXGI_SWAP_CHAIN_DESC scd = {0};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = w;
    scd.BufferDesc.Height = h;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 0;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got;
    hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        levels, 1, D3D11_SDK_VERSION,
        &scd, &vo->swap, &vo->device, &got, &vo->ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
            levels, 1, D3D11_SDK_VERSION,
            &scd, &vo->swap, &vo->device, &got, &vo->ctx);
    }
    if (FAILED(hr)) { LOG_ERROR("D3D11 create failed: 0x%08lX", hr); free(vo); return NULL; }

    ID3D11Texture2D* backbuf = NULL;
    hr = vo->swap->lpVtbl->GetBuffer(vo->swap, 0, &IID_ID3D11Texture2D, (void**)&backbuf);
    if (FAILED(hr)) { release_d3d11(vo); free(vo); return NULL; }
    hr = vo->device->lpVtbl->CreateRenderTargetView(vo->device, (ID3D11Resource*)backbuf, NULL, &vo->rtv);
    backbuf->lpVtbl->Release(backbuf);
    if (FAILED(hr)) { release_d3d11(vo); free(vo); return NULL; }

    ID3DBlob* vsblob = NULL;
    hr = compile_shader(g_vs_src, "vs_5_0", &vsblob);
    if (FAILED(hr)) { LOG_ERROR("VS compile failed: 0x%08lX", hr); release_d3d11(vo); free(vo); return NULL; }
    hr = vo->device->lpVtbl->CreateVertexShader(vo->device,
        vsblob->lpVtbl->GetBufferPointer(vsblob),
        vsblob->lpVtbl->GetBufferSize(vsblob), NULL, &vo->vs);

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    vo->device->lpVtbl->CreateInputLayout(vo->device, ied, 2,
        vsblob->lpVtbl->GetBufferPointer(vsblob),
        vsblob->lpVtbl->GetBufferSize(vsblob), &vo->layout);
    vsblob->lpVtbl->Release(vsblob);
    if (FAILED(hr)) { release_d3d11(vo); free(vo); return NULL; }

    ID3DBlob* psblob = NULL;
    hr = compile_shader(g_ps_nv12_src, "ps_5_0", &psblob);
    if (SUCCEEDED(hr)) {
        vo->device->lpVtbl->CreatePixelShader(vo->device,
            psblob->lpVtbl->GetBufferPointer(psblob),
            psblob->lpVtbl->GetBufferSize(psblob), NULL, &vo->ps_nv12);
        psblob->lpVtbl->Release(psblob);
    }

    psblob = NULL;
    hr = compile_shader(g_ps_rgb_src, "ps_5_0", &psblob);
    if (SUCCEEDED(hr)) {
        vo->device->lpVtbl->CreatePixelShader(vo->device,
            psblob->lpVtbl->GetBufferPointer(psblob),
            psblob->lpVtbl->GetBufferSize(psblob), NULL, &vo->ps_rgb);
        psblob->lpVtbl->Release(psblob);
    }

    D3D11_BUFFER_DESC bd = {sizeof(g_quad), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {g_quad, 0, 0};
    vo->device->lpVtbl->CreateBuffer(vo->device, &bd, &sd, &vo->vbuf);

    D3D11_SAMPLER_DESC sd_sam = {D3D11_FILTER_MIN_MAG_MIP_LINEAR,
        D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
        0, 0, D3D11_COMPARISON_NEVER, 0, D3D11_FLOAT32_MAX};
    vo->device->lpVtbl->CreateSamplerState(vo->device, &sd_sam, &vo->sam);

    D3D11_BUFFER_DESC cbd = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    vo->device->lpVtbl->CreateBuffer(vo->device, &cbd, NULL, &vo->cb);

    LOG_INFO("D3D11 feature level %d.%d",
             (got >> 12) & 0xF, (got >> 8) & 0xF);
    return vo;
}

static void ensure_textures(VideoOut* vo, int w, int h, int is_nv12) {
    if (vo->tex_w == w && vo->tex_h == h && (is_nv12 ? vo->tex_y : vo->tex_rgb)) return;

    if (vo->srv_y) { vo->srv_y->lpVtbl->Release(vo->srv_y); vo->srv_y = NULL; }
    if (vo->tex_y) { vo->tex_y->lpVtbl->Release(vo->tex_y); vo->tex_y = NULL; }
    if (vo->srv_uv) { vo->srv_uv->lpVtbl->Release(vo->srv_uv); vo->srv_uv = NULL; }
    if (vo->tex_uv) { vo->tex_uv->lpVtbl->Release(vo->tex_uv); vo->tex_uv = NULL; }
    if (vo->srv_rgb) { vo->srv_rgb->lpVtbl->Release(vo->srv_rgb); vo->srv_rgb = NULL; }
    if (vo->tex_rgb) { vo->tex_rgb->lpVtbl->Release(vo->tex_rgb); vo->tex_rgb = NULL; }

    if (is_nv12) {
        D3D11_TEXTURE2D_DESC td = {0};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr1 = vo->device->lpVtbl->CreateTexture2D(vo->device, &td, NULL, &vo->tex_y);
        if (SUCCEEDED(hr1))
            vo->device->lpVtbl->CreateShaderResourceView(vo->device, (ID3D11Resource*)vo->tex_y, NULL, &vo->srv_y);

        td.Width = w / 2;
        td.Height = h / 2;
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        hr1 = vo->device->lpVtbl->CreateTexture2D(vo->device, &td, NULL, &vo->tex_uv);
        if (SUCCEEDED(hr1))
            vo->device->lpVtbl->CreateShaderResourceView(vo->device, (ID3D11Resource*)vo->tex_uv, NULL, &vo->srv_uv);
    } else {
        D3D11_TEXTURE2D_DESC td = {0};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr1 = vo->device->lpVtbl->CreateTexture2D(vo->device, &td, NULL, &vo->tex_rgb);
        if (FAILED(hr1)) {
            LOG_ERROR("CreateTexture2D RGB failed: 0x%08lX (%dx%d)", hr1, w, h);
        } else {
            HRESULT hr2 = vo->device->lpVtbl->CreateShaderResourceView(vo->device, (ID3D11Resource*)vo->tex_rgb, NULL, &vo->srv_rgb);
            if (FAILED(hr2)) LOG_WARN("CreateSRV RGB failed: 0x%08lX", hr2);
        }
    }
    vo->tex_w = w;
    vo->tex_h = h;
}

static void upload_nv12(VideoOut* vo, const uint8_t* data, int w, int h) {
    D3D11_MAPPED_SUBRESOURCE map;

    /* Y plane: w * h bytes */
    vo->ctx->lpVtbl->Map(vo->ctx, (ID3D11Resource*)vo->tex_y, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    for (int row = 0; row < h; row++) {
        memcpy((uint8_t*)map.pData + row * map.RowPitch, data + row * w, w);
    }
    vo->ctx->lpVtbl->Unmap(vo->ctx, (ID3D11Resource*)vo->tex_y, 0);

    /* UV plane: w * h/2 bytes (interleaved) */
    vo->ctx->lpVtbl->Map(vo->ctx, (ID3D11Resource*)vo->tex_uv, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    const uint8_t* uv = data + w * h;
    for (int row = 0; row < h / 2; row++) {
        memcpy((uint8_t*)map.pData + row * map.RowPitch, uv + row * w, w);
    }
    vo->ctx->lpVtbl->Unmap(vo->ctx, (ID3D11Resource*)vo->tex_uv, 0);
}

static void upload_rgb32(VideoOut* vo, const uint8_t* data, int w, int h) {
    if (!vo->tex_rgb) { LOG_WARN("tex_rgb is NULL"); return; }
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr = vo->ctx->lpVtbl->Map(vo->ctx, (ID3D11Resource*)vo->tex_rgb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    if (FAILED(hr)) { LOG_WARN("Map tex_rgb failed: 0x%08lX", hr); return; }
    for (int row = 0; row < h; row++) {
        memcpy((uint8_t*)map.pData + row * map.RowPitch, data + row * w * 4, w * 4);
    }
    vo->ctx->lpVtbl->Unmap(vo->ctx, (ID3D11Resource*)vo->tex_rgb, 0);
}

void vo_render(VideoOut* vo, const uint8_t* data, int src_w, int src_h, int data_size) {
    if (!vo || !vo->swap || !vo->device || !data || src_w <= 0 || src_h <= 0) return;
    if (!vo->rtv) return;

    /* Detect format by data size: NV12 = w*h*1.5, RGB32 = w*h*4 */
    int nv12_size = src_w * src_h * 3 / 2;
    int is_nv12 = (abs(data_size - nv12_size) < 1000);

    ensure_textures(vo, src_w, src_h, is_nv12);

    if (is_nv12) {
        upload_nv12(vo, data, src_w, src_h);
    } else {
        upload_rgb32(vo, data, src_w, src_h);
    }

    float clearColor[4] = {0, 0, 0, 1};
    vo->ctx->lpVtbl->OMSetRenderTargets(vo->ctx, 1, &vo->rtv, NULL);

    vo->ctx->lpVtbl->ClearRenderTargetView(vo->ctx, vo->rtv, clearColor);

    D3D11_VIEWPORT vp = {0, 0, (float)vo->win_w, (float)vo->win_h, 0, 1};
    vo->ctx->lpVtbl->RSSetViewports(vo->ctx, 1, &vp);
    vo->ctx->lpVtbl->IASetPrimitiveTopology(vo->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    vo->ctx->lpVtbl->IASetInputLayout(vo->ctx, vo->layout);

    UINT stride = sizeof(Vertex), offset = 0;
    vo->ctx->lpVtbl->IASetVertexBuffers(vo->ctx, 0, 1, &vo->vbuf, &stride, &offset);
    vo->ctx->lpVtbl->VSSetShader(vo->ctx, vo->vs, NULL, 0);
    vo->ctx->lpVtbl->PSSetSamplers(vo->ctx, 0, 1, &vo->sam);

    /* Compute letterbox in clip space */
    float sx = (float)vo->win_w / src_w;
    float sy = (float)vo->win_h / src_h;
    float s = sx < sy ? sx : sy;
    float hw = s * src_w / vo->win_w;
    float hh = s * src_h / vo->win_h;

    Vertex box[4] = {
        {{-hw,  hh}, {0, 0}},
        {{ hw,  hh}, {1, 0}},
        {{-hw, -hh}, {0, 1}},
        {{ hw, -hh}, {1, 1}},
    };
    D3D11_MAPPED_SUBRESOURCE vb_map;
    vo->ctx->lpVtbl->Map(vo->ctx, (ID3D11Resource*)vo->vbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &vb_map);
    memcpy(vb_map.pData, box, sizeof(box));
    vo->ctx->lpVtbl->Unmap(vo->ctx, (ID3D11Resource*)vo->vbuf, 0);

    if (is_nv12 && vo->ps_nv12) {
        vo->ctx->lpVtbl->PSSetShader(vo->ctx, vo->ps_nv12, NULL, 0);
        vo->ctx->lpVtbl->PSSetShaderResources(vo->ctx, 0, 1, &vo->srv_y);
        ID3D11ShaderResourceView* uv_srvs[1] = {vo->srv_uv};
        vo->ctx->lpVtbl->PSSetShaderResources(vo->ctx, 1, 1, uv_srvs);
    } else if (vo->ps_rgb && vo->srv_rgb) {
        vo->ctx->lpVtbl->PSSetShader(vo->ctx, vo->ps_rgb, NULL, 0);
        ID3D11ShaderResourceView* rgb_srvs[1] = {vo->srv_rgb};
        vo->ctx->lpVtbl->PSSetShaderResources(vo->ctx, 0, 1, rgb_srvs);
    } else {
        LOG_WARN("No shader/resource: ps_rgb=%p srv_rgb=%p tex_rgb=%p",
                 (void*)vo->ps_rgb, (void*)vo->srv_rgb, (void*)vo->tex_rgb);
        return;
    }

    vo->ctx->lpVtbl->Draw(vo->ctx, 4, 0);

    /* Present with no vsync to avoid blocking */
    HRESULT hr = vo->swap->lpVtbl->Present(vo->swap, 0, 0);
    if (hr == DXGI_STATUS_OCCLUDED) {
        /* Window is not visible, sleep to avoid burning CPU */
        Sleep(10);
    } else if (FAILED(hr)) {
        LOG_WARN("Present failed: 0x%08lX", hr);
    }
}

void vo_resize(VideoOut* vo, int w, int h) {
    if (!vo || !vo->swap) return;
    if (vo->win_w == w && vo->win_h == h) return;
    vo->win_w = w;
    vo->win_h = h;

    if (vo->rtv) { vo->rtv->lpVtbl->Release(vo->rtv); vo->rtv = NULL; }
    vo->swap->lpVtbl->ResizeBuffers(vo->swap, 0, w, h, DXGI_FORMAT_UNKNOWN, 0);

    ID3D11Texture2D* backbuf = NULL;
    if (SUCCEEDED(vo->swap->lpVtbl->GetBuffer(vo->swap, 0, &IID_ID3D11Texture2D, (void**)&backbuf))) {
        vo->device->lpVtbl->CreateRenderTargetView(vo->device, (ID3D11Resource*)backbuf, NULL, &vo->rtv);
        backbuf->lpVtbl->Release(backbuf);
    }
}

void vo_destroy(VideoOut* vo) {
    if (!vo) return;
    release_d3d11(vo);
    free(vo);
}
