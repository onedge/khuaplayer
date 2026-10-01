// Where the D3D11 renderer draws: the Windows counterpart of the Mac
// renderer's CAMetalLayer. A swap chain presents to the window; an offscreen
// target keeps the image for readback (tests, render dumps).
//
// Only the renderer's submit thread calls these, with the device lock held
// where the context is used, so implementations need no locking of their own.
#pragma once

#include <d3d11.h>

namespace sp {

// The renderer's output modes, as on the Mac.
enum RenderOutputMode : int {
    // B8G8R8A8_UNORM, composed as sRGB.
    kRenderOutputSDR = 0,
    // R16G16B16A16_FLOAT, composed as scRGB (linear BT.709, 1.0 = 80 nits):
    // the Mac's EDR layer.
    kRenderOutputEDR = 1,
};

class RenderTarget {
public:
    virtual ~RenderTarget() = default;

    // Sets the pixel format for `mode` and the size in pixels. Returns false
    // when the target cannot take it (the renderer retries later).
    virtual bool configure(ID3D11Device *device, int mode, int width, int height) = 0;

    // The next image to draw into, or null when none is available now (an
    // occluded window, a busy swap chain): the renderer retries, as it does
    // when the Mac layer has no drawable.
    virtual ID3D11RenderTargetView *acquire() = 0;

    // Shows what was drawn since acquire.
    virtual void present(ID3D11DeviceContext *context) = 0;
};

} // namespace sp
