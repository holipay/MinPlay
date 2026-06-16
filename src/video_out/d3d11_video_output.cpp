#include "d3d11_video_output.h"
#include <cstdlib>
#include <cstring>
#include <cmath>

HRESULT D3D11VideoOutput::CompileShader(const char* src, const char* target,
                                         ID3DBlob** blob) {
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            "main", target, 0, 0, blob, &err);
    if (err) { err->Release(); }
    return hr;
}

static constexpr D3D11Vertex s_quad[4] = {
    {{-1,  1}, {0, 0}},
    {{ 1,  1}, {1, 0}},
    {{-1, -1}, {0, 1}},
    {{ 1, -1}, {1, 1}},
};

void D3D11VideoOutput::ReleaseD3D11() {
    #define REL(x) if (x) { (x)->Release(); (x) = nullptr; }
    REL(srv_y_); REL(srv_uv_); REL(tex_y_); REL(tex_uv_);
    REL(srv_rgb_); REL(tex_rgb_); REL(sam_);
    REL(vbuf_); REL(layout_); REL(ps_rgb_);
    REL(ps_nv12_); REL(vs_); REL(rtv_);
    REL(swap_); REL(ctx_); REL(device_);
    #undef REL
}

bool D3D11VideoOutput::Initialize(HWND hwnd, int w, int h) {
    hwnd_ = hwnd;
    win_w_ = w;
    win_h_ = h;

    DXGI_SWAP_CHAIN_DESC scd = {};
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
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 1, D3D11_SDK_VERSION,
        &scd, &swap_, &device_, &got, &ctx_);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 1, D3D11_SDK_VERSION,
            &scd, &swap_, &device_, &got, &ctx_);
    }
    if (FAILED(hr)) {
        LOG_ERROR("D3D11 create failed: 0x%08lX", hr);
        return false;
    }

    ID3D11Texture2D* backbuf = nullptr;
    hr = swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuf);
    if (FAILED(hr)) { ReleaseD3D11(); return false; }
    hr = device_->CreateRenderTargetView(backbuf, nullptr, &rtv_);
    backbuf->Release();
    if (FAILED(hr)) { ReleaseD3D11(); return false; }

    ID3DBlob* vsblob = nullptr;
    hr = CompileShader(VS_SRC, "vs_5_0", &vsblob);
    if (FAILED(hr)) { LOG_ERROR("VS compile failed: 0x%08lX", hr); ReleaseD3D11(); return false; }
    hr = device_->CreateVertexShader(
        vsblob->GetBufferPointer(), vsblob->GetBufferSize(), nullptr, &vs_);
    if (FAILED(hr)) { vsblob->Release(); ReleaseD3D11(); return false; }

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device_->CreateInputLayout(ied, 2,
        vsblob->GetBufferPointer(), vsblob->GetBufferSize(), &layout_);
    vsblob->Release();
    if (FAILED(hr)) { ReleaseD3D11(); return false; }

    ID3DBlob* psblob = nullptr;
    hr = CompileShader(PS_NV12_SRC, "ps_5_0", &psblob);
    if (SUCCEEDED(hr)) {
        device_->CreatePixelShader(
            psblob->GetBufferPointer(), psblob->GetBufferSize(), nullptr, &ps_nv12_);
    }
    if (psblob) psblob->Release();

    psblob = nullptr;
    hr = CompileShader(PS_RGB_SRC, "ps_5_0", &psblob);
    if (SUCCEEDED(hr)) {
        device_->CreatePixelShader(
            psblob->GetBufferPointer(), psblob->GetBufferSize(), nullptr, &ps_rgb_);
    }
    if (psblob) psblob->Release();

    D3D11_BUFFER_DESC bd = {sizeof(s_quad), D3D11_USAGE_DYNAMIC,
                            D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {s_quad, 0, 0};
    if (FAILED(device_->CreateBuffer(&bd, &sd, &vbuf_)) || !vbuf_) {
        LOG_ERROR("CreateBuffer vertex failed");
        return false;
    }

    D3D11_SAMPLER_DESC sd_sam = {D3D11_FILTER_MIN_MAG_MIP_LINEAR,
        D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_CLAMP,
        0, 0, D3D11_COMPARISON_NEVER, 0, D3D11_FLOAT32_MAX};
    if (FAILED(device_->CreateSamplerState(&sd_sam, &sam_)) || !sam_) {
        LOG_ERROR("CreateSamplerState failed");
        vbuf_->Release(); vbuf_ = nullptr;
        return false;
    }

    LOG_INFO("D3D11 feature level %d.%d",
             (got >> 12) & 0xF, (got >> 8) & 0xF);
    return true;
}

void D3D11VideoOutput::EnsureTextures(int w, int h, int is_nv12) {
    if (tex_w_ == w && tex_h_ == h && tex_is_nv12_ == is_nv12 &&
        (is_nv12 ? tex_y_ : tex_rgb_)) return;

    #define REL(x) if (x) { (x)->Release(); (x) = nullptr; }
    REL(srv_y_); REL(tex_y_); REL(srv_uv_); REL(tex_uv_);
    REL(srv_rgb_); REL(tex_rgb_);
    #undef REL

    if (is_nv12) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr1 = device_->CreateTexture2D(&td, nullptr, &tex_y_);
        if (SUCCEEDED(hr1))
            device_->CreateShaderResourceView(tex_y_, nullptr, &srv_y_);

        td.Width = w / 2; td.Height = h / 2;
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        hr1 = device_->CreateTexture2D(&td, nullptr, &tex_uv_);
        if (SUCCEEDED(hr1))
            device_->CreateShaderResourceView(tex_uv_, nullptr, &srv_uv_);

        if (tex_y_ && tex_uv_ && srv_y_ && srv_uv_) {
            tex_w_ = w; tex_h_ = h; tex_is_nv12_ = is_nv12;
        }
    } else {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr1 = device_->CreateTexture2D(&td, nullptr, &tex_rgb_);
        if (FAILED(hr1)) {
            LOG_ERROR("CreateTexture2D RGB failed: 0x%08lX (%dx%d)", hr1, w, h);
        } else {
            HRESULT hr2 = device_->CreateShaderResourceView(tex_rgb_, nullptr, &srv_rgb_);
            if (FAILED(hr2)) LOG_WARN("CreateSRV RGB failed: 0x%08lX", hr2);
        }
        if (tex_rgb_ && srv_rgb_) {
            tex_w_ = w; tex_h_ = h; tex_is_nv12_ = is_nv12;
        }
    }
}

void D3D11VideoOutput::UploadNV12(const uint8_t* data, int w, int h, int stride) {
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr;

    if (!tex_y_) { LOG_WARN("UploadNV12: tex_y_ is null"); return; }
    hr = ctx_->Map(tex_y_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    if (FAILED(hr)) { LOG_WARN("Map tex_y_ failed: 0x%08lX", hr); return; }
    if (map.RowPitch == (UINT)stride) {
        memcpy(map.pData, data, (SIZE_T)stride * h);
    } else {
        for (int row = 0; row < h; row++)
            memcpy((uint8_t*)map.pData + row * map.RowPitch, data + row * stride, w);
    }
    ctx_->Unmap(tex_y_, 0);

    if (!tex_uv_) { LOG_WARN("UploadNV12: tex_uv_ is null"); return; }
    hr = ctx_->Map(tex_uv_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    if (FAILED(hr)) { LOG_WARN("Map tex_uv_ failed: 0x%08lX", hr); return; }
    const uint8_t* uv = data + stride * h;
    if (map.RowPitch == (UINT)stride) {
        memcpy(map.pData, uv, (SIZE_T)stride * (h / 2));
    } else {
        for (int row = 0; row < h / 2; row++)
            memcpy((uint8_t*)map.pData + row * map.RowPitch, uv + row * stride, w);
    }
    ctx_->Unmap(tex_uv_, 0);
}

void D3D11VideoOutput::UploadRGB32(const uint8_t* data, int w, int h) {
    if (!tex_rgb_) { LOG_WARN("tex_rgb is NULL"); return; }
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr = ctx_->Map(tex_rgb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    if (FAILED(hr)) { LOG_WARN("Map tex_rgb failed: 0x%08lX", hr); return; }
    int stride = w * 4;
    if (map.RowPitch == (UINT)stride) {
        memcpy(map.pData, data, (SIZE_T)stride * h);
    } else {
        for (int row = 0; row < h; row++)
            memcpy((uint8_t*)map.pData + row * map.RowPitch, data + row * stride, stride);
    }
    ctx_->Unmap(tex_rgb_, 0);
}

void D3D11VideoOutput::Render(const uint8_t* data, int src_w, int src_h, int stride, PixelFormat fmt) {
    if (!swap_ || !device_ || !data || src_w <= 0 || src_h <= 0) return;
    if (!rtv_) return;
    if (IsIconic(hwnd_)) return;

    int is_nv12 = (fmt == PixelFormat::NV12) ? 1 : 0;
    int src_stride = (stride > 0) ? stride : src_w;

    EnsureTextures(src_w, src_h, is_nv12);

    if (is_nv12)
        UploadNV12(data, src_w, src_h, src_stride);
    else
        UploadRGB32(data, src_w, src_h);

    float clearColor[4] = {0, 0, 0, 1};
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->ClearRenderTargetView(rtv_, clearColor);

    D3D11_VIEWPORT vp = {0, 0, (float)win_w_, (float)win_h_, 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->IASetInputLayout(layout_);

    UINT vb_stride = sizeof(D3D11Vertex), vb_offset = 0;
    ctx_->IASetVertexBuffers(0, 1, &vbuf_, &vb_stride, &vb_offset);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetSamplers(0, 1, &sam_);

    float sx = (float)win_w_ / src_w;
    float sy = (float)win_h_ / src_h;
    float s = sx < sy ? sx : sy;
    float hw = s * src_w / win_w_;
    float hh = s * src_h / win_h_;

    D3D11Vertex box[4] = {
        {{-hw,  hh}, {0, 0}},
        {{ hw,  hh}, {1, 0}},
        {{-hw, -hh}, {0, 1}},
        {{ hw, -hh}, {1, 1}},
    };
    D3D11_MAPPED_SUBRESOURCE vb_map;
    if (FAILED(ctx_->Map(vbuf_, 0, D3D11_MAP_WRITE_DISCARD, 0, &vb_map))) {
        LOG_WARN("Map vertex buffer failed");
        return;
    }
    memcpy(vb_map.pData, box, sizeof(box));
    ctx_->Unmap(vbuf_, 0);

    if (is_nv12 && ps_nv12_) {
        ctx_->PSSetShader(ps_nv12_, nullptr, 0);
        ctx_->PSSetShaderResources(0, 1, &srv_y_);
        ID3D11ShaderResourceView* uv_srvs[1] = {srv_uv_};
        ctx_->PSSetShaderResources(1, 1, uv_srvs);
    } else if (ps_rgb_ && srv_rgb_) {
        ctx_->PSSetShader(ps_rgb_, nullptr, 0);
        ctx_->PSSetShaderResources(0, 1, &srv_rgb_);
    } else {
        LOG_WARN("No shader/resource");
        return;
    }

    ctx_->Draw(4, 0);

    HRESULT hr = swap_->Present(0, 0);
    if (hr == DXGI_STATUS_OCCLUDED) {
        // Window is occluded — skip render, don't sleep (blocks message loop)
        return;
    } else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        LOG_WARN("D3D11 device lost (0x%08lX) — releasing resources", hr);
        ReleaseD3D11();
        return;
    } else if (FAILED(hr)) {
        LOG_WARN("Present failed: 0x%08lX", hr);
    }
}

void D3D11VideoOutput::Resize(int w, int h) {
    if (!swap_) return;
    if (win_w_ == w && win_h_ == h) return;

    HRESULT hr = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        LOG_WARN("D3D11 device lost during Resize — releasing resources");
        ReleaseD3D11();
        return;
    }
    if (FAILED(hr)) {
        LOG_WARN("ResizeBuffers failed: 0x%08lX", hr);
        return;
    }
    win_w_ = w;
    win_h_ = h;

    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    ID3D11Texture2D* backbuf = nullptr;
    if (SUCCEEDED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuf))) {
        if (FAILED(device_->CreateRenderTargetView(backbuf, nullptr, &rtv_)))
            LOG_WARN("CreateRenderTargetView failed after ResizeBuffers");
        backbuf->Release();
    } else {
        LOG_WARN("GetBuffer failed after ResizeBuffers");
    }
}

D3D11VideoOutput::~D3D11VideoOutput() {
    ReleaseD3D11();
}
