/*
 * Modulation — 8 free-running LFOs and a routing table (LFO → parameter, depth).
 *
 * LFOs are pure functions of time, so any thread can ask "what is LFO n at
 * sample t?". The audio thread advances them each block and publishes a
 * (clock, phase, rate, shape) snapshot; the render threads — which work
 * ~100 ms ahead of the audio — extrapolate from it to the exact time of
 * the samples they are making. Exports run their own clock from zero.
 *
 * Modulation is applied in the parameter's normalised (0..1, skewed) range:
 * depth ±1 sweeps the whole range, so it feels the same on every control.
 */
#pragma once

#include "Params.h"
#include "StretchEngine.h"
#include <atomic>
#include <array>

namespace ss
{

constexpr int kMaxRoutes = 64;

/** Bipolar (-1..1) value of a shape at `cycles` (whole + fractional periods). */
inline float lfoShape (int shape, double cycles, int lfoIndex) noexcept
{
    const double ph = cycles - std::floor (cycles);
    switch (shape)
    {
        case Triangle: return (float) (ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph);
        case SawUp:    return (float) (2.0 * ph - 1.0);
        case SawDown:  return (float) (1.0 - 2.0 * ph);
        case Square:   return ph < 0.5 ? 1.0f : -1.0f;
        case SampleHold:
            return (float) (hashUnit (0xA11CEu + (uint32_t) lfoIndex, (uint32_t) (int64_t) std::floor (cycles)) * 2.0 - 1.0);
        case Wander:
        {
            const uint32_t i = (uint32_t) (int64_t) std::floor (cycles);
            const double a = hashUnit (0xBEEFu + (uint32_t) lfoIndex, i) * 2.0 - 1.0;
            const double b = hashUnit (0xBEEFu + (uint32_t) lfoIndex, i + 1) * 2.0 - 1.0;
            const double u = 0.5 - 0.5 * std::cos (kPi * ph);
            return (float) (a + (b - a) * u);
        }
        default: return (float) std::sin (kTwoPi * ph);
    }
}

struct RouteData { int lfo = -1, target = -1; float depth = 0; };
using RouteList = std::array<RouteData, kMaxRoutes>;

class ModSystem
{
public:
    ModSystem()
    {
        for (int t = 0; t < kNumTargets; ++t)
            if (auto* s = findSpec (targetParamId (t))) { ranges[(size_t) t] = specRange (*s); integer[(size_t) t] = s->integer; }
        for (int i = 0; i < kNumLfos; ++i) { rate[i].store (0.25f); shape[i].store (Sine); cycles[i].store (0.0); }
    }

    // ------------------------------------------------- routes (message thread)
    int addRoute (int lfo, int target, float depth)
    {
        for (int r = 0; r < kMaxRoutes; ++r)
            if (routes[r].lfo.load() == lfo && routes[r].target.load() == target) { routes[r].depth.store (depth); bump(); return r; }
        for (int r = 0; r < kMaxRoutes; ++r)
            if (routes[r].lfo.load() < 0)
            {
                routes[r].depth.store (depth);
                routes[r].target.store (target);
                routes[r].lfo.store (lfo);
                bump();
                return r;
            }
        return -1;
    }
    void removeRoute (int r) { if (r >= 0 && r < kMaxRoutes) { routes[r].lfo.store (-1); routes[r].target.store (-1); bump(); } }
    void setDepth (int r, float d) { if (r >= 0 && r < kMaxRoutes) { routes[r].depth.store (juce::jlimit (-1.0f, 1.0f, d)); bump(); } }
    void clearRoutes() { for (int r = 0; r < kMaxRoutes; ++r) removeRoute (r); }
    RouteData route (int r) const { return { routes[r].lfo.load(), routes[r].target.load(), routes[r].depth.load() }; }
    RouteList snapshotRoutes() const
    {
        RouteList l;
        for (int r = 0; r < kMaxRoutes; ++r) l[(size_t) r] = route (r);
        return l;
    }
    /** Changes whenever a route is added/removed/edited (cheap UI change detection). */
    int version() const noexcept { return ver.load(); }
    bool hasRoutesFor (int target) const noexcept
    {
        for (int r = 0; r < kMaxRoutes; ++r) if (routes[r].lfo.load() >= 0 && routes[r].target.load() == target) return true;
        return false;
    }

    // --------------------------------------------------------- LFO clock
    /** Audio thread, once per block before rendering: LFO state valid at sample `clock`. */
    void advance (int64_t clock, int numSamples, double sampleRate, const float* rates, const int* shapes) noexcept
    {
        seq.fetch_add (1, std::memory_order_acq_rel);          // odd: writing
        for (int i = 0; i < kNumLfos; ++i)
        {
            rate[i].store (rates[i], std::memory_order_relaxed);
            shape[i].store (shapes[i], std::memory_order_relaxed);
            cycles[i].store (localCycles[i], std::memory_order_relaxed);
        }
        refClock.store (clock, std::memory_order_relaxed);
        sr.store (sampleRate, std::memory_order_relaxed);
        seq.fetch_add (1, std::memory_order_acq_rel);          // even: done
        for (int i = 0; i < kNumLfos; ++i)
        {
            localCycles[i] += (double) rates[i] * numSamples / sampleRate;
            if (localCycles[i] > 1.0e9) localCycles[i] = std::fmod (localCycles[i], 1.0);
        }
    }

    /** Any thread: all 8 LFO values at sample time t (extrapolated from the last published state). */
    void lfoValuesAt (int64_t t, float* out) const noexcept
    {
        double c[kNumLfos] {}; float r[kNumLfos] {}; int s[kNumLfos] {}; int64_t clk = t; double rateHz = 48000;
        for (int tries = 0; tries < 8; ++tries)
        {
            const uint32_t s0 = seq.load (std::memory_order_acquire);
            if (s0 & 1u) continue;
            for (int i = 0; i < kNumLfos; ++i)
            {
                c[i] = cycles[i].load (std::memory_order_relaxed);
                r[i] = rate[i].load (std::memory_order_relaxed);
                s[i] = shape[i].load (std::memory_order_relaxed);
            }
            clk = refClock.load (std::memory_order_relaxed);
            rateHz = sr.load (std::memory_order_relaxed);
            if (seq.load (std::memory_order_acquire) == s0) break;
        }
        const double dt = (double) (t - clk) / juce::jmax (1.0, rateHz);
        for (int i = 0; i < kNumLfos; ++i) out[i] = lfoShape (s[i], c[i] + r[i] * dt, i);
    }

    /** Offline: LFOs started at phase 0 at time zero. */
    static void lfoValuesOffline (double seconds, const float* rates, const int* shapes, float* out) noexcept
    {
        for (int i = 0; i < kNumLfos; ++i) out[i] = lfoShape (shapes[i], (double) rates[i] * seconds, i);
    }

    // ------------------------------------------------------ apply
    /** base (real units) + routed LFOs → modulated value (real units). */
    float apply (int target, float base, const float* lfo) const noexcept
    {
        float s = 0; bool any = false;
        for (int r = 0; r < kMaxRoutes; ++r)
        {
            const int l = routes[r].lfo.load (std::memory_order_relaxed);
            if (l < 0 || routes[r].target.load (std::memory_order_relaxed) != target) continue;
            s += routes[r].depth.load (std::memory_order_relaxed) * lfo[l];
            any = true;
        }
        return any ? shifted (target, base, s) : base;
    }

    float apply (int target, float base, const float* lfo, const RouteList& list) const noexcept
    {
        float s = 0; bool any = false;
        for (const auto& rd : list)
            if (rd.lfo >= 0 && rd.target == target) { s += rd.depth * lfo[rd.lfo]; any = true; }
        return any ? shifted (target, base, s) : base;
    }

    /** Normalised offset of a target (for drawing): sum of depth × LFO. */
    float offset (int target, const float* lfo) const noexcept
    {
        float s = 0;
        for (int r = 0; r < kMaxRoutes; ++r)
            if (routes[r].lfo.load() >= 0 && routes[r].target.load() == target)
                s += routes[r].depth.load() * lfo[routes[r].lfo.load()];
        return s;
    }

    float toNorm (int target, float v) const { return ranges[(size_t) target].convertTo0to1 (v); }
    float fromNorm (int target, float n) const { return ranges[(size_t) target].convertFrom0to1 (n); }

    // ------------------------------------------------------ state
    juce::ValueTree toTree() const
    {
        juce::ValueTree t ("MOD");
        for (int r = 0; r < kMaxRoutes; ++r)
        {
            const auto d = route (r);
            if (d.lfo < 0 || d.target < 0) continue;
            juce::ValueTree e ("ROUTE");
            e.setProperty ("lfo", d.lfo, nullptr);
            e.setProperty ("target", targetParamId (d.target), nullptr);
            e.setProperty ("depth", d.depth, nullptr);
            t.appendChild (e, nullptr);
        }
        return t;
    }

    void fromTree (const juce::ValueTree& t)
    {
        clearRoutes();
        for (const auto& e : t)
        {
            const int lfo = e.getProperty ("lfo", -1);
            const int target = targetForParam (e.getProperty ("target", "").toString());
            if (lfo >= 0 && lfo < kNumLfos && target >= 0)
                addRoute (lfo, target, juce::jlimit (-1.0f, 1.0f, (float) (double) e.getProperty ("depth", 0.5)));
        }
    }

private:
    float shifted (int target, float base, float s) const noexcept
    {
        const auto& rg = ranges[(size_t) target];
        const float n = juce::jlimit (0.0f, 1.0f, rg.convertTo0to1 (juce::jlimit (rg.start, rg.end, base)) + s);
        float v = rg.convertFrom0to1 (n);
        if (integer[(size_t) target]) v = std::round (v);
        return v;
    }
    void bump() { ver.fetch_add (1); }

    struct Route { std::atomic<int> lfo { -1 }, target { -1 }; std::atomic<float> depth { 0 }; };
    Route routes[kMaxRoutes];
    std::atomic<int> ver { 0 };

    std::array<juce::NormalisableRange<float>, kNumTargets> ranges;
    std::array<bool, kNumTargets> integer {};

    // published LFO state (seqlock)
    std::atomic<uint32_t> seq { 0 };
    std::atomic<double> cycles[kNumLfos];
    std::atomic<float> rate[kNumLfos];
    std::atomic<int> shape[kNumLfos];
    std::atomic<int64_t> refClock { 0 };
    std::atomic<double> sr { 48000 };
    double localCycles[kNumLfos] {};          // audio thread only
};

} // namespace ss
