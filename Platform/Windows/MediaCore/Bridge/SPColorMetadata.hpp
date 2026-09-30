// Platform-neutral color description shared by the renderers: FFmpeg color
// values to the renderer's transfer and source-gamut indices. The CoreVideo
// tag mapping built on top of it lives in SPVideoColorMetadata.hpp.
#pragma once

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace sp {

// A stream declaration is authoritative even when the platform has no
// equivalent tag for it. Only AVCOL_*_UNSPECIFIED delegates that individual
// field to the decoded frame; an unsupported explicit stream value must not
// be silently replaced by a frame value with different semantics.
inline constexpr int spAuthoritativeColorValue(int streamValue,
                                                int frameValue,
                                                int unspecifiedValue) {
    return streamValue != unspecifiedValue ? streamValue : frameValue;
}

enum : int {
    kSPTransferBT709  = 0,
    kSPTransferPQ     = 1, // SMPTE ST 2084
    kSPTransferHLG    = 2, // ARIB STD-B67
    kSPTransferLinear = 3,
    kSPTransferSRGB   = 4, // IEC 61966-2-1
    kSPTransferGamma22 = 5,
    kSPTransferGamma28 = 6,
    kSPTransferST428   = 7,
};

struct SPRendererTransfer {
    int index;
    bool hdr;

    constexpr bool operator==(const SPRendererTransfer &) const = default;
};

inline constexpr SPRendererTransfer spRendererTransferForAVTrc(int trc) {
    switch (trc) {
        case AVCOL_TRC_SMPTE2084:    return { kSPTransferPQ,     true  };
        case AVCOL_TRC_ARIB_STD_B67: return { kSPTransferHLG,    true  };
        case AVCOL_TRC_LINEAR:       return { kSPTransferLinear, false };
        case AVCOL_TRC_IEC61966_2_1: return { kSPTransferSRGB,   false };
        case AVCOL_TRC_GAMMA22:      return { kSPTransferGamma22, false };
        case AVCOL_TRC_GAMMA28:      return { kSPTransferGamma28, false };
        case AVCOL_TRC_SMPTE428:     return { kSPTransferST428,   false };

        default:                     return { kSPTransferBT709,  false };
    }
}

enum SPRendererSourceGamut : int {
    kSPGamutBT709  = 0,
    kSPGamutBT2020 = 1,
    kSPGamutSMPTEC = 2, // SMPTE 170M / 240M（NTSC）
    kSPGamutEBU    = 3, // BT.470BG / EBU Tech 3213（PAL）
    kSPGamutP3     = 4, // P3-D65（SMPTE 432）
    kSPGamutDCIP3  = 5,
};

inline constexpr int spRendererSourceGamutForAV(int primaries, int matrix) {
    switch (primaries) {
        case AVCOL_PRI_BT2020:    return kSPGamutBT2020;
        case AVCOL_PRI_SMPTE170M:
        case AVCOL_PRI_SMPTE240M: return kSPGamutSMPTEC;
        case AVCOL_PRI_BT470BG:   return kSPGamutEBU;
        case AVCOL_PRI_SMPTE432:  return kSPGamutP3;
        case AVCOL_PRI_SMPTE431:  return kSPGamutDCIP3;
        case AVCOL_PRI_BT709:     return kSPGamutBT709;
        default: break;
    }
    const bool declared = primaries > 0 && primaries != AVCOL_PRI_UNSPECIFIED;
    if (!declared && (matrix == AVCOL_SPC_BT2020_NCL || matrix == AVCOL_SPC_BT2020_CL)) {
        return kSPGamutBT2020;
    }
    return kSPGamutBT709;
}

inline constexpr int spSDRLayerTagKey(int sourceGamut, int transfer, bool hasHdr) {
    return hasHdr ? 0 : 1 + (sourceGamut & 7) + (transfer & 0xF) * 8;
}
inline constexpr int spSDRLayerTagKeyGamut(int key) { return (key - 1) & 7; }
inline constexpr int spSDRLayerTagKeyTransfer(int key) { return (key - 1) >> 3; }
static_assert(spSDRLayerTagKey(kSPGamutBT709, kSPTransferBT709, false) == 1);
static_assert(spSDRLayerTagKey(kSPGamutBT2020, kSPTransferPQ, true) == 0);
static_assert(spSDRLayerTagKeyGamut(spSDRLayerTagKey(kSPGamutDCIP3, kSPTransferGamma28, false)) == kSPGamutDCIP3);
static_assert(spSDRLayerTagKeyTransfer(spSDRLayerTagKey(kSPGamutDCIP3, kSPTransferGamma28, false)) == kSPTransferGamma28);

inline constexpr double spCVGammaLevelForTransfer(int transfer) {
    return transfer == kSPTransferGamma22 ? 2.2 : transfer == kSPTransferGamma28 ? 2.8 : 0.0;
}

} // namespace sp
