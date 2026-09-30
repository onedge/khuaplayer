#pragma once

#include <cstdint>
#include <cstddef>

namespace sp {

constexpr int kDoviMaxPieces = 8;

struct DoviReshape {
    bool valid = false;
    bool usePrev = false;

    int coefLog2Denom = 23;
    int blBitDepth = 10;
    int vdrBitDepth = 12;

    bool hasCurves = false;
    struct Comp {
        int numPieces = 0;
        float pivots[kDoviMaxPieces + 1] = {};
        struct Piece {
            int type = 0;                     // 0=poly 1=mmr
            float c[3] = {0, 1, 0};           // poly: c0+c1*s+c2*s²
        } pieces[kDoviMaxPieces];

        int mmrOrder = 0;
        float mmrConst = 0;
        float mmrCoef[3][7] = {};
    } comps[3];

    bool hasDm = false;
    float yccToRgb[9] = {1, 0.09753f, 0.20520f,
                         1, -0.11389f, 0.13318f,
                         1, 0.03259f, -0.67688f};
    float yccOffset[3] = {0, 0.5f, 0.5f};
    float rgbToLms[9] = {1.042542f, -0.021301f, -0.021301f,
                         -0.021301f, 1.042542f, -0.021301f,
                         -0.021301f, -0.021301f, 1.042542f};
    bool signalFullRange = true;
    float minPqNorm = 0.0f;     // source_min_pq / 4095
    float maxPqNorm = 3696.0f / 4095.0f;
};

bool doviParseRPUFromPacket(const uint8_t* data, size_t size, bool isAnnexB, DoviReshape& out,
                            int nalLengthSize = 4);

constexpr int kDoviGpuFloats = 232;
void doviToGpuFloats(const DoviReshape& r, float* out);

} // namespace sp
