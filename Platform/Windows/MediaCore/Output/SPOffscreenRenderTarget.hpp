// A render target that keeps its image in a texture for readback, for tests
// and render dumps: the same shader and state machine as on screen, without
// a window.
#pragma once

#include "SPRenderTarget.hpp"

#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace sp {

class OffscreenRenderTarget final : public RenderTarget {
public:
    bool configure(ID3D11Device *device, int mode, int width, int height) override;
    ID3D11RenderTargetView *acquire() override;
    void present(ID3D11DeviceContext *context) override;

    // While unavailable, acquire returns null, like an occluded window.
    void setAvailable(bool available) { available_.store(available); }

    int mode() const { return mode_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int presentCount() const { return presents_.load(); }

    // The last presented image as RGBA floats, row-major from the top. Call
    // with the device lock held and the renderer idle.
    std::vector<float> readPixels(ID3D11Device *device, ID3D11DeviceContext *context) const;

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    int mode_ = -1;
    int width_ = 0;
    int height_ = 0;
    std::atomic<bool> available_{true};
    std::atomic<int> presents_{0};
};

} // namespace sp
