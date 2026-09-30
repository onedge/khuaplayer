#include "SPDoviRPU.hpp"

#include <cstring>
#include <vector>

namespace sp {

namespace {

struct BitReader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    bool bad = false;

    BitReader(const uint8_t* d, size_t bytes) : p(d), n(bytes * 8) {}

    uint32_t bits(int c) {
        if (c == 0) return 0;
        if (pos + c > n || c > 32) { bad = true; return 0; }
        uint32_t v = 0;
        for (int i = 0; i < c; i++) {
            v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1);
            pos++;
        }
        return v;
    }
    int32_t sbits(int c) {
        uint32_t v = bits(c);

        if (c < 32 && (v & (1u << (c - 1)))) v |= ~((1u << c) - 1);
        return (int32_t)v;
    }
    uint32_t ue() { // Exp-Golomb
        int zeros = 0;
        while (!bad && bits(1) == 0) {
            if (++zeros > 31) { bad = true; return 0; }
        }
        if (bad) return 0;
        return (1u << zeros) - 1 + bits(zeros);
    }
    int32_t se() {
        uint32_t k = ue();
        return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
    }

    double coef(int log2Denom) {
        int32_t ip = se();
        uint64_t frac = 0;
        int rem = log2Denom;
        while (rem > 0 && !bad) {
            int c = rem > 32 ? 32 : rem;
            frac = (frac << c) | bits(c);
            rem -= c;
        }
        return (double)ip + (double)frac / (double)(1ull << log2Denom);
    }
};

void stripEmulation(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
        if (i >= 2 && in[i] == 3 && in[i - 1] == 0 && in[i - 2] == 0) continue;
        out.push_back(in[i]);
    }
}

bool parseRPUPayload(const uint8_t* rbsp, size_t n, DoviReshape& out) {
    BitReader b(rbsp, n);
    if (b.bits(8) != 25) return false; // rpu_nal_prefix
    uint32_t rpuType = b.bits(6);
    uint32_t rpuFormat = b.bits(11);
    if (rpuType != 2) return false;

    b.bits(4); // vdr_rpu_profile
    b.bits(4); // vdr_rpu_level
    bool disableResidual = true;
    if (b.bits(1)) { // vdr_seq_info_present
        b.bits(1); // chroma_resampling_explicit_filter
        uint32_t coefDataType = b.bits(2);
        if (coefDataType == 0) out.coefLog2Denom = (int)b.ue();

        if (out.coefLog2Denom < 0 || out.coefLog2Denom > 32) return false;
        b.bits(2); // vdr_rpu_normalized_idc
        b.bits(1); // bl_video_full_range
        if ((rpuFormat & 0x700) == 0) {
            out.blBitDepth = (int)b.ue() + 8;
            b.ue(); // el_bit_depth
            out.vdrBitDepth = (int)b.ue() + 8;
            if (out.blBitDepth < 8 || out.blBitDepth > 16 ||
                out.vdrBitDepth < 8 || out.vdrBitDepth > 16) return false;
            b.bits(1); // spatial_resampling
            b.bits(3); // reserved
            b.bits(1); // el_spatial_resampling
            disableResidual = b.bits(1);
        }
    }
    bool dmPresent = b.bits(1);
    bool usePrev = b.bits(1);
    if (usePrev) {
        b.ue(); // prev_vdr_rpu_id
        out.usePrev = true;
    } else {
        b.ue(); // vdr_rpu_id
        b.ue(); // mapping_color_space
        b.ue(); // mapping_chroma_format_idc
        int numPivots[3] = {};
        float blMax = (float)((1 << out.blBitDepth) - 1);
        for (int c = 0; c < 3; c++) {
            numPivots[c] = (int)b.ue() + 2;

            if (numPivots[c] < 2 || numPivots[c] > 64) return false;
            uint32_t acc = 0;
            for (int i = 0; i < numPivots[c] && !b.bad; i++) {
                acc += b.bits(out.blBitDepth);
                if (i <= kDoviMaxPieces) out.comps[c].pivots[i] = (float)acc / blMax;
            }
            out.comps[c].numPieces = numPivots[c] - 1;
        }
        if ((rpuFormat & 0x700) == 0 && !disableResidual) {
            b.bits(3);

        }
        b.ue(); // num_x_partitions_minus1
        b.ue(); // num_y_partitions_minus1

        bool curvesOk = true;
        for (int c = 0; c < 3 && !b.bad; c++) {
            DoviReshape::Comp& comp = out.comps[c];
            if (comp.numPieces > kDoviMaxPieces) { curvesOk = false; }
            for (int p = 0; p < numPivots[c] - 1 && !b.bad; p++) {
                uint32_t mappingIdc = b.ue();
                if (mappingIdc == 0) {

                    int order = (int)b.ue() + 1;
                    if (order > 2) return false;
                    if (order == 1) b.bits(1);
                    double coefs[3] = {0, 0, 0};
                    for (int i = 0; i <= order && i < 3; i++) {
                        coefs[i] = b.coef(out.coefLog2Denom);
                    }
                    if (p < kDoviMaxPieces) {
                        comp.pieces[p].type = 0;
                        for (int i = 0; i < 3; i++) comp.pieces[p].c[i] = (float)coefs[i];
                    }
                } else if (mappingIdc == 1) {

                    int order = (int)b.bits(2) + 1;
                    if (order > 3) return false;
                    double cst = b.coef(out.coefLog2Denom);
                    double mc[3][7] = {};
                    for (int i = 0; i < order && i < 3; i++) {
                        for (int j = 0; j < 7; j++) mc[i][j] = b.coef(out.coefLog2Denom);
                    }
                    if (p < kDoviMaxPieces) {
                        comp.pieces[p].type = 1;
                        comp.mmrOrder = order;
                        comp.mmrConst = (float)cst;
                        for (int i = 0; i < 3; i++)
                            for (int j = 0; j < 7; j++) comp.mmrCoef[i][j] = (float)mc[i][j];
                    }
                } else {
                    curvesOk = false;
                }
            }
        }

        if (curvesOk) {
            for (int c = 0; c < 3; c++) {
                DoviReshape::Comp& comp = out.comps[c];
                if (comp.numPieces == 1 && comp.pieces[0].type == 0 &&
                    comp.pieces[0].c[1] == 0.0f && comp.pieces[0].c[2] == 0.0f) {
                    comp.numPieces = 0;
                }
            }
        }
        out.hasCurves = curvesOk && !b.bad &&
                        (out.comps[0].numPieces > 0 || out.comps[1].numPieces > 0 ||
                         out.comps[2].numPieces > 0);
    }

    if (dmPresent && !b.bad) {
        b.ue(); // affected_dm_metadata_id
        b.ue(); // current_dm_metadata_id
        b.ue(); // scene_refresh_flag
        for (int i = 0; i < 9; i++) out.yccToRgb[i] = (float)(b.sbits(16) / 8192.0);
        for (int i = 0; i < 3; i++) {
            uint64_t hi = b.bits(32);
            out.yccOffset[i] = (float)((double)hi / 268435456.0); // /2^28
        }
        for (int i = 0; i < 9; i++) out.rgbToLms[i] = (float)(b.sbits(16) / 16384.0);

        b.bits(16); // signal_eotf
        b.bits(16); // signal_eotf_param0
        b.bits(32); // signal_eotf_param1
        b.bits(16); // signal_eotf_param2
        b.bits(5);  // signal_bit_depth
        b.bits(2);  // signal_color_space
        b.bits(2);  // signal_chroma_format
        out.signalFullRange = (b.bits(2) == 1); // signal_full_range_flag
        out.minPqNorm = (float)(b.bits(12) / 4095.0);
        out.maxPqNorm = (float)(b.bits(12) / 4095.0);
        b.bits(10); // source_diagonal
        out.hasDm = !b.bad;
    }
    out.valid = !b.bad;
    return out.valid;
}

bool parseFromNal(const uint8_t* nal, size_t len, DoviReshape& out) {
    if (len < 3) return false;
    int nalType = (nal[0] >> 1) & 0x3F;
    if (nalType != 62) return false;
    std::vector<uint8_t> rbsp;
    stripEmulation(nal + 2, len - 2, rbsp);
    DoviReshape tmp;
    if (parseRPUPayload(rbsp.data(), rbsp.size(), tmp)) {
        out = tmp;
        return true;
    }
    return false;
}

} // namespace

bool doviParseRPUFromPacket(const uint8_t* data, size_t size, bool isAnnexB, DoviReshape& out,
                            int nalLengthSize) {
    bool found = false;
    if (!data || size < 5) return false;
    if (!isAnnexB) {

        if (nalLengthSize < 1 || nalLengthSize > 4) nalLengthSize = 4;
        const size_t ls = (size_t)nalLengthSize;
        size_t i = 0;
        while (i + ls <= size) {
            uint32_t len = 0;
            for (size_t b = 0; b < ls; b++) len = (len << 8) | data[i + b];
            i += ls;
            if (len == 0 || i + len > size) break;
            if (parseFromNal(data + i, len, out)) found = true;
            i += len;
        }
    } else {

        size_t i = 0;
        while (i + 4 < size) {
            if (data[i] == 0 && data[i + 1] == 0 &&
                (data[i + 2] == 1 || (data[i + 2] == 0 && data[i + 3] == 1))) {
                size_t start = i + (data[i + 2] == 1 ? 3 : 4);
                size_t next = start;
                while (next + 3 < size &&
                       !(data[next] == 0 && data[next + 1] == 0 &&
                         (data[next + 2] == 1 ||
                          (next + 4 < size && data[next + 2] == 0 && data[next + 3] == 1)))) {
                    next++;
                }
                if (next + 3 >= size) next = size;
                if (parseFromNal(data + start, next - start, out)) found = true;
                i = next;
            } else {
                i++;
            }
        }
    }
    return found;
}

void doviToGpuFloats(const DoviReshape& r, float* o) {
    memset(o, 0, sizeof(float) * kDoviGpuFloats);
    for (int i = 0; i < 9; i++) o[i] = r.yccToRgb[i];
    for (int i = 0; i < 3; i++) o[9 + i] = r.yccOffset[i];
    for (int i = 0; i < 9; i++) o[12 + i] = r.rgbToLms[i];
    o[21] = r.minPqNorm;
    o[22] = r.maxPqNorm;
    o[23] = r.signalFullRange ? 1.0f : 0.0f;
    o[24] = r.hasCurves ? 1.0f : 0.0f;
    for (int c = 0; c < 3; c++) {
        const DoviReshape::Comp& comp = r.comps[c];
        int C = 28 + c * 44;
        o[C] = (float)comp.numPieces;
        for (int i = 0; i <= kDoviMaxPieces; i++) o[C + 1 + i] = comp.pivots[i];
        for (int p = 0; p < kDoviMaxPieces; p++) {
            int P = C + 12 + p * 4;
            o[P] = (float)comp.pieces[p].type;
            o[P + 1] = comp.pieces[p].c[0];
            o[P + 2] = comp.pieces[p].c[1];
            o[P + 3] = comp.pieces[p].c[2];
        }
        int M = 160 + c * 24;

        o[M] = (float)(comp.mmrOrder > 3 ? 3 : comp.mmrOrder);
        o[M + 1] = comp.mmrConst;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 7; j++) o[M + 2 + i * 7 + j] = comp.mmrCoef[i][j];
    }
}

} // namespace sp
