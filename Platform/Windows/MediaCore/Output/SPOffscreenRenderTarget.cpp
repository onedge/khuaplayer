#include "SPOffscreenRenderTarget.hpp"

#include <DirectXPackedVector.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace sp {

bool OffscreenRenderTarget::configure(ID3D11Device *device, int mode, int width, int height) {
    if (texture_ && mode == mode_ && width == width_ && height == height_) return true;
    rtv_.Reset();
    texture_.Reset();
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = mode == kRenderOutputEDR ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture_)) ||
        FAILED(device->CreateRenderTargetView(texture_.Get(), nullptr, &rtv_))) {
        texture_.Reset();
        rtv_.Reset();
        return false;
    }
    mode_ = mode;
    width_ = width;
    height_ = height;
    return true;
}

ID3D11RenderTargetView *OffscreenRenderTarget::acquire() { return available_.load() ? rtv_.Get() : nullptr; }

void OffscreenRenderTarget::present(ID3D11DeviceContext *context) {
    context->Flush();
    presents_.fetch_add(1);
}

std::vector<float> OffscreenRenderTarget::readPixels(ID3D11Device *device, ID3D11DeviceContext *context) const {
    std::vector<float> pixels;
    if (!texture_) return pixels;
    D3D11_TEXTURE2D_DESC desc;
    texture_->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return pixels;
    context->CopyResource(staging.Get(), texture_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return pixels;
    pixels.resize((size_t)width_ * height_ * 4);
    for (int y = 0; y < height_; y++) {
        const auto *row = static_cast<const uint8_t *>(mapped.pData) + (size_t)y * mapped.RowPitch;
        float *out = pixels.data() + (size_t)y * width_ * 4;
        for (int x = 0; x < width_; x++) {
            if (mode_ == kRenderOutputEDR) {
                const auto *half = reinterpret_cast<const DirectX::PackedVector::HALF *>(row) + x * 4;
                for (int c = 0; c < 4; c++) out[x * 4 + c] = DirectX::PackedVector::XMConvertHalfToFloat(half[c]);
            } else {
                const uint8_t *bgra = row + x * 4;
                out[x * 4 + 0] = bgra[2] / 255.0f;
                out[x * 4 + 1] = bgra[1] / 255.0f;
                out[x * 4 + 2] = bgra[0] / 255.0f;
                out[x * 4 + 3] = bgra[3] / 255.0f;
            }
        }
    }
    context->Unmap(staging.Get(), 0);
    return pixels;
}

} // namespace sp
