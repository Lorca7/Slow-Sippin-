#include "Effects.h"
#include <algorithm>

namespace ss
{

static inline double princarg2 (double x) noexcept { return x - kTwoPi * std::round (x / kTwoPi); }

template <class T> static inline T lim (T lo, T hi, T v) noexcept { return v < lo ? lo : v > hi ? hi : v; }

static int nextPow2 (int v) { int p = 1; while (p < v) p <<= 1; return p; }

static inline float readFrac (const std::vector<float>& b, int mask, double pos) noexcept
{
    const double fl = std::floor (pos);
    const int i0 = (int) (int64_t) fl;
    const float f = (float) (pos - fl);
    const float a = b[(size_t) (i0 & mask)], c = b[(size_t) ((i0 + 1) & mask)];
    return a + (c - a) * f;
}

// ================================================================= Chorus

void Chorus::prepare (double s)
{
    sr = s;
    const int size = nextPow2 ((int) (sr * 0.06) + 4);
    for (auto& b : buf) b.assign ((size_t) size, 0.0f);
    mask = size - 1;
    reset();
}

void Chorus::reset()
{
    for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f);
    w = 0;
    phase = 0;
}

void Chorus::process (const float* inL, const float* inR, float* outL, float* outR, int n, float rateHz, float depth) noexcept
{
    const double inc = lim (0.001, 20.0, (double) rateHz) / sr;
    const double msToS = sr / 1000.0;
    for (int i = 0; i < n; ++i)
    {
        sDepth += (lim (0.0f, 1.0f, depth) - sDepth) * 0.0015f;
        buf[0][(size_t) w] = inL[i];
        buf[1][(size_t) w] = inR[i];
        for (int c = 0; c < 2; ++c)
        {
            float acc = 0;
            for (int v = 0; v < 3; ++v)
            {
                const double m = std::sin (kTwoPi * (phase + v / 3.0 + c * 0.25));
                const double dMs = 7.0 + 3.0 * v + sDepth * 9.0 * (0.5 + 0.5 * m);
                acc += readFrac (buf[c], mask, (double) w - dMs * msToS);
            }
            (c == 0 ? outL : outR)[i] = acc * 0.5f;     // between 1/3 (tones) and 1/sqrt 3 (textures)
        }
        w = (w + 1) & mask;
        phase += inc;
        if (phase >= 1.0) phase -= 1.0;
    }
}

// ================================================================= Filter

void Filter::prepare (double s) { sr = s; reset(); }
void Filter::reset() { for (int c = 0; c < 2; ++c) ic1[c] = ic2[c] = 0; first = true; }

void Filter::process (const float* inL, const float* inR, float* outL, float* outR, int n, float cutHz, float res, float type) noexcept
{
    const float tCut = std::log (lim (20.0f, (float) (sr * 0.45), cutHz));
    const float tRes = lim (0.0f, 1.0f, res), tType = lim (0.0f, 1.0f, type);
    if (first) { logCut = tCut; sRes = tRes; sType = tType; first = false; }
    for (int i = 0; i < n; ++i)
    {
        logCut += (tCut - logCut) * 0.0025f;
        sRes += (tRes - sRes) * 0.0025f;
        sType += (tType - sType) * 0.0025f;
        const double g = std::tan (kPi * std::exp ((double) logCut) / sr);
        const double k = 2.0 - 1.9 * sRes;
        const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        for (int c = 0; c < 2; ++c)
        {
            const double x = (c == 0 ? inL : inR)[i];
            const double v3 = x - ic2[c];
            const double v1 = a1 * ic1[c] + a2 * v3;
            const double v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
            ic1[c] = (float) (2.0 * v1 - ic1[c]);
            ic2[c] = (float) (2.0 * v2 - ic2[c]);
            const double lp = v2, bp = k * v1, hp = x - k * v1 - v2;
            const double y = sType < 0.5f ? lp + (bp - lp) * (sType * 2.0) : bp + (hp - bp) * ((sType - 0.5) * 2.0);
            (c == 0 ? outL : outR)[i] = (float) y;
        }
    }
}

// ================================================================ DubEcho

void DubEcho::prepare (double s)
{
    sr = s;
    const int size = nextPow2 ((int) (sr * 2.2) + 8);
    for (auto& b : buf) b.assign ((size_t) size, 0.0f);
    mask = size - 1;
    reset();
}

void DubEcho::reset()
{
    for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f);
    w = 0;
    delay = -1;
    wowPhase = 0;
    for (int c = 0; c < 2; ++c) lp[c] = hp[c] = 0;
}

void DubEcho::process (const float* inL, const float* inR, float* outL, float* outR, int n, float timeMs, float fb, float tone) noexcept
{
    const float target = lim (20.0f, 2000.0f, timeMs) * (float) sr / 1000.0f;
    if (delay < 0) { delay = target; sFb = fb; sTone = tone; }
    const float glide = 1.0f - (float) std::exp (-1.0 / (0.12 * sr));      // tape-style time glide
    const float hpA = 1.0f - (float) std::exp (-kTwoPi * 90.0 / sr);
    for (int i = 0; i < n; ++i)
    {
        delay += (target - delay) * glide;
        sFb += (lim (0.0f, 1.1f, fb) - sFb) * 0.002f;
        sTone += (lim (0.0f, 1.0f, tone) - sTone) * 0.002f;
        const float lpC = 1.0f - std::exp ((float) (-kTwoPi * 250.0 * std::pow (48.0, (double) sTone) / sr));
        wowPhase += 0.45 / sr;
        if (wowPhase >= 1.0) wowPhase -= 1.0;
        const double d = delay + std::sin (kTwoPi * wowPhase) * 0.0012 * sr;

        float y[2], f[2];
        for (int c = 0; c < 2; ++c)
        {
            y[c] = readFrac (buf[c], mask, (double) w - d);
            lp[c] += lpC * (y[c] - lp[c]);
            hp[c] += hpA * (lp[c] - hp[c]);
            f[c] = lp[c] - hp[c];
        }
        buf[0][(size_t) w] = inL[i] + std::tanh (sFb * (0.85f * f[0] + 0.15f * f[1]));
        buf[1][(size_t) w] = inR[i] + std::tanh (sFb * (0.85f * f[1] + 0.15f * f[0]));
        outL[i] = y[0];
        outR[i] = y[1];
        w = (w + 1) & mask;
    }
}

// =============================================================== Spectral

void Spectral::prepare (double s)
{
    sr = s;
    N = sr <= 50000 ? 2048 : sr <= 100000 ? 4096 : 8192;
    H = N / 2 + 1;
    hop = N / 4;
    fft.prepare (N);
    win.resize ((size_t) N);
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5 - 0.5 * std::cos (kTwoPi * i / N);
    binW.resize ((size_t) H);
    for (int k = 0; k < H; ++k) binW[(size_t) k] = kTwoPi * k / N;
    for (auto& c : ch)
    {
        c.inRing.assign ((size_t) N, 0.0);
        c.ola.assign ((size_t) (2 * N), 0.0);
        c.frameBuf.assign ((size_t) N, 0.0);
        for (auto* v : { &c.re, &c.im, &c.ms, &c.phPrev, &c.phAcc, &c.outMag, &c.outW, &c.sRe, &c.sIm, &c.phNow, &c.omega, &c.outPh })
            v->assign ((size_t) H, 0.0);
        c.peaks.assign ((size_t) H, 0);
    }
    reset();
}

void Spectral::reset()
{
    for (auto& c : ch)
    {
        std::fill (c.inRing.begin(), c.inRing.end(), 0.0);
        std::fill (c.ola.begin(), c.ola.end(), 0.0);
        std::fill (c.ms.begin(), c.ms.end(), 0.0);
        std::fill (c.phPrev.begin(), c.phPrev.end(), 0.0);
        std::fill (c.phAcc.begin(), c.phAcc.end(), 0.0);
        c.pos = 0;
    }
    count = 0;
}

void Spectral::frame (int c)
{
    Chan& C = ch[c];
    const int nMask = N - 1;
    for (int i = 0; i < N; ++i)
        C.frameBuf[(size_t) i] = C.inRing[(size_t) ((C.pos + 1 + i) & nMask)] * win[(size_t) i];
    fft.forward (C.frameBuf.data(), C.re.data(), C.im.data());

    const double b = std::pow ((double) blur, 0.6) * 0.997;      // magnitude smoothing per hop
    const double r = std::pow (2.0, (double) shift / 12.0);
    const bool identity = std::abs (r - 1.0) < 1e-6;
    double maxMs = 0;
    int maxK = 0;
    for (int k = 0; k < H; ++k)
    {
        const double re = C.re[(size_t) k], im = C.im[(size_t) k];
        const double m = std::sqrt (re * re + im * im);
        const double ph = std::atan2 (im, re);
        C.omega[(size_t) k] = binW[(size_t) k] + princarg2 (ph - C.phPrev[(size_t) k] - binW[(size_t) k] * hop) / hop;
        C.phPrev[(size_t) k] = ph;
        C.phNow[(size_t) k] = ph;
        C.ms[(size_t) k] = b * C.ms[(size_t) k] + (1.0 - b) * m;
        if (C.ms[(size_t) k] > maxMs) { maxMs = C.ms[(size_t) k]; maxK = k; }
    }

    if (identity)
    {
        std::copy (C.ms.begin(), C.ms.end(), C.outMag.begin());
        std::copy (C.phNow.begin(), C.phNow.end(), C.outPh.begin());
    }
    else
    {
        // move each spectral peak with its surrounding bins as one block (keeps partials intact)
        std::fill (C.outMag.begin(), C.outMag.end(), 0.0);
        for (int k = 0; k < H; ++k) C.outPh[(size_t) k] = C.phAcc[(size_t) k] + binW[(size_t) k] * hop;
        const double* ms = C.ms.data();
        int np = 0;
        for (int k = 2; k < H - 2; ++k)
            if (ms[k] > maxMs * 1e-4 && ms[k] > ms[k - 1] && ms[k] >= ms[k + 1] && ms[k] > ms[k - 2] && ms[k] >= ms[k + 2])
                C.peaks[(size_t) np++] = k;
        if (np == 0) C.peaks[(size_t) np++] = maxK;
        int lo = 0;
        for (int i = 0; i < np; ++i)
        {
            const int kp = C.peaks[(size_t) i];
            int hi = H;
            if (i < np - 1)
            {
                const int kn = C.peaks[(size_t) i + 1];
                int mi = kp + 1;
                for (int k = kp + 2; k < kn; ++k) if (ms[k] < ms[mi]) mi = k;
                hi = mi;
            }
            const int d = (int) std::lround (kp * (r - 1.0));
            const int jp = kp + d;
            if (jp >= 0 && jp < H)
            {
                const double phs = C.phAcc[(size_t) jp] + r * C.omega[(size_t) kp] * hop;
                const int k0 = std::max (lo, -d), k1 = std::min (hi, H - d);
                for (int k = k0; k < k1; ++k)
                {
                    C.outMag[(size_t) (k + d)] += ms[k];
                    C.outPh[(size_t) (k + d)] = phs + C.phNow[(size_t) k] - C.phNow[(size_t) kp];
                }
            }
            lo = hi;
        }
    }

    double mx = 0;
    for (int k = 0; k < H; ++k) mx = std::max (mx, C.outMag[(size_t) k]);
    const double thr = (double) thin * (double) thin * 0.35 * mx;
    Rng rng (hash32 (frameNo, (uint32_t) c + 11u));
    const double jit = (double) blur * (double) blur * kPi;
    for (int k = 0; k < H; ++k)
    {
        double mag = C.outMag[(size_t) k];
        if (mag < thr) mag = 0;
        C.phAcc[(size_t) k] = princarg2 (C.outPh[(size_t) k]);
        const double ph = C.outPh[(size_t) k] + jit * (2.0 * rng.next() - 1.0);
        C.sRe[(size_t) k] = mag * std::cos (ph);
        C.sIm[(size_t) k] = mag * std::sin (ph);
    }
    C.sIm[0] = 0;
    C.sIm[(size_t) H - 1] = 0;
    fft.inverse (C.sRe.data(), C.sIm.data(), C.frameBuf.data());
    const int oMask = 2 * N - 1;
    const double scale = 1.0 / 1.5;      // Hann² overlap-add at 75 % overlap
    for (int i = 0; i < N; ++i)
        C.ola[(size_t) ((C.pos + 1 + i) & oMask)] += C.frameBuf[(size_t) i] * win[(size_t) i] * scale;
}

void Spectral::process (const float* inL, const float* inR, float* outL, float* outR, int n, float b, float s, float t) noexcept
{
    blur = lim (0.0f, 1.0f, b);
    shift = lim (-24.0f, 24.0f, s);
    thin = lim (0.0f, 1.0f, t);
    const int nMask = N - 1, oMask = 2 * N - 1;
    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < 2; ++c)
        {
            Chan& C = ch[c];
            const int o = C.pos & oMask;
            (c == 0 ? outL : outR)[i] = (float) C.ola[(size_t) o];
            C.ola[(size_t) o] = 0;
        }
        for (int c = 0; c < 2; ++c)
        {
            Chan& C = ch[c];
            C.pos = (C.pos + 1) & 0x3fffffff;
            C.inRing[(size_t) (C.pos & nMask)] = (c == 0 ? inL : inR)[i];
        }
        if (++count >= hop)
        {
            count = 0;
            frame (0);
            frame (1);
            ++frameNo;
        }
    }
}

// ================================================================ FxChain

void FxChain::prepare (double sampleRate, int block)
{
    sr = sampleRate;
    maxBlock = std::max (32, block);
    chorus.prepare (sr);
    filter.prepare (sr);
    echo.prepare (sr);
    spectral.prepare (sr);
    for (auto& b : t) b.assign ((size_t) maxBlock, 0.0f);
    sleeping = true;
    sMix = 0;
}

void FxChain::reset()
{
    chorus.reset();
    filter.reset();
    echo.reset();
    spectral.reset();
}

void FxChain::process (float* L, float* R, int n, const FxParams& p) noexcept
{
    if (sleeping)
    {
        if (p.mix <= 0.0f) return;
        reset();
        sleeping = false;
        sMix = 0;
        sX = p.x;
        sY = p.y;
    }
    for (int off = 0; off < n; off += maxBlock)
        run (L + off, R + off, std::min (maxBlock, n - off), p);
    if (p.mix <= 0.0f && sMix < 1.0e-4f) sleeping = true;
}

void FxChain::run (float* L, float* R, int n, const FxParams& p) noexcept
{
    float* dL = t[0].data(); float* dR = t[1].data();
    float* cL = t[2].data(); float* cR = t[3].data();
    float* eL = t[4].data(); float* eR = t[5].data();
    float* fL = t[6].data(); float* fR = t[7].data();
    float* sL = t[8].data(); float* sR = t[9].data();
    std::copy (L, L + n, dL);
    std::copy (R, R + n, dR);
    chorus.process (dL, dR, cL, cR, n, p.chRate, p.chDepth);
    echo.process (dL, dR, eL, eR, n, p.echoTimeMs, p.echoFb, p.echoTone);
    filter.process (dL, dR, fL, fR, n, p.cutHz, p.res, p.type);
    spectral.process (dL, dR, sL, sR, n, p.blur, p.shiftSt, p.thin);

    const float k = 1.0f - std::exp ((float) (-1.0 / (0.03 * sr)));
    const float tm = lim (0.0f, 1.0f, p.mix), tx = lim (0.0f, 1.0f, p.x), ty = lim (0.0f, 1.0f, p.y);
    for (int i = 0; i < n; ++i)
    {
        sMix += (tm - sMix) * k;
        sX += (tx - sX) * k;
        sY += (ty - sY) * k;
        const float wTL = std::sqrt ((1 - sX) * sY), wTR = std::sqrt (sX * sY);
        const float wBL = std::sqrt ((1 - sX) * (1 - sY)), wBR = std::sqrt (sX * (1 - sY));
        const float wetL = wTL * cL[i] + wTR * eL[i] + wBL * fL[i] + wBR * sL[i];
        const float wetR = wTL * cR[i] + wTR * eR[i] + wBL * fR[i] + wBR * sR[i];
        const float dg = std::cos (sMix * 1.5707963f), wg = std::sin (sMix * 1.5707963f);
        L[i] = dg * dL[i] + wg * wetL;
        R[i] = dg * dR[i] + wg * wetR;
    }
}

} // namespace ss
