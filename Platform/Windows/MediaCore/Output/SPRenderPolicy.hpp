// The Mac renderer's pure decisions (SPMetalRenderer.mm), unchanged: which
// output mode a frame needs, when the output must be reconfigured, when SDR
// boost is in effect, and the aspect/crop model behind uniforms 27...29.
#pragma once

#include "SPColorMetadata.hpp"

#include <algorithm>

namespace sp {

// HDR content uses the EDR output on a display with headroom; an SDR display
// intentionally keeps mode 0 and tone-maps that same frame through the BGRA8
// path. EDR (sdrBoost) lifts SDR content into the EDR output on a capable
// display; it never changes anything for HDR content or on a display without
// headroom.
inline constexpr int spDesiredRendererOutputMode(bool displayEdrCapable, bool hasHdr, bool sdrBoost = false) {
    return (displayEdrCapable && (hasHdr || sdrBoost)) ? 1 : 0;
}
inline constexpr bool spFrameNeedsOutputModeRetry(bool displayEdrCapable, bool hasHdr, int currentMode,
                                                  bool sdrBoost = false) {
    return spDesiredRendererOutputMode(displayEdrCapable, hasHdr, sdrBoost) != currentMode;
}
static_assert(spFrameNeedsOutputModeRetry(true, true, 0));   // PQ/HLG on EDR
static_assert(spFrameNeedsOutputModeRetry(true, false, 1));  // HDR -> SDR
static_assert(!spFrameNeedsOutputModeRetry(false, true, 0)); // SDR tone-map
static_assert(!spFrameNeedsOutputModeRetry(true, true, 1));  // already EDR
static_assert(spFrameNeedsOutputModeRetry(true, false, 0, true));   // EDR on SDR content
static_assert(!spFrameNeedsOutputModeRetry(true, false, 1, true));  // EDR already EDR
static_assert(!spFrameNeedsOutputModeRetry(false, false, 0, true)); // EDR without headroom
static_assert(!spFrameNeedsOutputModeRetry(true, true, 1, true));

static_assert(spSDRLayerTagKey(kSPGamutBT2020, kSPTransferPQ, true) == 0); // HDR tone-mapped to 709
static_assert(spSDRLayerTagKey(kSPGamutBT2020, kSPTransferBT709, false) != 0); // 2020 SDR
static_assert(spSDRLayerTagKey(kSPGamutP3, kSPTransferBT709, false) != spSDRLayerTagKey(kSPGamutP3, kSPTransferSRGB, false));

// On Windows the "tag" is the shader's final conversion rather than layer
// state, but it still decides when the output configuration is out of date.
inline constexpr bool spOutputConfigNeedsTransition(bool displayEdrCapable, bool hasHdr, int currentMode,
                                                    bool sdrBoost, int sourceGamut, int transfer, int appliedTagKey) {
    const int desired = spDesiredRendererOutputMode(displayEdrCapable, hasHdr, sdrBoost);
    if (desired != currentMode) return true;
    return desired == 0 && spSDRLayerTagKey(sourceGamut, transfer, hasHdr) != appliedTagKey;
}
inline constexpr int kSPTag709 = spSDRLayerTagKey(kSPGamutBT709, kSPTransferBT709, false);
static_assert(spOutputConfigNeedsTransition(false, false, 0, false, kSPGamutP3, kSPTransferBT709, kSPTag709)); // tag 709→P3 pending
static_assert(!spOutputConfigNeedsTransition(false, false, 0, false, kSPGamutP3, kSPTransferBT709,
                                             spSDRLayerTagKey(kSPGamutP3, kSPTransferBT709, false))); // tag applied
static_assert(!spOutputConfigNeedsTransition(true, false, 1, true, kSPGamutP3, kSPTransferBT709, 0)); // EDR: tag irrelevant
static_assert(!spOutputConfigNeedsTransition(false, true, 0, false, kSPGamutBT2020, kSPTransferPQ, 0)); // HDR→SDR keeps sRGB tag
static_assert(spOutputConfigNeedsTransition(false, false, 0, false, kSPGamutBT709, kSPTransferSRGB, kSPTag709)); // 709→sRGB pending

// Effective boost = requested ∧ content is SDR ∧ the EDR output is actually
// in use. HDR content keeps its own path; on a display without headroom the
// mode stays 0, so the boost bit must stay clear too.
inline constexpr bool spEffectiveSdrBoost(bool requested, bool hasHdr, int outputMode) {
    return requested && !hasHdr && outputMode == 1;
}
static_assert(spEffectiveSdrBoost(true, false, 1));
static_assert(!spEffectiveSdrBoost(true, false, 0));
static_assert(!spEffectiveSdrBoost(true, true, 1));
static_assert(!spEffectiveSdrBoost(false, false, 1));

struct SPAspectModel {
    float dispAspect, eff, cw, ch;
};
inline SPAspectModel spAspectModel(float vidW, float vidH, float sar, float forcedAspect, float cropAspect) {
    float s = sar > 0 ? sar : 1.0f;
    float aV = (vidW > 0 && vidH > 0) ? vidW * s / vidH : 16.0f / 9.0f;
    if (forcedAspect > 0.1f) aV = forcedAspect;
    float eff = forcedAspect, cw = 0.0f, ch = 0.0f;
    if (cropAspect > 0.1f) {
        cw = std::min(1.0f, cropAspect / aV);
        ch = std::min(1.0f, aV / cropAspect);
        eff = cropAspect;
        aV = cropAspect;
    }
    return {aV, eff, cw, ch};
}

} // namespace sp
