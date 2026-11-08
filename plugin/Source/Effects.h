/*
 * Effects — two effect slots, A and B, each one of CHORUS / DUB ECHO /
 * FILTER / SPECTRAL.
 *
 *   SERIES:    dry → A → B → wet
 *   PARALLEL:  dry → A ┐
 *              dry → B ┴ balance (equal-power) → wet
 *
 * FX MIX crossfades dry ↔ wet. Only the two chosen effects run; an effect
 * that comes back into a slot starts clean. The chain sleeps while FX MIX
 * is at zero. (Which parameters the XY pad drives is decided by the caller.)
 */
#pragma once

#include "StretchEngine.h"
#include <vector>

namespace ss
{

struct FxParams
{
    float mix = 0;                       // 0..1
    int typeA = 2, typeB = 1;            // FxType (Params.h): 0 chorus, 1 echo, 2 filter, 3 spectral; -1 = empty
    bool series = true;
    float balance = 0.5f;                // parallel: 0 = all A, 1 = all B
    float chRate = 0.35f, chDepth = 0.5f;
    float echoTimeMs = 375, echoFb = 0.55f, echoTone = 0.45f;
    float cutHz = 1200, res = 0.3f, type = 0;     // type: 0 LP → 0.5 BP → 1 HP
    float blur = 0.6f, shiftSt = 0, thin = 0;
};

class Chorus
{
public:
    void prepare (double sr);
    void reset();
    void process (const float* inL, const float* inR, float* outL, float* outR, int n, float rateHz, float depth) noexcept;
private:
    double sr = 48000, phase = 0;
    std::vector<float> buf[2];
    int mask = 0, w = 0;
    float sDepth = 0.5f;
};

class Filter
{
public:
    void prepare (double sr);
    void reset();
    void process (const float* inL, const float* inR, float* outL, float* outR, int n, float cutHz, float res, float type) noexcept;
private:
    double sr = 48000;
    float ic1[2] {}, ic2[2] {};
    float logCut = 7.0f, sRes = 0.3f, sType = 0;
    bool first = true;
};

class DubEcho
{
public:
    void prepare (double sr);
    void reset();
    void process (const float* inL, const float* inR, float* outL, float* outR, int n, float timeMs, float fb, float tone) noexcept;
private:
    double sr = 48000, wowPhase = 0;
    std::vector<float> buf[2];
    int mask = 0, w = 0;
    float delay = -1, sFb = 0.5f, sTone = 0.5f;
    float lp[2] {}, hp[2] {};
};

class Spectral
{
public:
    void prepare (double sr);
    void reset();
    void process (const float* inL, const float* inR, float* outL, float* outR, int n, float blur, float shiftSt, float thin) noexcept;
    int latency() const noexcept { return N; }
private:
    void frame (int ch);
    struct Chan
    {
        std::vector<double> inRing, ola, frameBuf, re, im, ms, phPrev, phAcc, outMag, outW, sRe, sIm, phNow, omega, outPh;
        std::vector<int> peaks;
        int pos = 0;
    };
    double sr = 48000;
    int N = 2048, H = 1025, hop = 512, count = 0;
    RealFFT fft;
    std::vector<double> win, binW;
    Chan ch[2];
    float blur = 0.6f, shift = 0, thin = 0;
    uint32_t frameNo = 0;
};

class FxChain
{
public:
    void prepare (double sampleRate, int maxBlock);
    void reset();
    /** In place. Cheap no-op while mix is (and stays) at zero. */
    void process (float* L, float* R, int n, const FxParams& p) noexcept;

private:
    void run (float* L, float* R, int n, const FxParams& p) noexcept;
    void runOne (int type, const float* inL, const float* inR, float* outL, float* outR, int n, const FxParams& p) noexcept;

    double sr = 48000;
    int maxBlock = 512;
    Chorus chorus;
    Filter filter;
    DubEcho echo;
    Spectral spectral;
    std::vector<float> t[10];
    float sMix = 0, sBal = 0.5f;
    bool sleeping = true;
    bool active[4] {};
};

} // namespace ss
