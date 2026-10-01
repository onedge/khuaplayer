// The Mac player's SPMetalRenderer on Direct3D 11: samples decoded frames
// (D3D11VA texture slices without a copy, or uploaded software planes) and
// applies colour conversion, transfer functions, tone mapping and fit with
// the HLSL port of Video.metal.
//
// The contract is the Mac renderer's (SPMetalRenderer.h), with these
// differences:
//   - What the Mac does on the main thread (layer changes) happens on the
//     renderer's submit thread, which owns the render target. The completion
//     of synchronizeOutputMode therefore runs inline or on that thread, and
//     with false if the renderer is destroyed first. It must not block on
//     a thread that may be destroying the renderer, which joins that thread.
//   - Output mode 1 is an scRGB FP16 target instead of an extended Display P3
//     layer; setSdrWhiteLevelNits gives the scale (the Mac's 1.0 is SDR white).
//   - SDR output is converted to sRGB in the shader, since Windows does not
//     colour-manage a swap chain by its source tags.
//   - Not ported (macOS-only or outside v1): the compare split used by
//     motion interpolation, the window-drag depth effect, specialised
//     pipelines, and the SP_RENDERDUMP hook (OffscreenRenderTarget reads back).
#pragma once

#include "Player/SPVideoFrame.hpp"
#include "SPD3D11Device.hpp"
#include "SPRenderTarget.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sp {

struct RenderRect {
    float x = 0, y = 0, width = 0, height = 0;
};

class D3D11Renderer {
public:
    // Playback-instance identifier used by the logs.
    D3D11Renderer(std::shared_ptr<D3D11Device> device, std::shared_ptr<RenderTarget> target, unsigned logId);
    ~D3D11Renderer();
    D3D11Renderer(const D3D11Renderer &) = delete;
    D3D11Renderer &operator=(const D3D11Renderer &) = delete;

    unsigned logId() const;
    const std::shared_ptr<D3D11Device> &device() const;

    // Accepts a frame for asynchronous submission. true means accepted, not
    // presented; compare committedFrameCount for actual submission.
    bool renderFrame(VideoFrameRef frame);
    // A background speculative first frame is accepted only before this
    // submission session has accepted another frame. It does not count as
    // committed.
    bool renderSpeculativeFirstFrame(VideoFrameRef frame);
    // Presents black for the empty-window state.
    void clearToBlack();
    // Reactivates a retained frame or clear after the window becomes visible,
    // with a new retry budget.
    void kickSubmitDrain();
    // Session boundary: queued or retained frames are dropped so old work
    // cannot render with new media settings.
    void discardPendingSubmits();
    // While suspended only the latest intent is kept.
    void setSubmitsSuspended(bool suspended);

    uint64_t committedFrameCount() const;
    // Unsupported or unrenderable frames, not an unavailable target.
    uint64_t hardRenderFailureCount() const;

    std::string outputModeDescription();

    // Stream colour metadata as FFmpeg AVColor* values.
    void setColorimetry(int primaries, int trc, int colorspace, int range, float peakNits);
    // Publication barrier before any frame of an opening session: the
    // completion runs once the output mode for the setters above is in place.
    void synchronizeOutputMode(std::function<void(bool ready)> completion);

    void setViewportPixelSize(int width, int height);
    // Display capability: `potentialEDR` > 1 means HDR output is possible;
    // `currentEDR` is the headroom (peak / SDR white).
    void updateOutputMode(double currentEDR, double potentialEDR);
    void setDisplayEDRHeadroom(double currentEDR);
    // The SDR white level of the output in nits (scRGB 1.0 = 80 nits).
    void setSdrWhiteLevelNits(float nits);
    void setSDRBoostEnabled(bool enabled);
    bool sdrBoostEnabled();
    bool displayEdrCapable();
    double displayEDRHeadroom();

    void setSampleAspect(float sar);
    // Dolby Vision profile 5: the IPTPQ path instead of the YCbCr matrix.
    void setDoviIPT(bool on);
    // RPU metadata queued by packet timestamp and bound by frame timestamp.
    void queueDoviReshapeFloats(const float *data, int64_t ptsUs);
    void bindDoviReshape(int64_t ptsUs, int64_t frameIntervalUs);
    void clearDoviReshapeQueue();
    void resetDoviSessionState();

    // Premultiplied BGRA8 subtitle texture on this renderer's device, and its
    // rectangle in viewport pixels from the top left; null removes it.
    void setSubtitleTexture(ID3D11ShaderResourceView *texture);
    void setSubtitleRect(RenderRect rect);

    void setAspectMode(int mode);      // 0: original, 4: crop
    void setForcedAspect(float ratio); // positive forces display aspect
    void setCropAspect(float ratio);   // positive crops to that aspect
    void setRotation(int degrees);     // 0/90/180/270
    void setMirror(int mirror);        // 0 none, 1 horizontal, 2 vertical
    void resetPictureTransform();
    void setBrightness(float v); // -0.5...0.5
    void setContrast(float v);   // 0.5...2
    void setSaturation(float v); // 0...2
    void setGamma(float v);      // 0.5...2

    bool isReady() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace sp
