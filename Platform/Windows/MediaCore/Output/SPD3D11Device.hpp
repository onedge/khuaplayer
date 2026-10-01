// The Direct3D 11 device shared by decoding and rendering, like the Mac
// player's process-wide Metal device: D3D11VA decodes into textures the
// renderer samples directly, so both must use one device.
//
// The immediate context is shared too. Everything that uses it takes lock()
// (a recursive mutex); FFmpeg's D3D11VA device context is given the same lock,
// so decoding and rendering never interleave on the context.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <mutex>

namespace sp {

class D3D11Device {
public:
    // The process-wide device on the default adapter, created on first use;
    // falls back to WARP where there is no hardware device (CI). Null when
    // neither can be created.
    static std::shared_ptr<D3D11Device> shared();

    // A device of its own on WARP, for tests that must not depend on a GPU.
    static std::shared_ptr<D3D11Device> createWarp();

    ID3D11Device *device() const { return device_.Get(); }
    ID3D11DeviceContext *context() const { return context_.Get(); }
    bool isWarp() const { return warp_; }
    D3D_FEATURE_LEVEL featureLevel() const { return featureLevel_; }

    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }
    std::recursive_mutex &mutex() { return mutex_; }

private:
    static std::shared_ptr<D3D11Device> create(D3D_DRIVER_TYPE type);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    D3D_FEATURE_LEVEL featureLevel_ = D3D_FEATURE_LEVEL_11_0;
    bool warp_ = false;
    std::recursive_mutex mutex_;
};

} // namespace sp
