// The Mac player's Video.metal in HLSL (shader model 5.0), for the D3D11
// renderer. The colour math, geometry and uniform layout are the same line
// for line; only two things differ:
//   - Metal specialises with function constants. This is the generic path,
//     which reads the same values from the uniforms (kSpec == false there).
//   - macOS converts the layer's colour space for the display. Windows does
//     not, so spEncodeForSwapChain does that last step itself (see there).
// pow() of a negative is NaN, as in Metal, and max() then yields the other
// operand in both; the original relies on that, so keep fxc quiet about it.
#pragma warning(disable : 3571)
//
// Metal matrices are built from columns; the same literals here are rows, so
// every `M * v` of the original is mul(v, M).

cbuffer Uniforms : register(b0) {
    float4 U[8]; // the 32 floats of SPColorUniforms
};
#define UV(i) U[(i) >> 2][(i) & 3]

cbuffer DoviUniforms : register(b1) {
    float4 D[58]; // the 232 floats of SPDoviUniforms
};
#define DV(i) D[(i) >> 2][(i) & 3]

// Windows only: how the swap chain takes the Mac layer's pixel values.
// W.x: scRGB units per 1.0 of EDR output (the SDR white level / 80 nits).
// W.y: 1 leaves SDR output untouched, as SP_NO_COLORMATCH does on the Mac.
// W.zw: the visible part of the video textures (decoder surfaces are padded,
//       1080p decodes into 1088 lines); the Mac's buffers are exact.
cbuffer OutputUniforms : register(b2) {
    float4 W;
};

Texture2DArray<float4> yTex : register(t0);
Texture2DArray<float4> uvTex : register(t1);
Texture2D<float4> subTex : register(t2);
Texture2DArray<float4> vTex : register(t3);
SamplerState s : register(s0); // linear, clamp to edge

struct VertexOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// ---- Full-screen triangle ------------------------------------------------

VertexOut videoVertex(uint vid : SV_VertexID) {
    float2 pos = float2(float((vid << 1) & 2), float(vid & 2)); // 0..2
    pos = pos * 2.0 - 1.0;

    float sar = (UV(10) > 0.01) ? UV(10) : 1.0;
    float aspectV = (UV(0) > 0 && UV(1) > 0) ? (UV(0) * sar) / UV(1) : 16.0 / 9.0;

    float forced = UV(27);
    if (forced > 0.01) aspectV = forced;
    float aspectP = (UV(2) > 0 && UV(3) > 0) ? UV(2) / UV(3) : aspectV;

    int mode = int(UV(17));
    int rot = int(UV(18));
    int mirror = int(UV(19));

    float aEff = (rot == 1 || rot == 3) ? 1.0 / aspectV : aspectV;

    float sx = 1.0, sy = 1.0;
    if (mode == 4) {
        if (aEff > aspectP) { sx = aEff / aspectP; }
        else { sy = aspectP / aEff; }
    } else {
        if (aEff > aspectP) { sy = aspectP / aEff; }
        else { sx = aEff / aspectP; }
    }

    float2 q = pos;
    if (mirror == 1) q.x = -q.x;
    else if (mirror == 2) q.y = -q.y;
    q.x /= sx;
    q.y /= sy;
    if (rot == 1) { q = float2(q.y, -q.x); }
    else if (rot == 2) { q = -q; }
    else if (rot == 3) { q = float2(-q.y, q.x); }

    VertexOut o;
    o.position = float4(pos, 0.0, 1.0);
    o.uv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    return o;
}

// ---- Colour math -----------------------------------------------------------

float eotfSRGB(float c) {
    return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float oetfSRGB(float c) {
    return (c <= 0.0031308) ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float3 oetfSRGBExt(float3 c) {
    const float3 hi = 1.055 * pow(max(c, float3(0.0, 0.0, 0.0)), float3(1.0 / 2.4, 1.0 / 2.4, 1.0 / 2.4)) - 0.055;
    const float3 lo = 12.92 * c;
    return float3(c.r <= 0.0031308 ? lo.r : hi.r, c.g <= 0.0031308 ? lo.g : hi.g, c.b <= 0.0031308 ? lo.b : hi.b);
}

float eotfCoreMediaGamma(float c, float gamma) {
    return max(pow(c, gamma), c / 16.0);
}

float spSdrTierGamma(int transfer) {
    return (transfer == 5) ? 2.2 : (transfer == 6) ? 2.8 : (transfer == 7) ? 2.6 : 1.9609375;
}

float3 eotfSdrTier(int transfer, float3 c) {
    if (transfer == 4) return float3(eotfSRGB(c.r), eotfSRGB(c.g), eotfSRGB(c.b));
    if (transfer == 3) return c;
    const float g = spSdrTierGamma(transfer);
    return float3(eotfCoreMediaGamma(c.r, g), eotfCoreMediaGamma(c.g, g), eotfCoreMediaGamma(c.b, g));
}

float3 oetfSdrTier(int transfer, float3 l) {
    if (transfer == 4) return float3(oetfSRGB(l.r), oetfSRGB(l.g), oetfSRGB(l.b));
    if (transfer == 3) return l;
    const float g = spSdrTierGamma(transfer);
    return min(pow(max(l, float3(0.0, 0.0, 0.0)), float3(1.0 / g, 1.0 / g, 1.0 / g)), 16.0 * l);
}

float eotfPQ(float c) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float cp = pow(c, 1.0 / m2);
    float num = max(cp - c1, 0.0);
    float den = c2 - c3 * cp;
    return pow(num / den, 1.0 / m1);
}

float eotfHLG(float c) {
    const float a = 0.17883277;
    const float b = 1.0 - 4.0 * a;
    const float c0 = 0.5 - a * log(4.0 * a);
    return (c <= 0.5) ? c * c / 3.0 : (exp((c - c0) / a) + b) / 12.0;
}

// Convert the post-PQ, post-RPU-crosstalk LMS signal to linear BT.2020.
// Dolby metadata's rgb_to_lms_matrix already carries the stream-specific
// crosstalk transform, so this final Hunt-Pointer-Estevez inverse must not
// apply the 4% ICtCp crosstalk a second time.
float3 spDoviHpeLmsToBt2020(float3 lms) {
    return float3(
         3.0644187900 * lms.x - 2.1659767600 * lms.y + 0.1015581800 * lms.z,
        -0.6561210800 * lms.x + 1.7855411800 * lms.y - 0.1294374900 * lms.z,
         0.0173632100 * lms.x - 0.0472515400 * lms.y + 1.0300425300 * lms.z);
}

static const float3x3 m2020ToP3 = float3x3(
    float3(1.343578, -0.065297,  0.002822),
    float3(-0.282180, 1.075788, -0.019598),
    float3(-0.061399, -0.010490, 1.016777));

static const float3x3 m2020To709 = float3x3(
    float3(1.660491, -0.124550, -0.018151),
    float3(-0.587641, 1.132900, -0.100579),
    float3(-0.072850, -0.008349, 1.118730));

static const float3x3 m709ToP3 = float3x3(
    float3(0.822462, 0.033194, 0.017083),
    float3(0.177538, 0.966806, 0.072397),
    float3(0.000000, 0.000000, 0.910520));

static const float3x3 mSMPTECTo709 = float3x3(
    float3(0.939542, 0.017772, -0.001622),
    float3(0.050181, 0.965793, -0.004370),
    float3(0.010277, 0.016435,  1.005991));
static const float3x3 mEBUTo709 = float3x3(
    float3(1.044043, 0.000000, 0.000000),
    float3(-0.044043, 1.000000, 0.011793),
    float3(0.000000, 0.000000, 0.988207));

static const float3x3 mP3To709 = float3x3(
    float3(1.224940, -0.042057, -0.019638),
    float3(-0.224940, 1.042057, -0.078636),
    float3(0.000000, 0.000000, 1.098274));

static const float3x3 mDCIP3To709 = float3x3(
    float3(1.157516, -0.041500, -0.018050),
    float3(-0.154962, 1.045568, -0.078578),
    float3(-0.002554, -0.004068, 1.096628));

static const float3x3 m709To2020 = float3x3(
    float3(0.627404, 0.069097, 0.016391),
    float3(0.329283, 0.919540, 0.088013),
    float3(0.043313, 0.011362, 0.895595));
static const float3x3 m709ToSMPTEC = float3x3(
    float3(1.065379, -0.019633, 0.001632),
    float3(-0.055401, 1.036363, 0.004412),
    float3(-0.009978, -0.016731, 0.993956));
static const float3x3 m709ToEBU = float3x3(
    float3(0.957815, 0.000000, 0.000000),
    float3(0.042185, 1.000000, -0.011934),
    float3(0.000000, 0.000000, 1.011934));
static const float3x3 m709ToDCIP3 = float3x3(
    float3(0.868580, 0.034540, 0.016771),
    float3(0.128919, 0.961811, 0.071040),
    float3(0.002501, 0.003648, 0.912189));

float3 spSourceTo709(int gamut, float3 lin) {
    if (gamut == 1) return mul(lin, m2020To709);
    if (gamut == 2) return mul(lin, mSMPTECTo709);
    if (gamut == 3) return mul(lin, mEBUTo709);
    if (gamut == 4) return mul(lin, mP3To709);
    if (gamut == 5) return mul(lin, mDCIP3To709);
    return lin;
}

float3 spSourceToP3(int gamut, float3 lin) {
    if (gamut == 1) return mul(lin, m2020ToP3);
    if (gamut == 4) return lin;
    return mul(spSourceTo709(gamut, lin), m709ToP3);
}

float3 sp709ToSource(int gamut, float3 lin) {
    if (gamut == 1) return mul(lin, m709To2020);
    if (gamut == 2) return mul(lin, m709ToSMPTEC);
    if (gamut == 3) return mul(lin, m709ToEBU);
    if (gamut == 4) return mul(lin, m709ToP3);
    if (gamut == 5) return mul(lin, m709ToDCIP3);
    return lin;
}

float spXdrGain(float yEnc, float ratio) {
    float y = clamp(yEnc, 0.0, 1.0);
    float c = y * 255.0;
    float e = (c <= 70.0)
        ? (1.8712e-5 * c * c - 2.7334e-3 * c + 1.3141)
        : (2.8305e-6 * c * c - 7.4622e-4 * c + 1.2528);
    float r = max(ratio, 1.0);
    float k = log(r) / log(10.0);
    float lnG = (log(10.0) + 2.4 * (e - 1.0) * log(max(y, 1e-4))) * k;
    float g = lerp(1.0, exp(lnG), smoothstep(0.35, 0.85, y));
    return clamp(g, 1.0, r);
}

float spXdrGain2(float yEnc, float headroom, bool is10bit) {
    float rMid = clamp(headroom, 1.0, 6.0);
    float rHigh = clamp(headroom, rMid, is10bit ? 16.0 : 8.0);
    float g = spXdrGain(yEnc, rMid);
    float t = smoothstep(0.85, 1.0, clamp(yEnc, 0.0, 1.0));
    g *= pow(rHigh / rMid, t);
    return clamp(g, 1.0, rHigh);
}

float3 yuvToRgb(int primaries, float Y, float Cb, float Cr) {
    float3 rgb;
    if (primaries == 1) {            // BT.2020
        rgb = float3(
            Y + 1.4746 * Cr,
            Y - 0.164553 * Cb - 0.571353 * Cr,
            Y + 1.8814 * Cb);
    } else if (primaries == 2) {     // BT.601
        rgb = float3(
            Y + 1.402 * Cr,
            Y - 0.344136 * Cb - 0.714136 * Cr,
            Y + 1.772 * Cb);
    } else if (primaries == 3) {
        rgb = float3(
            Y + 1.576 * Cr,
            Y - 0.226622 * Cb - 0.476622 * Cr,
            Y + 1.826 * Cb);
    } else {
        rgb = float3(
            Y + 1.5748 * Cr,
            Y - 0.187324 * Cb - 0.468124 * Cr,
            Y + 1.8556 * Cb);
    }
    return rgb;
}

float3 toneMap(float3 lin10k, float contentPeak, float targetPeak) {
    float3 x = lin10k * (10000.0 / targetPeak);
    float w = max(contentPeak / targetPeak, 1.0001);
    float3 num = x * (1.0 + x / (w * w));
    return num / (1.0 + x);
}

float doviReshapeComp(int c, float3 s3) {
    int C = 28 + c * 44;
    int np = int(DV(C));
    float x = s3[c];
    if (np <= 0) return x;
    int p = 0;
    for (int i = 1; i < np; i++) {
        if (x >= DV(C + 1 + i)) p = i;
    }
    int P = C + 12 + p * 4;
    if (DV(P) < 0.5) {
        return DV(P + 1) + DV(P + 2) * x + DV(P + 3) * x * x;
    }

    int M = 160 + c * 24;
    int order = int(DV(M));
    float t[7] = {s3.x, s3.y, s3.z, s3.x * s3.y, s3.x * s3.z, s3.y * s3.z, s3.x * s3.y * s3.z};
    float r = DV(M + 1);
    // tp = t[j]^(o+1); the RPU parser clamps the order to 3, so o <= 2.
    for (int o = 0; o < order; o++) {
        for (int j = 0; j < 7; j++) {
            float tp = t[j];
            if (o >= 1) tp *= t[j];
            if (o >= 2) tp *= t[j];
            r += DV(M + 2 + o * 7 + j) * tp;
        }
    }
    return r;
}

// ---- Windows output --------------------------------------------------------

// macOS hands the layer's pixels to the compositor tagged with the layer's
// colour space and converts them for the display. Windows composes a
// B8G8R8A8 swap chain as sRGB and an FP16 one as scRGB (linear BT.709,
// 1.0 = 80 nits), so the conversion the Mac leaves to the system happens here.
//   - EDR (mode 1): the Mac layer is extended Display P3 with the sRGB curve,
//     1.0 = SDR white. Decode, convert to BT.709 and scale to the SDR white.
//   - SDR (mode 0): the Mac tags the layer with the source's primaries and
//     transfer (spSDRLayerTagKey); HDR tone-mapped for SDR is tagged sRGB.
//     Decode with the source's curve, convert to BT.709, encode as sRGB.
float3 spEncodeForSwapChain(float3 c, bool edrOut, bool hasHdr, int gamut, int transfer) {
    float3 result = c; // tagged sRGB, or unmanaged
    if (edrOut) {
        const float3 a = abs(c);
        const float3 lin = sign(c) * float3(eotfSRGB(a.r), eotfSRGB(a.g), eotfSRGB(a.b));
        result = mul(lin, mP3To709) * W.x;
    } else if (!hasHdr && W.y < 0.5) {
        const float3 lin = clamp(spSourceTo709(gamut, eotfSdrTier(transfer, c)), 0.0, 1.0);
        result = float3(oetfSRGB(lin.r), oetfSRGB(lin.g), oetfSRGB(lin.b));
    }
    return result;
}

// ---- Fragment --------------------------------------------------------------

float4 videoFragment(VertexOut i) : SV_Target {
    const int  primaries = int(UV(4));
    const int  transfer  = int(UV(5));
    const bool is10bit   = int(UV(7)) == 10;
    const bool fullRange = int(UV(6)) == 1;
    const bool hasHdr    = UV(8) > 0.5;
    const bool doviIPT   = UV(11) > 0.5;
    const bool edrOut    = int(UV(25)) == 1;
    const int  gamut     = int(UV(26));
    const bool gamut2020 = gamut == 1;
    const bool planar    = int(UV(30)) == 1;
    const bool sdrBoost  = UV(31) >= 1.0;

    bool inVideo = (i.uv.x >= 0.0 && i.uv.x <= 1.0 && i.uv.y >= 0.0 && i.uv.y <= 1.0);
    float3 outc = float3(0.0, 0.0, 0.0);
    if (inVideo) {

    float2 suv = i.uv;
    {
        float cw = UV(28), ch = UV(29);
        if (cw > 0.01 && ch > 0.01) {
            suv = float2(0.5 + (i.uv.x - 0.5) * cw,
                         0.5 + (i.uv.y - 0.5) * ch);
        }
    }
    // Keep bilinear taps half a texel inside the visible picture, so the
    // padding rows of a decoder surface do not blend into the last rows.
    // Luma and chroma texels differ in size; unpadded planes are unchanged.
    float yW, yH, cW, cH, slices;
    yTex.GetDimensions(yW, yH, slices);
    uvTex.GetDimensions(cW, cH, slices);
    const float2 vis = suv * W.zw;
    const float3 st = float3(min(vis, W.zw - 0.5 / float2(yW, yH)), 0.0);
    const float3 stC = float3(min(vis, W.zw - 0.5 / float2(cW, cH)), 0.0);

    float Y, Cb, Cr;
    if (planar && is10bit) {
        Y  = float(uint(yTex.Sample(s, st).r * 65535.0 + 0.0078125)) / 1023.0;
        Cb = float(uint(uvTex.Sample(s, stC).r * 65535.0 + 0.0078125)) / 1023.0;
        Cr = float(uint(vTex.Sample(s, stC).r * 65535.0 + 0.0078125)) / 1023.0;
    } else if (is10bit) {
        uint y16 = uint(yTex.Sample(s, st).r * 65535.0 + 0.5);
        float2 uv = uvTex.Sample(s, stC).rg;
        uint u16 = uint(uv.x * 65535.0 + 0.5);
        uint v16 = uint(uv.y * 65535.0 + 0.5);
        Y  = float(y16 >> 6) / 1023.0;
        Cb = float(u16 >> 6) / 1023.0;
        Cr = float(v16 >> 6) / 1023.0;
    } else if (planar) {
        Y  = yTex.Sample(s, st).r;
        Cb = uvTex.Sample(s, stC).r;
        Cr = vTex.Sample(s, stC).r;
    } else {
        // NV12
        Y  = yTex.Sample(s, st).r;
        Cb = uvTex.Sample(s, stC).r;
        Cr = uvTex.Sample(s, stC).g;
    }

    bool limitedRange = doviIPT ? (DV(23) < 0.5) : !fullRange;
    if (limitedRange) {
        if (is10bit) {
            Y  = (Y  - 64.0 / 1023.0) / (876.0 / 1023.0);
            Cb = (Cb - 64.0 / 1023.0) / (896.0 / 1023.0);
            Cr = (Cr - 64.0 / 1023.0) / (896.0 / 1023.0);
        } else {
            Y  = (Y  - 16.0 / 255.0) / (219.0 / 255.0);
            Cb = (Cb - 16.0 / 255.0) / (224.0 / 255.0);
            Cr = (Cr - 16.0 / 255.0) / (224.0 / 255.0);
        }
    }
    Y  = clamp(Y, 0.0, 1.0);
    Cb = clamp(Cb, 0.0, 1.0);
    Cr = clamp(Cr, 0.0, 1.0);

    const float chromaCenter = limitedRange ? 0.5 : (is10bit ? 512.0 / 1023.0 : 128.0 / 255.0);

    float3 lin = float3(0.0, 0.0, 0.0);
    float3 rgbEnc = float3(0.0, 0.0, 0.0);

    const bool sdrPassthrough = !doviIPT && !hasHdr && !edrOut;
    if (doviIPT) {
        float3 sig = float3(Y, Cb, Cr);
        if (DV(24) > 0.5) {
            sig = float3(doviReshapeComp(0, sig),
                         doviReshapeComp(1, sig),
                         doviReshapeComp(2, sig));
            sig = clamp(sig, 0.0, 1.0);
        }
        float3 ipt = sig - float3(DV(9), DV(10), DV(11));
        float3 lmsPq = float3(
            DV(0) * ipt.x + DV(1) * ipt.y + DV(2) * ipt.z,
            DV(3) * ipt.x + DV(4) * ipt.y + DV(5) * ipt.z,
            DV(6) * ipt.x + DV(7) * ipt.y + DV(8) * ipt.z);
        lmsPq = clamp(lmsPq, 0.0, 1.0);
        float3 lms = float3(eotfPQ(lmsPq.x), eotfPQ(lmsPq.y), eotfPQ(lmsPq.z));
        lms = float3(
            DV(12) * lms.x + DV(13) * lms.y + DV(14) * lms.z,
            DV(15) * lms.x + DV(16) * lms.y + DV(17) * lms.z,
            DV(18) * lms.x + DV(19) * lms.y + DV(20) * lms.z);

        lin = spDoviHpeLmsToBt2020(lms);
        lin = max(lin, 0.0);
    } else {
        rgbEnc = clamp(yuvToRgb(primaries, Y, Cb - chromaCenter, Cr - chromaCenter), 0.0, 1.0);
        if (sdrPassthrough) {
        } else if (transfer == 1) {           // PQ
            lin = float3(eotfPQ(rgbEnc.r), eotfPQ(rgbEnc.g), eotfPQ(rgbEnc.b));
        } else if (transfer == 2) {
            lin = float3(eotfHLG(rgbEnc.r), eotfHLG(rgbEnc.g), eotfHLG(rgbEnc.b));
            float Ys = dot(lin, float3(0.2627, 0.6780, 0.0593));
            lin *= pow(max(Ys, 1e-6), 0.2); // γ-1 = 0.2
            lin *= 0.1;
        } else {
            lin = eotfSdrTier(transfer, rgbEnc);
        }
    }

    float contentPeak = UV(9);
    if (doviIPT && DV(22) > 0.0) {
        contentPeak = max(eotfPQ(DV(22)) * 10000.0, 203.0);
    }
    if (hasHdr) {
        if (edrOut) {
            float3 nits;
            if (contentPeak > UV(24)) {
                nits = toneMap(lin, contentPeak, max(UV(24), 1.0)) * UV(24);
            } else {
                nits = lin * 10000.0;
            }
            nits = max(spSourceToP3(gamut, nits), float3(0.0, 0.0, 0.0));
            outc = oetfSRGBExt(nits / 100.0);
        } else {
            float3 tm = toneMap(lin, max(contentPeak, 203.0), 203.0);
            tm = clamp(spSourceTo709(gamut, tm), 0.0, 1.0);
            outc = float3(oetfSRGB(tm.r), oetfSRGB(tm.g), oetfSRGB(tm.b));
        }
    } else if (sdrBoost && edrOut) {
        float3 rgbP3 = spSourceToP3(gamut, lin);
        float yEnc = dot(rgbEnc, gamut2020 ? float3(0.2627, 0.6780, 0.0593)
                                           : float3(0.2126, 0.7152, 0.0722));
        outc = oetfSRGBExt(max(rgbP3 * spXdrGain2(yEnc, UV(31), is10bit), float3(0.0, 0.0, 0.0)));
    } else {
        outc = rgbEnc;
    }

    if (UV(20) != 0.0 || UV(21) != 1.0 || UV(22) != 1.0 || UV(23) != 1.0) {
        outc += UV(20);
        outc = (outc - 0.5) * UV(21) + 0.5;
        float luma = dot(outc, float3(0.2126, 0.7152, 0.0722));
        outc = lerp(float3(luma, luma, luma), outc, UV(22));
        outc = pow(max(outc, 0.0), 1.0 / max(UV(23), 0.1));
    }

    if ((hasHdr || sdrBoost) && edrOut) {
        outc = max(outc, float3(0.0, 0.0, 0.0));
    } else {
        outc = clamp(outc, 0.0, 1.0);
    }

    }

    if (UV(16) > 0 && UV(14) > 0 && UV(15) > 0) {
        float2 vp = i.position.xy;
        float2 subUv = (vp - float2(UV(12), UV(13))) / float2(UV(14), UV(15));
        if (subUv.x >= 0.0 && subUv.x <= 1.0 && subUv.y >= 0.0 && subUv.y <= 1.0) {
            float4 sub = subTex.Sample(s, subUv);
            if (sub.a > 0.0) {
                float3 col = clamp(sub.rgb / sub.a, 0.0, 1.0);
                float3 linSub = float3(eotfSRGB(col.r), eotfSRGB(col.g), eotfSRGB(col.b));
                float3 enc;
                if (edrOut) {
                    enc = oetfSRGBExt(max(mul(linSub, m709ToP3), float3(0.0, 0.0, 0.0)));
                } else if (!hasHdr && !doviIPT) {
                    enc = oetfSdrTier(transfer, clamp(sp709ToSource(gamut, linSub), 0.0, 1.0));
                } else {
                    enc = col;
                }
                outc = outc * (1.0 - sub.a) + enc * sub.a;
            }
        }
    }
    return float4(spEncodeForSwapChain(outc, edrOut, hasHdr || doviIPT, gamut, transfer), 1.0);
}
