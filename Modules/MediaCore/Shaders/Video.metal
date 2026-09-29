#include <metal_stdlib>
using namespace metal;

struct Uniforms {
    float uvals[32];
};
static_assert(sizeof(Uniforms) == 128, "CPU Uniforms size mismatch");
static_assert(alignof(Uniforms) == 4, "CPU Uniforms alignment mismatch");
static_assert(__builtin_offsetof(Uniforms, uvals) == 0,
              "CPU Uniforms field offset mismatch");

struct DoviUniforms {
    float v[232];
};
static_assert(sizeof(DoviUniforms) == 928, "CPU DoviUniforms size mismatch");
static_assert(alignof(DoviUniforms) == 4,
              "CPU DoviUniforms alignment mismatch");
static_assert(__builtin_offsetof(DoviUniforms, v) == 0,
              "CPU DoviUniforms field offset mismatch");
struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

constant int fcPrimaries  [[function_constant(0)]];
constant int fcTransfer   [[function_constant(1)]]; // 0=709/2020 SDR 1=PQ 2=HLG 3=linear 4=sRGB 5=γ2.2 6=γ2.8 7=ST428(γ2.6)
constant int fcBits       [[function_constant(2)]];
constant int fcRange      [[function_constant(3)]]; // 1=full
constant int fcHdr        [[function_constant(4)]];
constant int fcDovi       [[function_constant(5)]];
constant int fcOutputMode [[function_constant(6)]];
constant int fcSourceGamut [[function_constant(7)]];
constant int fcPlanar     [[function_constant(8)]];
constant int fcSdrBoost   [[function_constant(9)]];
// Metal API validation treats a function constant as required unless its own
// is_function_constant_defined guards it, so the generic pipeline (no values)
// fails under Xcode's default validation when only fcPrimaries is checked.
// Specialized pipelines always set all ten constants together.
constant bool kSpec =
    is_function_constant_defined(fcPrimaries) &&
    is_function_constant_defined(fcTransfer) &&
    is_function_constant_defined(fcBits) &&
    is_function_constant_defined(fcRange) &&
    is_function_constant_defined(fcHdr) &&
    is_function_constant_defined(fcDovi) &&
    is_function_constant_defined(fcOutputMode) &&
    is_function_constant_defined(fcSourceGamut) &&
    is_function_constant_defined(fcPlanar) &&
    is_function_constant_defined(fcSdrBoost);

#pragma mark - Full-screen triangle

vertex VertexOut videoVertex(uint vid [[vertex_id]], constant Uniforms &u [[buffer(0)]]) {
    float2 pos = float2(float((vid << 1) & 2), float(vid & 2)); // 0..2
    pos = pos * 2.0 - 1.0;

    float sar = (u.uvals[10] > 0.01) ? u.uvals[10] : 1.0;
    float aspectV = (u.uvals[0] > 0 && u.uvals[1] > 0) ? (u.uvals[0] * sar) / u.uvals[1] : 16.0 / 9.0;

    float forced = u.uvals[27];
    if (forced > 0.01) aspectV = forced;
    float aspectP = (u.uvals[2] > 0 && u.uvals[3] > 0) ? u.uvals[2] / u.uvals[3] : aspectV;

    int mode = int(u.uvals[17]);
    int rot = int(u.uvals[18]);
    int mirror = int(u.uvals[19]);

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

    VertexOut out;
    out.position = float4(pos, 0.0, 1.0);
    out.uv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    return out;
}

#pragma mark - Color math

float eotfSRGB(float c);
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
float oetfSRGB(float c);

float3 oetfSdrTier(int transfer, float3 l) {
    if (transfer == 4) return float3(oetfSRGB(l.r), oetfSRGB(l.g), oetfSRGB(l.b));
    if (transfer == 3) return l;
    const float g = spSdrTierGamma(transfer);
    return min(pow(max(l, float3(0.0)), float3(1.0 / g)), 16.0 * l);
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
static float3 spDoviHpeLmsToBt2020(float3 lms) {
    return float3(
         3.0644187900 * lms.x - 2.1659767600 * lms.y + 0.1015581800 * lms.z,
        -0.6561210800 * lms.x + 1.7855411800 * lms.y - 0.1294374900 * lms.z,
         0.0173632100 * lms.x - 0.0472515400 * lms.y + 1.0300425300 * lms.z);
}

float eotfSRGB(float c) {
    return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float oetfSRGB(float c) {
    return (c <= 0.0031308) ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float3 oetfSRGBExt(float3 c) {
    return select(1.055 * pow(max(c, float3(0.0)), float3(1.0 / 2.4)) - 0.055,
                  12.92 * c,
                  c <= 0.0031308);
}

constant float3x3 m2020ToP3 = float3x3(
    float3(1.343578, -0.065297,  0.002822),
    float3(-0.282180, 1.075788, -0.019598),
    float3(-0.061399, -0.010490, 1.016777));

constant float3x3 m2020To709 = float3x3(
    float3(1.660491, -0.124550, -0.018151),
    float3(-0.587641, 1.132900, -0.100579),
    float3(-0.072850, -0.008349, 1.118730));

constant float3x3 m709ToP3 = float3x3(
    float3(0.822462, 0.033194, 0.017083),
    float3(0.177538, 0.966806, 0.072397),
    float3(0.000000, 0.000000, 0.910520));

constant float3x3 mSMPTECTo709 = float3x3(
    float3(0.939542, 0.017772, -0.001622),
    float3(0.050181, 0.965793, -0.004370),
    float3(0.010277, 0.016435,  1.005991));
constant float3x3 mEBUTo709 = float3x3(
    float3(1.044043, 0.000000, 0.000000),
    float3(-0.044043, 1.000000, 0.011793),
    float3(0.000000, 0.000000, 0.988207));

constant float3x3 mP3To709 = float3x3(
    float3(1.224940, -0.042057, -0.019638),
    float3(-0.224940, 1.042057, -0.078636),
    float3(0.000000, 0.000000, 1.098274));

constant float3x3 mDCIP3To709 = float3x3(
    float3(1.157516, -0.041500, -0.018050),
    float3(-0.154962, 1.045568, -0.078578),
    float3(-0.002554, -0.004068, 1.096628));

constant float3x3 m709To2020 = float3x3(
    float3(0.627404, 0.069097, 0.016391),
    float3(0.329283, 0.919540, 0.088013),
    float3(0.043313, 0.011362, 0.895595));
constant float3x3 m709ToSMPTEC = float3x3(
    float3(1.065379, -0.019633, 0.001632),
    float3(-0.055401, 1.036363, 0.004412),
    float3(-0.009978, -0.016731, 0.993956));
constant float3x3 m709ToEBU = float3x3(
    float3(0.957815, 0.000000, 0.000000),
    float3(0.042185, 1.000000, -0.011934),
    float3(0.000000, 0.000000, 1.011934));
constant float3x3 m709ToDCIP3 = float3x3(
    float3(0.868580, 0.034540, 0.016771),
    float3(0.128919, 0.961811, 0.071040),
    float3(0.002501, 0.003648, 0.912189));

float3 spSourceTo709(int gamut, float3 lin) {
    if (gamut == 1) return m2020To709 * lin;
    if (gamut == 2) return mSMPTECTo709 * lin;
    if (gamut == 3) return mEBUTo709 * lin;
    if (gamut == 4) return mP3To709 * lin;
    if (gamut == 5) return mDCIP3To709 * lin;
    return lin;
}

float3 spSourceToP3(int gamut, float3 lin) {
    if (gamut == 1) return m2020ToP3 * lin;
    if (gamut == 4) return lin;
    return m709ToP3 * spSourceTo709(gamut, lin);
}

float3 sp709ToSource(int gamut, float3 lin) {
    if (gamut == 1) return m709To2020 * lin;
    if (gamut == 2) return m709ToSMPTEC * lin;
    if (gamut == 3) return m709ToEBU * lin;
    if (gamut == 4) return m709ToP3 * lin;
    if (gamut == 5) return m709ToDCIP3 * lin;
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
    float g = mix(1.0, exp(lnG), smoothstep(0.35, 0.85, y));
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

float doviReshapeComp(int c, float3 s, constant float *v) {
    int C = 28 + c * 44;
    int np = int(v[C]);
    float x = s[c];
    if (np <= 0) return x;
    int p = 0;
    for (int i = 1; i < np; i++) {
        if (x >= v[C + 1 + i]) p = i;
    }
    int P = C + 12 + p * 4;
    if (v[P] < 0.5) {
        return v[P + 1] + v[P + 2] * x + v[P + 3] * x * x;
    }

    int M = 160 + c * 24;
    int order = int(v[M]);
    float t[7] = {s.x, s.y, s.z, s.x * s.y, s.x * s.z, s.y * s.z, s.x * s.y * s.z};
    float r = v[M + 1];
    for (int o = 0; o < order; o++) {
        for (int j = 0; j < 7; j++) {
            float tp = t[j];
            for (int k = 0; k < o; k++) tp *= t[j];
            r += v[M + 2 + o * 7 + j] * tp;
        }
    }
    return r;
}

#pragma mark - Fragments

fragment float4 videoFragment(VertexOut in [[stage_in]],
                              texture2d<float> yTex [[texture(0)]],
                              texture2d<float> uvTex [[texture(1)]],
                              texture2d<float> subTex [[texture(2)]],
                              texture2d<float> vTex [[texture(3)]],
                              constant Uniforms &u [[buffer(0)]],
                              constant DoviUniforms &dv [[buffer(1)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);

    const int  primaries = kSpec ? fcPrimaries : int(u.uvals[4]);
    const int  transfer  = kSpec ? fcTransfer : int(u.uvals[5]);
    const bool is10bit   = (kSpec ? fcBits : int(u.uvals[7])) == 10;
    const bool fullRange = (kSpec ? fcRange : int(u.uvals[6])) == 1;
    const bool hasHdr    = (kSpec ? fcHdr : (u.uvals[8] > 0.5 ? 1 : 0)) == 1;
    const bool doviIPT   = (kSpec ? fcDovi : (u.uvals[11] > 0.5 ? 1 : 0)) == 1;
    const bool edrOut    = (kSpec ? fcOutputMode : int(u.uvals[25])) == 1;
    const int  gamut     = kSpec ? fcSourceGamut : int(u.uvals[26]);
    const bool gamut2020 = gamut == 1;
    const bool planar    = (kSpec ? fcPlanar : int(u.uvals[30])) == 1;
    const bool sdrBoost  = kSpec ? (fcSdrBoost == 1) : (u.uvals[31] >= 1.0);

    bool inVideo = (in.uv.x >= 0.0 && in.uv.x <= 1.0 && in.uv.y >= 0.0 && in.uv.y <= 1.0);
    float3 out = float3(0.0);
    if (inVideo) {

    float2 suv = in.uv;
    {
        float cw = u.uvals[28], ch = u.uvals[29];
        if (cw > 0.01 && ch > 0.01) {
            suv = float2(0.5 + (in.uv.x - 0.5) * cw,
                         0.5 + (in.uv.y - 0.5) * ch);
        }
    }

    float Y, Cb, Cr;
    if (planar && is10bit) {

        Y  = float(uint(yTex.sample(s, suv).r * 65535.0 + 0.0078125)) / 1023.0;
        Cb = float(uint(uvTex.sample(s, suv).r * 65535.0 + 0.0078125)) / 1023.0;
        Cr = float(uint(vTex.sample(s, suv).r * 65535.0 + 0.0078125)) / 1023.0;
    } else if (is10bit) {

        uint y16 = uint(yTex.sample(s, suv).r * 65535.0 + 0.5);
        float2 uv = uvTex.sample(s, suv).rg;
        uint u16 = uint(uv.x * 65535.0 + 0.5);
        uint v16 = uint(uv.y * 65535.0 + 0.5);
        Y  = float(y16 >> 6) / 1023.0;
        Cb = float(u16 >> 6) / 1023.0;
        Cr = float(v16 >> 6) / 1023.0;
    } else if (planar) {

        Y  = yTex.sample(s, suv).r;
        Cb = uvTex.sample(s, suv).r;
        Cr = vTex.sample(s, suv).r;
    } else {
        // NV12
        Y  = yTex.sample(s, suv).r;
        Cb = uvTex.sample(s, suv).r;
        Cr = uvTex.sample(s, suv).g;
    }

    bool limitedRange = doviIPT ? (dv.v[23] < 0.5) : !fullRange;
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

    float3 lin = float3(0.0);
    float3 rgbEnc = float3(0.0);

    const bool sdrPassthrough = !doviIPT && !hasHdr && !edrOut;
    if (doviIPT) {

        float3 sig = float3(Y, Cb, Cr);
        if (dv.v[24] > 0.5) {
            sig = float3(doviReshapeComp(0, sig, dv.v),
                         doviReshapeComp(1, sig, dv.v),
                         doviReshapeComp(2, sig, dv.v));
            sig = clamp(sig, 0.0, 1.0);
        }
        float3 ipt = sig - float3(dv.v[9], dv.v[10], dv.v[11]);
        float3 lmsPq = float3(
            dv.v[0] * ipt.x + dv.v[1] * ipt.y + dv.v[2] * ipt.z,
            dv.v[3] * ipt.x + dv.v[4] * ipt.y + dv.v[5] * ipt.z,
            dv.v[6] * ipt.x + dv.v[7] * ipt.y + dv.v[8] * ipt.z);
        lmsPq = clamp(lmsPq, 0.0, 1.0);
        float3 lms = float3(eotfPQ(lmsPq.x), eotfPQ(lmsPq.y), eotfPQ(lmsPq.z));
        lms = float3(
            dv.v[12] * lms.x + dv.v[13] * lms.y + dv.v[14] * lms.z,
            dv.v[15] * lms.x + dv.v[16] * lms.y + dv.v[17] * lms.z,
            dv.v[18] * lms.x + dv.v[19] * lms.y + dv.v[20] * lms.z);

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

    float contentPeak = u.uvals[9];
    if (doviIPT && dv.v[22] > 0.0) {
        contentPeak = max(eotfPQ(dv.v[22]) * 10000.0, 203.0);
    }
    if (hasHdr) {

        if (edrOut) {

            float3 nits;
            if (contentPeak > u.uvals[24]) {
                nits = toneMap(lin, contentPeak, max(u.uvals[24], 1.0)) * u.uvals[24];
            } else {
                nits = lin * 10000.0;
            }
            nits = max(spSourceToP3(gamut, nits), float3(0.0));
            out = oetfSRGBExt(nits / 100.0);
        } else {

            float3 tm = toneMap(lin, max(contentPeak, 203.0), 203.0);
            tm = clamp(spSourceTo709(gamut, tm), 0.0, 1.0);
            out = float3(oetfSRGB(tm.r), oetfSRGB(tm.g), oetfSRGB(tm.b));
        }
    } else if (sdrBoost && edrOut) {

        float3 rgbP3 = spSourceToP3(gamut, lin);
        float yEnc = dot(rgbEnc, gamut2020 ? float3(0.2627, 0.6780, 0.0593)
                                           : float3(0.2126, 0.7152, 0.0722));
        out = oetfSRGBExt(max(rgbP3 * spXdrGain2(yEnc, u.uvals[31], is10bit), float3(0.0)));
    } else {

        out = rgbEnc;
    }

    if (u.uvals[20] != 0.0 || u.uvals[21] != 1.0 || u.uvals[22] != 1.0 || u.uvals[23] != 1.0) {
        out += float3(u.uvals[20]);
        out = (out - 0.5) * u.uvals[21] + 0.5;
        float luma = dot(out, float3(0.2126, 0.7152, 0.0722));
        out = mix(float3(luma), out, u.uvals[22]);
        out = pow(max(out, 0.0), float3(1.0 / max(u.uvals[23], 0.1)));
    }

    if ((hasHdr || sdrBoost) && edrOut) {
        out = max(out, float3(0.0));
    } else {
        out = clamp(out, 0.0, 1.0);
    }

    }

    if (u.uvals[16] > 0 && u.uvals[14] > 0 && u.uvals[15] > 0) {

        float2 vp = in.position.xy;
        float2 subUv = (vp - float2(u.uvals[12], u.uvals[13])) / float2(u.uvals[14], u.uvals[15]);
        if (subUv.x >= 0.0 && subUv.x <= 1.0 && subUv.y >= 0.0 && subUv.y <= 1.0) {
            float4 sub = subTex.sample(s, subUv);

            if (sub.a > 0.0) {
                float3 col = clamp(sub.rgb / sub.a, 0.0, 1.0);
                float3 linSub = float3(eotfSRGB(col.r), eotfSRGB(col.g), eotfSRGB(col.b));
                float3 enc;
                if (edrOut) {
                    enc = oetfSRGBExt(max(m709ToP3 * linSub, float3(0.0)));
                } else if (!hasHdr && !doviIPT) {
                    enc = oetfSdrTier(transfer, clamp(sp709ToSource(gamut, linSub), 0.0, 1.0));
                } else {
                    enc = col;
                }
                out = out * (1.0 - sub.a) + enc * sub.a;
            }
        }
    }
    return float4(out, 1.0);
}

#pragma mark - Window depth effect

struct DragFxUniforms {
    float v[16];
};
static_assert(sizeof(DragFxUniforms) == 64, "CPU DragFxUniforms size mismatch");
static_assert(alignof(DragFxUniforms) == 4, "CPU DragFxUniforms alignment mismatch");

struct DragFxVertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex DragFxVertexOut dragFxVertex(uint vid [[vertex_id]]) {
    float2 pos = float2(vid == 1 ? 3.0 : -1.0, vid == 2 ? 3.0 : -1.0);
    DragFxVertexOut o;
    o.position = float4(pos, 0.0, 1.0);
    o.uv = float2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);
    return o;
}

fragment float4 dragFxDownsample(DragFxVertexOut in [[stage_in]],
                                 texture2d<float> src [[texture(0)]],
                                 constant float2 &texel [[buffer(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float2 uv = in.uv;
    float3 a = src.sample(s, uv + float2(-2.0, -2.0) * texel).rgb;
    float3 b = src.sample(s, uv + float2( 0.0, -2.0) * texel).rgb;
    float3 c = src.sample(s, uv + float2( 2.0, -2.0) * texel).rgb;
    float3 d = src.sample(s, uv + float2(-2.0,  0.0) * texel).rgb;
    float3 e = src.sample(s, uv).rgb;
    float3 f = src.sample(s, uv + float2( 2.0,  0.0) * texel).rgb;
    float3 g = src.sample(s, uv + float2(-2.0,  2.0) * texel).rgb;
    float3 h = src.sample(s, uv + float2( 0.0,  2.0) * texel).rgb;
    float3 i = src.sample(s, uv + float2( 2.0,  2.0) * texel).rgb;
    float3 j = src.sample(s, uv + float2(-1.0, -1.0) * texel).rgb;
    float3 k = src.sample(s, uv + float2( 1.0, -1.0) * texel).rgb;
    float3 l = src.sample(s, uv + float2(-1.0,  1.0) * texel).rgb;
    float3 m = src.sample(s, uv + float2( 1.0,  1.0) * texel).rgb;
    float3 o = e * 0.125
             + (a + c + g + i) * 0.03125
             + (b + d + f + h) * 0.0625
             + (j + k + l + m) * 0.125;
    return float4(o, 1.0);
}

static inline float3 spDragFxTent(texture2d<float> t, sampler s, float2 uv) {
    float2 texel = 1.0 / float2(t.get_width(), t.get_height());
    float3 c = t.sample(s, uv + float2(-0.5, -0.5) * texel).rgb
             + t.sample(s, uv + float2( 0.5, -0.5) * texel).rgb
             + t.sample(s, uv + float2(-0.5,  0.5) * texel).rgb
             + t.sample(s, uv + float2( 0.5,  0.5) * texel).rgb;
    return c * 0.25;
}

static inline float3 spDragFxLevel(int lvl, array<texture2d<float>, 7> lv,
                                   sampler s, float2 uv) {
    switch (lvl) {
        case 0:  return lv[0].sample(s, uv).rgb;
        case 1:  return spDragFxTent(lv[1], s, uv);
        case 2:  return spDragFxTent(lv[2], s, uv);
        case 3:  return spDragFxTent(lv[3], s, uv);
        case 4:  return spDragFxTent(lv[4], s, uv);
        case 5:  return spDragFxTent(lv[5], s, uv);
        default: return spDragFxTent(lv[6], s, uv);
    }
}

static inline float3 spDragFxSample(int l0, float f, int maxLevel,
                                    array<texture2d<float>, 7> levels,
                                    sampler s, float2 uv) {
    float3 c = spDragFxLevel(l0, levels, s, uv);
    if (f > 0.001 && l0 < maxLevel) {
        c = mix(c, spDragFxLevel(l0 + 1, levels, s, uv), f);
    }
    return c;
}

fragment float4 dragFxComposite(DragFxVertexOut in [[stage_in]],
                                array<texture2d<float>, 7> levels [[texture(0)]],
                                constant DragFxUniforms &u [[buffer(0)]],
                                constant float &outputCeiling [[buffer(1)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    const float2 vp = float2(u.v[0], u.v[1]);
    const float2 anchor = clamp(float2(u.v[2], u.v[3]), float2(0.0), vp);
    const float strength = clamp(u.v[4], 0.0, 1.0);
    const float colorStrength = clamp(u.v[9], 0.0, 1.0);
    const float maxLevel = clamp(u.v[7], 0.0, 6.0);
    const float2 p = in.position.xy;

    float R = max(length(max(anchor, vp - anchor)), 1.0);
    float d = length(p - anchor) / R;
    float radial = smoothstep(u.v[8], 1.0, d);

    float t = mix(clamp(u.v[13], 0.0, 1.0), 1.0, radial);
    float k = colorStrength * t;
    float2 uv = p / vp;

    const float shortSide = min(vp.x, vp.y);
    const float refractionPx = shortSide * max(u.v[14], 0.0);
    const float2 geometryAnchor = clamp(anchor, float2(0.5), vp - 0.5);
    const float coreRadius = shortSide * clamp(u.v[12], 0.0, 0.45);
    const float geometryRadius = length(p - geometryAnchor);
    const float core = smoothstep(coreRadius, coreRadius + shortSide * 0.20, geometryRadius);
    const float2 q = uv * 2.0 - 1.0;
    const float2 q2 = q * q;
    const float2 surfaceSlope = -q * q2 * (0.8 + 0.2 * q2.yx);
    const float2 inward = surfaceSlope * core * strength;
    uv += inward * refractionPx / vp;

    float nearLevel = clamp(u.v[5], 0.0, maxLevel);
    float farLevel = clamp(u.v[6], nearLevel, maxLevel);
    float L = strength * mix(nearLevel, farLevel, radial);
    int l0 = int(floor(L));
    float f = L - float(l0);
    float3 c = spDragFxSample(l0, f, int(maxLevel), levels, s, uv);

    const float dispersionPx = min(4.0 + 0.5 * exp2(L), 20.0) * max(u.v[15], 0.0);
    const float2 dispersionDirection = inward;
    const float3 lumaWeights = float3(0.2126, 0.7152, 0.0722);
    float3 chromaDelta = float3(0.0);
    if (dispersionPx > 0.0 && any(dispersionDirection != float2(0.0))) {
        float2 rgbPx = dispersionDirection * dispersionPx;

        float2 available = max(min(uv * vp - 0.5, (1.0 - uv) * vp - 0.5), float2(0.0));
        float2 ratio = abs(rgbPx) / max(available, float2(0.0001));
        rgbPx /= 1.0 + length(ratio);
        rgbPx = clamp(rgbPx, -available, available);
        float2 rgbOffset = rgbPx / vp;
        float chromaLevel = max(L - 1.5, 0.0);
        int chromaL0 = int(floor(chromaLevel));
        float chromaF = chromaLevel - float(chromaL0);
        float3 detail = spDragFxSample(chromaL0, chromaF, int(maxLevel), levels, s, uv);
        float red = spDragFxSample(chromaL0, chromaF, int(maxLevel), levels, s, uv + rgbOffset).r;
        float blue = spDragFxSample(chromaL0, chromaF, int(maxLevel), levels, s, uv - rgbOffset).b;
        chromaDelta = float3(red - detail.r, 0.0, blue - detail.b);
        chromaDelta -= dot(chromaDelta, lumaWeights);
    }

    float luma = dot(c, lumaWeights);
    c = mix(c, float3(luma), clamp(u.v[11] * k, 0.0, 1.0));

    float dim = 1.0 - clamp(u.v[10] * k, 0.0, 1.0);
    float3 lower = select(float3(0.9), max(c, float3(0.0)) / max(-chromaDelta, float3(0.00001)),
                          chromaDelta < float3(0.0));
    float3 upper = select(float3(0.9), max(outputCeiling / max(dim, 0.00001) - c, float3(0.0))
                          / max(chromaDelta, float3(0.00001)), chromaDelta > float3(0.0));
    float3 allowance = min(lower, upper);
    float chromaGain = min(allowance.r, min(allowance.g, allowance.b));
    c += chromaDelta * max(chromaGain, 0.0);
    c *= dim;
    return float4(c, 1.0);
}

#pragma mark - Dolby Vision thumbnail conversion

static float oetfPQ(float lin) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float ym = pow(max(lin, 0.0), m1);
    return pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2);
}

kernel void doviThumbConvert(texture2d<float, access::read> yTex [[texture(0)]],
                             texture2d<float, access::read> uvTex [[texture(1)]],
                             constant DoviUniforms &dv [[buffer(0)]],
                             constant int4 &meta [[buffer(1)]],
                             device ushort4 *outPix [[buffer(2)]],
                             uint2 gid [[thread_position_in_grid]]) {
    const uint W = uint(meta.y), H = uint(meta.z);
    if (gid.x >= W || gid.y >= H) return;
    const bool is10bit = meta.x == 10;
    const uint2 cg = uint2(min(gid.x / 2, uvTex.get_width() - 1),
                           min(gid.y / 2, uvTex.get_height() - 1));
    float Y, Cb, Cr;
    if (is10bit) {
        uint y16 = uint(yTex.read(gid).r * 65535.0 + 0.5);
        float2 uv = uvTex.read(cg).rg;
        uint u16 = uint(uv.x * 65535.0 + 0.5);
        uint v16 = uint(uv.y * 65535.0 + 0.5);
        Y  = float(y16 >> 6) / 1023.0;
        Cb = float(u16 >> 6) / 1023.0;
        Cr = float(v16 >> 6) / 1023.0;
    } else {
        Y  = yTex.read(gid).r;
        float2 uv = uvTex.read(cg).rg;
        Cb = uv.x;
        Cr = uv.y;
    }
    if (dv.v[23] < 0.5) { // RPU signal_full_range=0：limited → full
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
    float3 sig = clamp(float3(Y, Cb, Cr), 0.0, 1.0);
    if (dv.v[24] > 0.5) {
        sig = float3(doviReshapeComp(0, sig, dv.v),
                     doviReshapeComp(1, sig, dv.v),
                     doviReshapeComp(2, sig, dv.v));
        sig = clamp(sig, 0.0, 1.0);
    }
    float3 ipt = sig - float3(dv.v[9], dv.v[10], dv.v[11]);
    float3 lmsPq = float3(
        dv.v[0] * ipt.x + dv.v[1] * ipt.y + dv.v[2] * ipt.z,
        dv.v[3] * ipt.x + dv.v[4] * ipt.y + dv.v[5] * ipt.z,
        dv.v[6] * ipt.x + dv.v[7] * ipt.y + dv.v[8] * ipt.z);
    lmsPq = clamp(lmsPq, 0.0, 1.0);
    float3 lms = float3(eotfPQ(lmsPq.x), eotfPQ(lmsPq.y), eotfPQ(lmsPq.z));
    lms = float3(
        dv.v[12] * lms.x + dv.v[13] * lms.y + dv.v[14] * lms.z,
        dv.v[15] * lms.x + dv.v[16] * lms.y + dv.v[17] * lms.z,
        dv.v[18] * lms.x + dv.v[19] * lms.y + dv.v[20] * lms.z);
    float3 lin = max(spDoviHpeLmsToBt2020(lms), 0.0);
    float3 pq = clamp(float3(oetfPQ(lin.r), oetfPQ(lin.g), oetfPQ(lin.b)), 0.0, 1.0);
    outPix[gid.y * W + gid.x] = ushort4(ushort(pq.r * 65535.0 + 0.5), ushort(pq.g * 65535.0 + 0.5),
                                        ushort(pq.b * 65535.0 + 0.5), 65535);
}
