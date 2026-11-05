// Engine tests: g++ -O2 -std=c++17 Tests/EngineTests.cpp Source/StretchEngine.cpp -o engine-tests
#include "../Source/StretchEngine.h"
#include <cstdio>
#include <chrono>
#include <cstdlib>

using namespace ss;

static int failures = 0;
#define CHECK(cond, ...) do { if (! (cond)) { ++failures; std::printf ("  FAIL: " __VA_ARGS__); std::printf ("\n"); } } while (0)

static std::vector<float> sine (double f, double sec, double sr, float amp = 0.5f)
{
    std::vector<float> v ((size_t) (sec * sr));
    for (size_t i = 0; i < v.size(); ++i) v[i] = amp * (float) std::sin (kTwoPi * f * (double) i / sr);
    return v;
}

static double rms (const std::vector<float>& a, size_t s, size_t e)
{
    double t = 0; for (size_t i = s; i < e; ++i) t += a[i] * a[i];
    return std::sqrt (t / (double) (e - s));
}

static double zcFreq (const std::vector<float>& a, size_t s, size_t e, double sr)
{
    int z = 0; for (size_t i = s + 1; i < e; ++i) if (a[i - 1] < 0 && a[i] >= 0) ++z;
    return z / ((double) (e - s) / sr);
}

struct Out { std::vector<float> L, R; bool ended = false; };

static Out run (const std::vector<float>& src, double srcRate, double outRate, StreamSettings st, double seconds, uint32_t seed = 123)
{
    std::vector<float> rev (src.rbegin(), src.rend());
    SourceView v;
    v.fwd[0] = src.data(); v.rev[0] = rev.data();
    v.numChannels = 1; v.len = (int) src.size(); v.sampleRate = srcRate;
    StretchStream s;
    s.prepare (outRate);
    s.start (0.0, seed, st.reverse, v.len);
    Out o;
    const size_t total = (size_t) (seconds * outRate);
    o.L.assign (total, 0); o.R.assign (total, 0);
    const int block = 256;
    for (size_t p = 0; p < total; p += block)
    {
        const int n = (int) std::min<size_t> (block, total - p);
        if (s.render (v, st, o.L.data() + p, o.R.data() + p, n, 1.0f) < n) { o.ended = true; break; }
    }
    return o;
}

int main()
{
    // FFT round trip
    {
        RealFFT f; f.prepare (1024);
        std::vector<double> x (1024), y (1024), re (513), im (513);
        for (auto& v : x) v = (std::rand() / (double) RAND_MAX) * 2 - 1;
        f.forward (x.data(), re.data(), im.data());
        f.inverse (re.data(), im.data(), y.data());
        double e = 0; for (int i = 0; i < 1024; ++i) e = std::max (e, std::abs (x[i] - y[i]));
        std::printf ("fft round trip err %.2e\n", e);
        CHECK (e < 1e-12, "fft round trip");
    }

    const double sr = 48000;
    const auto tone = sine (440, 2, sr);
    struct Case { double scatter; int weave; double pitch; double drift; double expectHz; };
    for (auto c : { Case { 0, 4, 0, 0, 440 }, Case { 1, 4, 0, 0, 440 }, Case { 0.5, 4, 0, 0, 440 }, Case { 0, 4, 12, 0, 880 },
                    Case { 1, 4, -12, 0, 220 }, Case { 0, 8, 7, 0, 659.26 }, Case { 1, 16, 0, 300, 440 }, Case { 0, 4, 0, 200, 440 } })
    {
        StreamSettings st; st.stretch = 5; st.scatter = c.scatter; st.weave = c.weave; st.pitchSemis = c.pitch; st.driftMs = c.drift;
        auto o = run (tone, sr, sr, st, 10);
        const size_t a = (size_t) (3 * sr), b = (size_t) (7 * sr);
        const double r = rms (o.L, a, b), f = zcFreq (o.L, a, b, sr);
        std::printf ("scatter %.1f weave %2d pitch %+5.1f drift %3.0f: rms %.3f (in 0.354)  freq %.1f Hz\n", c.scatter, c.weave, c.pitch, c.drift, r, f);
        CHECK (std::abs (20 * std::log10 (r / 0.3536)) < 1.5, "level");
        CHECK (std::abs (f - c.expectHz) / c.expectHz < 0.02, "frequency %.1f vs %.1f", f, c.expectHz);
    }

    // Source at 44.1 kHz played at 48 kHz: pitch and duration must be kept
    {
        const auto t44 = sine (440, 1, 44100);
        StreamSettings st; st.stretch = 4; st.scatter = 0;
        auto o = run (t44, 44100, 48000, st, 6);
        const double f = zcFreq (o.L, (size_t) (1 * 48000), (size_t) (3 * 48000), 48000);
        std::printf ("44.1k source @ 48k out: %.1f Hz, ended %d (expect ~4 s)\n", f, (int) o.ended);
        CHECK (std::abs (f - 440) < 6, "resample pitch");
        size_t last = 0; for (size_t i = 0; i < o.L.size(); ++i) if (std::abs (o.L[i]) > 1e-4f) last = i;
        std::printf ("  last audible sample at %.2f s\n", last / 48000.0);
        CHECK (o.ended && std::abs (last / 48000.0 - 4.0) < 0.35, "one-shot length");
    }

    // Loop keeps going; reverse works; mono→stereo decorrelates
    {
        const auto t = sine (330, 0.5, sr);
        StreamSettings st; st.stretch = 2; st.loop = true;
        auto o = run (t, sr, sr, st, 4);
        const double r = rms (o.L, (size_t) (2.5 * sr), (size_t) (3.5 * sr));
        std::printf ("loop: still playing at 3 s (rms %.3f), ended %d\n", r, (int) o.ended);
        CHECK (! o.ended && r > 0.2, "loop");

        StreamSettings rv; rv.stretch = 3; rv.reverse = true;
        std::vector<float> ramp ((size_t) sr);
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = (float) (i / sr) * (float) std::sin (kTwoPi * 300 * i / sr);
        auto o2 = run (ramp, sr, sr, rv, 3.5);
        const double early = rms (o2.L, (size_t) (0.4 * sr), (size_t) (0.9 * sr)), late = rms (o2.L, (size_t) (2.1 * sr), (size_t) (2.6 * sr));
        std::printf ("reverse: fades out (early %.3f > late %.3f)\n", early, late);
        CHECK (early > late * 2, "reverse");

        StreamSettings ms; ms.stretch = 8; ms.scatter = 1;
        auto o3 = run (tone, sr, sr, ms, 5);
        double num = 0, dl = 0, dr = 0;
        for (size_t i = (size_t) sr; i < (size_t) (4 * sr); ++i) { num += o3.L[i] * o3.R[i]; dl += o3.L[i] * o3.L[i]; dr += o3.R[i] * o3.R[i]; }
        const double corr = num / std::sqrt (dl * dr);
        std::printf ("mono→stereo L/R correlation %.2f (low = wide)\n", corr);
        CHECK (corr < 0.6, "stereo width");
    }

    // Convolution against direct
    {
        std::vector<float> x (5000), h (300), y (5299), d (5299, 0);
        for (auto& v : x) v = (std::rand() / (float) RAND_MAX) - 0.5f;
        for (auto& v : h) v = (std::rand() / (float) RAND_MAX) - 0.5f;
        fftConvolve (x.data(), x.size(), h.data(), h.size(), y.data());
        for (size_t i = 0; i < x.size(); ++i) for (size_t j = 0; j < h.size(); ++j) d[i + j] += x[i] * h[j];
        double e = 0; for (size_t i = 0; i < y.size(); ++i) e = std::max (e, (double) std::abs (y[i] - d[i]));
        std::printf ("fftConvolve max err %.2e\n", e);
        CHECK (e < 1e-4, "convolution");
        std::vector<float> L, R; makeImpulse (48000, 2.5, 9, L, R);
        double en = 0; for (float v : L) en += v * v;
        CHECK (std::abs (en - 1) < 1e-3, "IR energy");
    }

    // Real-time cost: one stereo voice, worst case weave 16 + phase vocoder + pitch
    for (auto c : { Case { 1, 4, 0, 0, 0 }, Case { 0.3, 16, 5, 100, 0 } })
    {
        StreamSettings st; st.stretch = 50; st.scatter = c.scatter; st.weave = c.weave; st.pitchSemis = c.pitch; st.driftMs = c.drift;
        const auto t0 = std::chrono::steady_clock::now();
        run (tone, sr, sr, st, 30);
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("CPU: 30 s stereo, scatter %.1f weave %d → %.2f s  (%.1f%% of one core)\n", c.scatter, c.weave, secs, 100 * secs / 30);
    }

    std::printf (failures ? "\n%d FAILURE(S)\n" : "\nall engine tests passed\n", failures);
    return failures ? 1 : 0;
}
