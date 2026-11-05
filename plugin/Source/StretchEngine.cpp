#include "StretchEngine.h"

namespace ss
{

static inline double princarg (double x) noexcept
{
    return x - kTwoPi * std::round (x / kTwoPi);
}

// ================================================================ RealFFT

void RealFFT::prepare (int size)
{
    n = size;
    m = n >> 1;
    zr.assign ((size_t) m, 0.0);
    zi.assign ((size_t) m, 0.0);
    cs.resize ((size_t) std::max (1, m >> 1));
    sn.resize ((size_t) std::max (1, m >> 1));
    for (int k = 0; k < (m >> 1); ++k)
    {
        cs[(size_t) k] = std::cos (kTwoPi * k / m);
        sn[(size_t) k] = std::sin (kTwoPi * k / m);
    }
    wc.resize ((size_t) m + 1);
    ws.resize ((size_t) m + 1);
    for (int k = 0; k <= m; ++k)
    {
        wc[(size_t) k] = std::cos (kTwoPi * k / n);
        ws[(size_t) k] = std::sin (kTwoPi * k / n);
    }
    rev.resize ((size_t) m);
    int bits = 0;
    while ((1 << bits) < m) ++bits;
    for (int i = 0; i < m; ++i)
    {
        uint32_t r = 0;
        for (int b = 0; b < bits; ++b)
            r |= (uint32_t) ((i >> b) & 1) << (bits - 1 - b);
        rev[(size_t) i] = r;
    }
}

void RealFFT::fft (double* re, double* im, bool inv) noexcept
{
    for (int i = 0; i < m; ++i)
    {
        const int j = (int) rev[(size_t) i];
        if (j > i) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
    }
    for (int size = 2; size <= m; size <<= 1)
    {
        const int half = size >> 1, step = m / size;
        for (int j = 0; j < half; ++j)
        {
            const double wr = cs[(size_t) (j * step)];
            const double wi = inv ? sn[(size_t) (j * step)] : -sn[(size_t) (j * step)];
            for (int i = j; i < m; i += size)
            {
                const int b = i + half;
                const double xr = re[b] * wr - im[b] * wi;
                const double xi = re[b] * wi + im[b] * wr;
                re[b] = re[i] - xr; im[b] = im[i] - xi;
                re[i] += xr;        im[i] += xi;
            }
        }
    }
}

void RealFFT::forward (const double* x, double* re, double* im) noexcept
{
    double* a = zr.data();
    double* b = zi.data();
    for (int i = 0; i < m; ++i) { a[i] = x[2 * i]; b[i] = x[2 * i + 1]; }
    fft (a, b, false);
    for (int k = 0; k <= m; ++k)
    {
        const int k1 = k == m ? 0 : k;
        const int k2 = k == 0 ? 0 : m - k;
        const double rr = a[k1], ri = b[k1];
        const double cr = a[k2], ci = -b[k2];
        const double er = 0.5 * (rr + cr), ei = 0.5 * (ri + ci);
        const double orr = 0.5 * (ri - ci), oi = -0.5 * (rr - cr);
        const double wr = wc[(size_t) k], wi = -ws[(size_t) k];
        re[k] = er + (orr * wr - oi * wi);
        im[k] = ei + (orr * wi + oi * wr);
    }
}

void RealFFT::inverse (const double* re, const double* im, double* x) noexcept
{
    double* a = zr.data();
    double* b = zi.data();
    for (int k = 0; k < m; ++k)
    {
        const double ar = re[k], ai = im[k];
        const double br = re[m - k], bi = -im[m - k];
        const double er = 0.5 * (ar + br), ei = 0.5 * (ai + bi);
        const double dr = 0.5 * (ar - br), di = 0.5 * (ai - bi);
        const double wr = wc[(size_t) k], wi = ws[(size_t) k];
        const double orr = dr * wr - di * wi, oi = dr * wi + di * wr;
        a[k] = er - oi;
        b[k] = ei + orr;
    }
    fft (a, b, true);
    const double s = 1.0 / m;
    for (int i = 0; i < m; ++i) { x[2 * i] = a[i] * s; x[2 * i + 1] = b[i] * s; }
}

int windowSizeFor (double sr)
{
    const double target = sr * 0.17;
    int n = 1024;
    while (n * 2 <= target * 1.42 && n < 65536) n *= 2;
    return n;
}

// ========================================================= StretchChannel

void StretchChannel::prepare (int size)
{
    N = size;
    H = N / 2 + 1;
    fft.prepare (N);
    auto alloc = [] (std::vector<double>& v, int len) { v.assign ((size_t) len, 0.0); };
    alloc (win, N); alloc (fa, N); alloc (fb, N); alloc (fy, N);
    for (auto* v : { &aRe, &aIm, &bRe, &bIm, &mag, &phA, &omega, &oMag, &oPh, &prev, &sRe, &sIm, &binW })
        alloc (*v, H);
    peaks.assign ((size_t) H, 0);
    sumW2 = 0;
    for (int i = 0; i < N; ++i)
    {
        win[(size_t) i] = 0.5 - 0.5 * std::cos (kTwoPi * i / N);
        sumW2 += win[(size_t) i] * win[(size_t) i];
    }
    for (int k = 0; k < H; ++k)
        binW[(size_t) k] = kTwoPi * k / N;
    started = false;
}

void StretchChannel::read (const float* x, int L, bool loop, double centre, double* dest) noexcept
{
    const int half = N >> 1, msk = N - 1;
    const int64_t start = (int64_t) std::llround (centre) - half;
    const double* w = win.data();
    if (L <= 0)
    {
        std::fill (dest, dest + N, 0.0);
        return;
    }
    if (loop)
    {
        int64_t idx = start % L;
        if (idx < 0) idx += L;
        for (int i = 0; i < N; ++i)
        {
            dest[(i + half) & msk] = x[idx] * w[i];
            if (++idx >= L) idx = 0;
        }
        return;
    }
    for (int i = 0; i < N; ++i)
    {
        const int64_t idx = start + i;
        dest[(i + half) & msk] = (idx >= 0 && idx < L) ? x[idx] * w[i] : 0.0;
    }
}

void StretchChannel::grain (const Grain& g, float* out) noexcept
{
    const int half = N >> 1, msk = N - 1, hop = std::max (1, g.hop);
    const double s = std::clamp (g.scatter, 0.0, 1.0);
    const bool pv = s < 1.0;
    const bool shift = std::abs (g.ratio - 1.0) > 1e-9;
    double* mg = mag.data();

    read (g.src, g.len, g.loop, g.centre, fa.data());
    fft.forward (fa.data(), aRe.data(), aIm.data());

    double maxMag = 0;
    int maxK = 0;
    for (int k = 0; k < H; ++k)
    {
        const double v = std::sqrt (aRe[(size_t) k] * aRe[(size_t) k] + aIm[(size_t) k] * aIm[(size_t) k]);
        mg[k] = v;
        if (v > maxMag) { maxMag = v; maxK = k; }
    }
    if (maxMag < 1e-10)
    {
        std::fill (out, out + N, 0.0f);
        return;
    }

    if (pv)
    {
        const int d = N >> 3;
        for (int k = 0; k < H; ++k)
            phA[(size_t) k] = std::atan2 (aIm[(size_t) k], aRe[(size_t) k]);
        read (g.src, g.len, g.loop, g.centre + d, fb.data());
        fft.forward (fb.data(), bRe.data(), bIm.data());
        for (int k = 0; k < H; ++k)
        {
            const double phB = std::atan2 (bIm[(size_t) k], bRe[(size_t) k]);
            omega[(size_t) k] = binW[(size_t) k] + princarg (phB - phA[(size_t) k] - binW[(size_t) k] * d) / d;
        }
    }

    // ---- output spectrum
    if (! shift && ! pv)
    {
        std::copy (mag.begin(), mag.end(), oMag.begin());
    }
    else
    {
        std::fill (oMag.begin(), oMag.end(), 0.0);
        if (pv)
        {
            if (started) for (int k = 0; k < H; ++k) oPh[(size_t) k] = prev[(size_t) k] + binW[(size_t) k] * hop;
            else         std::copy (phA.begin(), phA.end(), oPh.begin());
        }
        const double thr = maxMag * 1e-4;
        int np = 0;
        for (int k = 2; k < H - 2; ++k)
        {
            const double v = mg[k];
            if (v > thr && v > mg[k - 1] && v >= mg[k + 1] && v > mg[k - 2] && v >= mg[k + 2])
                peaks[(size_t) np++] = k;
        }
        if (np == 0) peaks[(size_t) np++] = maxK;

        int lo = 0;
        for (int i = 0; i < np; ++i)
        {
            const int kp = peaks[(size_t) i];
            int hi = H;
            if (i < np - 1)
            {
                const int kn = peaks[(size_t) i + 1];
                int mi = kp + 1;
                for (int k = kp + 2; k < kn; ++k) if (mg[k] < mg[mi]) mi = k;
                hi = mi;
            }
            const int d = shift ? (int) std::lround (kp * (g.ratio - 1.0)) : 0;
            const int jp = kp + d;
            if (jp >= 0 && jp < H)
            {
                double phs = 0;
                if (pv) phs = started ? prev[(size_t) jp] + g.ratio * omega[(size_t) kp] * hop : phA[(size_t) kp];
                const int k0 = std::max (lo, -d), k1 = std::min (hi, H - d);
                for (int k = k0; k < k1; ++k)
                {
                    oMag[(size_t) (k + d)] += mg[k];
                    if (pv) oPh[(size_t) (k + d)] = phs + phA[(size_t) k] - phA[(size_t) kp];
                }
            }
            lo = hi;
        }
    }

    // ---- synthesis
    Rng rng (hash32 (g.seed, g.index));
    const double sp = s * kPi;
    for (int k = 0; k < H; ++k)
    {
        double ph;
        if (! pv) ph = kTwoPi * rng.next();
        else
        {
            ph = oPh[(size_t) k];
            if (s > 0) ph += sp * (2.0 * rng.next() - 1.0);
        }
        const double v = oMag[(size_t) k];
        sRe[(size_t) k] = v * std::cos (ph);
        sIm[(size_t) k] = v * std::sin (ph);
    }
    sIm[0] = 0;
    sIm[(size_t) H - 1] = 0;
    if (pv)
    {
        for (int k = 0; k < H; ++k) prev[(size_t) k] = princarg (oPh[(size_t) k]);
        started = true;
    }
    else
    {
        started = false;
    }

    fft.inverse (sRe.data(), sIm.data(), fy.data());

    // level: coherent vs incoherent overlap-add, blended by scatter
    const double gCoh = hop / sumW2;
    const double gInc = std::sqrt ((double) N * hop) / sumW2;
    const double gain = std::pow (gCoh, 1.0 - s) * std::pow (gInc, s);
    for (int i = 0; i < N; ++i)
        out[i] = (float) (fy[(size_t) ((i + half) & msk)] * win[(size_t) i] * gain);
}

// ========================================================== StretchStream

void StretchStream::prepare (double sr)
{
    outRate = sr;
    N = windowSizeFor (sr);
    mask = 2 * N - 1;
    for (auto& c : chan) c.prepare (N);
    for (auto& r : ring) r.assign ((size_t) (2 * N), 0.0f);
    tmp.assign ((size_t) N, 0.0f);
    finished = true;
}

void StretchStream::start (double startFrac, uint32_t seed, bool reversed, int regionLen) noexcept
{
    for (auto& r : ring) std::fill (r.begin(), r.end(), 0.0f);
    for (auto& c : chan) c.resetPhase();
    seeds[0] = seed ? seed : 1u;
    seeds[1] = hash32 (seeds[0], 77u) | 1u;
    reversedView = reversed;
    lastLen = std::max (0, regionLen);
    pos = std::clamp (startFrac, 0.0, 1.0) * lastLen;
    outPos = 0;
    nextStart = 0;
    endAt = -1;
    grainIndex = 0;
    finished = false;
    lastTwoPasses = false;
}

void StretchStream::jumpTo (double frac, int regionLen) noexcept
{
    lastLen = std::max (0, regionLen);
    pos = std::clamp (frac, 0.0, 1.0) * lastLen;
    endAt = -1;
    finished = false;
    for (auto& c : chan) c.resetPhase();
}

static double smoothNoise (uint32_t seed, double t) noexcept
{
    const double fi = std::floor (t);
    const double f = t - fi;
    const uint32_t i = (uint32_t) (int64_t) fi;
    const double u = 0.5 - 0.5 * std::cos (kPi * f);
    const double a = hashUnit (seed, i) * 2 - 1, b = hashUnit (seed, i + 1) * 2 - 1;
    return a + (b - a) * u;
}

double StretchStream::driftOffset (int ch, double driftSrc) const noexcept
{
    const uint32_t sd = seeds[ch];
    const double t = (double) (nextStart + N / 2) / outRate;
    const double s1 = smoothNoise (sd ^ 0xD1F7u, t / 2.9);
    const double s2 = smoothNoise (sd ^ 0xA5C3u, t / 0.7);
    const double jit = hashUnit (sd ^ 0x5BD1E995u, grainIndex) * 2 - 1;
    return driftSrc * (0.4 * s1 + 0.2 * s2 + 0.4 * jit);
}

void StretchStream::nextGrain (const SourceView& src, const StreamSettings& st) noexcept
{
    const int weave = std::clamp (st.weave, 1, 16);
    const int hop = std::max (1, (int) std::lround ((double) N / weave));
    const double srcScale = src.sampleRate / outRate;
    const double ratio = std::clamp (std::pow (2.0, st.pitchSemis / 12.0) * srcScale, 1.0 / 64.0, 64.0);
    const double stretch = std::max (0.01, st.stretch);
    const double advance = hop * srcScale / stretch;
    const int len = src.len;

    if (len <= 0 || src.numChannels <= 0)
    {
        nextStart += hop;
        return;
    }
    if (st.reverse != reversedView)
    {
        pos = len - pos;
        reversedView = st.reverse;
    }
    if (lastLen != len)
    {
        pos = std::clamp (pos, 0.0, (double) len);
        lastLen = len;
    }
    if (st.loop)
    {
        pos = std::fmod (pos, (double) len);
        if (pos < 0) pos += len;
    }
    else if (pos > len + 0.5 * N * srcScale)
    {
        // played to the end: let the last grain ring out, then stop
        endAt = nextStart - hop + N;
        return;
    }

    const bool twoPasses = src.numChannels == 1 && st.monoToStereo;
    if (twoPasses != lastTwoPasses)
    {
        chan[1].resetPhase();
        lastTwoPasses = twoPasses;
    }
    const double driftSrc = std::max (0.0, st.driftMs) * src.sampleRate / 1000.0;
    const int passes = (src.numChannels == 2 || twoPasses) ? 2 : 1;

    for (int c = 0; c < passes; ++c)
    {
        double centre = pos;
        if (driftSrc > 0)
        {
            // stereo sources share one drift curve (keeps the image); two passes each wander
            centre = pos + driftOffset (twoPasses ? c : 0, driftSrc);
            if (! st.loop && pos <= len)
            {
                if (centre < 0) centre = -centre;
                if (centre > len) centre = 2.0 * len - centre;
                if (centre < 0) centre = 0;
            }
        }
        StretchChannel::Grain g;
        const int sc = src.numChannels == 2 ? c : 0;
        g.src = reversedView ? src.rev[sc] : src.fwd[sc];
        g.len = len;
        g.loop = st.loop;
        g.centre = centre;
        g.hop = hop;
        g.ratio = ratio;
        g.scatter = st.scatter;
        g.seed = twoPasses ? seeds[c] : seeds[0];
        g.index = grainIndex;
        chan[c].grain (g, tmp.data());

        float* r = ring[(size_t) c].data();
        for (int i = 0; i < N; ++i)
            r[(nextStart + i) & mask] += tmp[(size_t) i];
        if (passes == 1)
        {
            float* r1 = ring[1].data();
            for (int i = 0; i < N; ++i)
                r1[(nextStart + i) & mask] += tmp[(size_t) i];
        }
    }

    pos += advance;
    ++grainIndex;
    nextStart += hop;
}

int StretchStream::render (const SourceView& src, const StreamSettings& st, float* outL, float* outR, int n, float gain) noexcept
{
    if (finished) return 0;
    float* r0 = ring[0].data();
    float* r1 = ring[1].data();
    for (int i = 0; i < n; ++i)
    {
        while (endAt < 0 && nextStart <= outPos)
            nextGrain (src, st);
        if (endAt >= 0 && outPos >= endAt)
        {
            finished = true;
            return i;
        }
        const int idx = (int) (outPos & mask);
        outL[i] += r0[idx] * gain;
        outR[i] += r1[idx] * gain;
        r0[idx] = 0;
        r1[idx] = 0;
        ++outPos;
    }
    return n;
}

// ================================================================= reverb

void makeImpulse (double sr, double size, uint32_t seed, std::vector<float>& left, std::vector<float>& right)
{
    size = std::max (0.05, size);
    const int pre = (int) std::lround (0.012 * sr);
    const int len = pre + std::max (1, (int) std::lround (size * sr));
    for (int c = 0; c < 2; ++c)
    {
        auto& h = c == 0 ? left : right;
        h.assign ((size_t) len, 0.0f);
        Rng rng ((seed ? seed : 7u) + (uint32_t) c * 7919u);
        double lp = 0, energy = 0;
        for (int i = 0; i < len - pre; ++i)
        {
            const double t = i / sr;
            const double env = std::exp (-6.9078 * t / size) * std::min (1.0, t / 0.004);
            const double a = 0.82 * std::exp (-t / (size * 0.5)) + 0.1;
            lp += a * ((rng.next() * 2 - 1) - lp);
            const double v = lp * env;
            h[(size_t) (pre + i)] = (float) v;
            energy += v * v;
        }
        const float sc = (float) (1.0 / std::sqrt (energy > 0 ? energy : 1.0));
        for (auto& v : h) v *= sc;
    }
}

void fftConvolve (const float* x, size_t xLen, const float* h, size_t hLen, float* out)
{
    const size_t outLen = xLen + hLen - 1;
    std::fill (out, out + outLen, 0.0f);
    if (xLen == 0 || hLen == 0) return;
    size_t n = 1 << 16;
    while (n < 2 * hLen) n <<= 1;
    const size_t block = n - hLen + 1;
    const size_t H = n / 2 + 1;
    RealFFT f;
    f.prepare ((int) n);
    std::vector<double> buf (n), hr (H), hi (H), xr (H), xi (H);
    for (size_t i = 0; i < n; ++i) buf[i] = i < hLen ? h[i] : 0.0;
    f.forward (buf.data(), hr.data(), hi.data());
    for (size_t b0 = 0; b0 < xLen; b0 += block)
    {
        const size_t cnt = std::min (block, xLen - b0);
        for (size_t i = 0; i < n; ++i) buf[i] = i < cnt ? x[b0 + i] : 0.0;
        f.forward (buf.data(), xr.data(), xi.data());
        for (size_t k = 0; k < H; ++k)
        {
            const double r = xr[k] * hr[k] - xi[k] * hi[k];
            const double im = xr[k] * hi[k] + xi[k] * hr[k];
            xr[k] = r; xi[k] = im;
        }
        f.inverse (xr.data(), xi.data(), buf.data());
        const size_t lim = std::min (n, outLen - b0);
        for (size_t i = 0; i < lim; ++i) out[b0 + i] += (float) buf[i];
    }
}

} // namespace ss
