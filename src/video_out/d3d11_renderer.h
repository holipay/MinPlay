#ifndef D3D11_RENDERER_H
#define D3D11_RENDERER_H

#include <windows.h>
#include <stdint.h>

typedef struct D3D11Renderer D3D11Renderer;

D3D11Renderer* d3d11_create(HWND hwnd);
void           d3d11_render(D3D11Renderer* r, const uint8_t* rgba,
                             int width, int height, int stride);
void           d3d11_clear(D3D11Renderer* r);
void           d3d11_resize(D3D11Renderer* r, int w, int h);
void           d3d11_destroy(D3D11Renderer* r);

#endif
