#include "SPD3D11Device.hpp"

#include <d3d10.h> // ID3D10Multithread

#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace sp {

std::shared_ptr<D3D11Device> D3D11Device::create(D3D_DRIVER_TYPE type) {
    // BGRA for the swap chain and subtitles, video support for D3D11VA.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    auto result = std::shared_ptr<D3D11Device>(new D3D11Device);
    HRESULT hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &result->device_, &result->featureLevel_, &result->context_);
    if (hr == E_INVALIDARG) // a runtime without 11.1
        hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels + 1, 1, D3D11_SDK_VERSION, &result->device_,
                               &result->featureLevel_, &result->context_);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_VIDEO_SUPPORT)) {
        // WARP has no video decoding; rendering still works.
        flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels + 1, 1, D3D11_SDK_VERSION, &result->device_,
                               &result->featureLevel_, &result->context_);
    }
    if (FAILED(hr)) return nullptr;
    result->warp_ = type == D3D_DRIVER_TYPE_WARP;
    // Decoding and rendering run on different threads; D3D11VA requires the
    // runtime's own serialisation on top of lock().
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(result->context_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    return result;
}

std::shared_ptr<D3D11Device> D3D11Device::shared() {
    static std::shared_ptr<D3D11Device> device = [] {
        auto d = create(D3D_DRIVER_TYPE_HARDWARE);
        if (!d) {
            std::fprintf(stderr, "[Renderer] no hardware Direct3D 11 device; using WARP\n");
            d = create(D3D_DRIVER_TYPE_WARP);
        }
        return d;
    }();
    return device;
}

std::shared_ptr<D3D11Device> D3D11Device::createWarp() { return create(D3D_DRIVER_TYPE_WARP); }

} // namespace sp
