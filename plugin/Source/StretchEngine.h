/*
 * StretchEngine — streaming spectral time-stretch.
 *
 * C++ port of the original JavaScript engine, reshaped to run in real time:
 * grains are synthesised just before they are needed, so every control
 * (stretch, pitch, weave, scatter, drift, reverse, loop) can change live.
 *
 *   STRETCH  the read position advances by hop / stretch per grain
 *   PITCH    spectral peaks (and their surrounding bins) are moved
 *   WEAVE    grains overlapping at each moment: hop = N / weave
 *   SCATTER  0 = phase vocoder (propagated phase, locked to the real
 *            analysis phase around each peak); 1 = invented random phase
 *   DRIFT    each grain reads from a wandering position
 *
 * No JUCE dependency, so it can be unit-tested on its own.
 */
#pragma once

#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

namespace ss
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// ------------------------------------------------------------- random --

inline uint32_t hash32 (uint32_t a, uint32_t b) noexcept
{
    uint32_t h = (a ^ (uint32_t) ((b + 0x9E3779B9u) * 0x85EBCA6Bu));
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

/** Uniform in [0, 1). */
inline double hashUnit (uint32_t a, uint32_t b) noexcept
{
    return (double) hash32 (a, b) / 4294967296.0;
}

struct Rng
{
    uint32_t s;
    explicit Rng (uint32_t seed) noexcept : s (seed) {}
    double next() noexcept
    {
        s += 0x6D2B79F5u;
        uint32_t t = s;
        t = (t ^ (t >> 15)) * (t | 1u);
        t ^= t + (t ^ (t >> 7)) * (t | 61u);
        return (double) (t ^ (t >> 14)) / 4294967296.0;
    }
};

// ---------------------------------------------------------------- FFT --

/** Real FFT of size n (power of two) via a complex FFT of size n/2. */
class RealFFT
{
public:
    void prepare (int n);
    int size() const noexcept { return n; }
    /** x[n] → re/im[n/2 + 1], unnormalised. */
    void forward (const double* x, double* re, double* im) noexcept;
    /** re/im[n/2 + 1] → x[n]; exact inverse of forward. */
    void inverse (const double* re, const double* im, double* x) noexcept;

private:
    void fft (double* re, double* im, bool inv) noexcept;
    int n = 0, m = 0;
    std::vector<double> zr, zi, cs, sn, wc, ws;
    std::vector<uint32_t> rev;
};

/** Grain size: power of two closest to ~170 ms. */
int windowSizeFor (double sampleRate);

// ------------------------------------------------------- one channel --

/** Turns one analysis position into one windowed output grain. */
class StretchChannel
{
public:
    struct Grain
    {
        const float* src = nullptr;  // source view (region), length len
        int len = 0;
        bool loop = false;           // read circularly
        double centre = 0;           // read centre, source samples
        int hop = 1;                 // output hop
        double ratio = 1;            // pitch ratio (incl. sample-rate scaling)
        double scatter = 1;          // 0..1
        uint32_t seed = 1;
        uint32_t index = 0;          // grain counter (drives the random streams)
    };

    void prepare (int n);
    void resetPhase() noexcept { started = false; }
    int size() const noexcept { return N; }

    /** Writes N output samples (windowed, gain-compensated) to out. */
    void grain (const Grain& g, float* out) noexcept;

private:
    void read (const float* src, int len, bool loop, double centre, double* dest) noexcept;

    int N = 0, H = 0;
    double sumW2 = 1;
    bool started = false;
    RealFFT fft;
    std::vector<double> win, fa, fb, fy, aRe, aIm, bRe, bIm, mag, phA, omega, oMag, oPh, prev, sRe, sIm, binW;
    std::vector<int> peaks;
};

// ----------------------------------------------------------- a stream --

/** Settings read once per grain (all may change at any time). */
struct StreamSettings
{
    double stretch = 20;
    double pitchSemis = 0;      // total transposition (param + note + bend)
    int weave = 4;
    double scatter = 1;         // 0..1
    double driftMs = 0;
    bool loop = false;
    bool reverse = false;
    bool monoToStereo = true;   // mono source → two independent passes
};

/** Source data seen by a stream: forward and reversed views of one region. */
struct SourceView
{
    const float* fwd[2] = { nullptr, nullptr };   // region start, per channel
    const float* rev[2] = { nullptr, nullptr };   // reversed region start
    int numChannels = 0;                          // 1 or 2
    int len = 0;                                  // region length
    double sampleRate = 44100;
};

/**
 * A continuous stretched stream (one voice): stereo out, pulls grains on
 * demand. Real-time safe after prepare().
 */
class StretchStream
{
public:
    void prepare (double outputSampleRate);
    double outputSampleRate() const noexcept { return outRate; }
    int grainSize() const noexcept { return N; }

    /** Begin at `startFrac` (0..1) of the region. */
    void start (double startFrac, uint32_t seed, bool reversed, int regionLen) noexcept;
    /** Jump to a new position without restarting the voice. */
    void jumpTo (double frac, int regionLen) noexcept;

    /**
     * Render n samples, *adding* into outL/outR (scaled by gain).
     * Returns how many samples were produced: fewer than n once a
     * non-looping stream has played to its end.
     */
    int render (const SourceView& src, const StreamSettings& st, float* outL, float* outR, int n, float gain) noexcept;

    bool isFinished() const noexcept { return finished; }
    /** Position in the region, 0..1, in the direction currently heard. */
    double positionFrac() const noexcept { return lastLen > 0 ? std::clamp (pos / lastLen, 0.0, 1.0) : 0.0; }
    /** Read position in forward source terms, 0..1 (for drawing on the source waveform). */
    double sourceFrac() const noexcept { return reversedView ? 1.0 - positionFrac() : positionFrac(); }
    /** Output time since start(), seconds. */
    double elapsedSeconds() const noexcept { return (double) outPos / outRate; }

private:
    void nextGrain (const SourceView& src, const StreamSettings& st) noexcept;
    double driftOffset (int ch, double driftSrc) const noexcept;

    double outRate = 48000;
    int N = 0, mask = 0;
    StretchChannel chan[2];
    std::vector<float> ring[2];
    std::vector<float> tmp;
    int64_t outPos = 0, nextStart = 0, endAt = -1;
    double pos = 0;                 // region position (source samples, current view)
    double lastLen = 0;
    bool reversedView = false;
    uint32_t seeds[2] = { 1, 1 };
    bool lastTwoPasses = false;     // mono source rendered as two independent passes
    uint32_t grainIndex = 0;
    bool finished = true;
};

// ------------------------------------------------------------ reverb --

/** Stereo impulse response: darkening decaying noise, RT60 = size seconds, unit energy. */
void makeImpulse (double sampleRate, double size, uint32_t seed, std::vector<float>& left, std::vector<float>& right);

/** out = x * h (full length x.size() + h.size() - 1), FFT block convolution. */
void fftConvolve (const float* x, size_t xLen, const float* h, size_t hLen, float* out);

} // namespace ss
