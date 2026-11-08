/*
 * VoiceEngine — polyphonic stretched voices.
 *
 * Each voice's StretchStream runs on a background render thread and fills a
 * lock-free FIFO ~100 ms ahead; the audio thread only reads finished samples
 * and applies the envelope. Spectral grains are bursty work (several FFTs at
 * once), so this keeps the audio callback light at any buffer size.
 *
 * Commands (start / jump / stop) go to the worker through a generation
 * counter: the audio thread bumps `gen`, the worker applies the command and
 * answers with `ack`. Until then the audio thread discards stale samples.
 */
#pragma once

#include "StretchEngine.h"
#include "SampleData.h"
#include "Modulation.h"
#include <atomic>

namespace ss
{

/** Engine settings shared with the render threads (written by the audio thread). */
struct EngineShared
{
    std::atomic<float> stretch { 20 }, pitch { 0 }, scatter { 1 }, driftMs { 0 };
    std::atomic<int> weave { 4 };
    std::atomic<bool> loop { false }, reverse { false }, monoStereo { true };
    std::atomic<int> regionA { 0 }, regionB { 0 };   // samples; b <= a means "whole sample"
    std::atomic<float> window { 0 };                 // % of the sample length the region is moved by
    const ModSystem* mod = nullptr;

    StreamSettings snapshot (float noteSemis) const noexcept
    {
        StreamSettings s;
        s.stretch = stretch.load (std::memory_order_relaxed);
        s.pitchSemis = pitch.load (std::memory_order_relaxed) + noteSemis;
        s.weave = weave.load (std::memory_order_relaxed);
        s.scatter = scatter.load (std::memory_order_relaxed);
        s.driftMs = driftMs.load (std::memory_order_relaxed);
        s.loop = loop.load (std::memory_order_relaxed);
        s.reverse = reverse.load (std::memory_order_relaxed);
        s.monoToStereo = monoStereo.load (std::memory_order_relaxed);
        return s;
    }

    /** Engine settings at output sample time t, with LFO modulation applied. */
    StreamSettings snapshotAt (float noteSemis, int64_t t, float& windowOut) const noexcept
    {
        StreamSettings s = snapshot (noteSemis);
        float w = window.load (std::memory_order_relaxed);
        if (mod != nullptr)
        {
            float lfo[kNumLfos];
            mod->lfoValuesAt (t, lfo);
            s.stretch = mod->apply (tStretch, (float) s.stretch, lfo);
            s.pitchSemis = mod->apply (tPitch, pitch.load (std::memory_order_relaxed), lfo) + noteSemis;
            s.weave = (int) mod->apply (tWeave, (float) s.weave, lfo);
            s.scatter = mod->apply (tScatter, (float) s.scatter * 100.0f, lfo) / 100.0f;
            s.driftMs = mod->apply (tDrift, (float) s.driftMs, lfo);
            w = mod->apply (tWindow, w, lfo);
        }
        windowOut = w;
        return s;
    }

    /** Region of `d` as a SourceView, moved by `windowPct` % of the sample. */
    SourceView view (const SampleData& d, float windowPct = 0) const noexcept
    {
        return regionView (d, regionA.load (std::memory_order_relaxed), regionB.load (std::memory_order_relaxed), windowPct);
    }
};

class VoiceEngine
{
public:
    static constexpr int kMaxVoices = 8;
    static constexpr int kFifoSize = 32768;
    static constexpr int kChunk = 512;

    VoiceEngine();
    ~VoiceEngine();

    /** Workers call this when a voice starts (may lock; never on the audio thread). */
    std::function<SampleData::Ptr()> currentSample;
    EngineShared* shared = nullptr;

    void prepare (double sampleRate, int maxBlock);
    void shutdown();

    // ---- audio thread
    void noteOn (int note, float velocity, bool transport, double startFrac, uint32_t seed, int64_t startTime) noexcept;
    void noteOff (int note) noexcept;
    void stopTransport() noexcept;              // quick fade
    void jumpTransport (double frac) noexcept;
    void killAll() noexcept;
    void setSustain (bool down) noexcept;
    void setBend (float semis) noexcept;
    void setEnvelope (float attackMs, float releaseMs) noexcept;
    void setKeyTrack (bool on) noexcept { keyTrack = on; }
    /** Adds all voices into L/R. offline = wait for the render threads instead of dropping out. */
    void render (float* L, float* R, int n, bool offline) noexcept;

    // ---- any thread (UI)
    bool transportActive() const noexcept { return transportOn.load(); }
    float transportFrac() const noexcept;
    /** Forward-source positions of sounding voices (0..1); returns count. */
    int positions (float* out, int maxCount) const noexcept;
    int underruns() const noexcept { return underrunCount.load(); }

private:
    struct Voice
    {
        // audio thread
        bool active = false, releasing = false, transport = false, sustained = false;
        int note = -1;
        float velocity = 1, quickRelease = 0;
        uint64_t age = 0;
        juce::ADSR env;

        // handshake
        std::atomic<uint32_t> gen { 0 }, ack { 0 };
        std::atomic<int> cmd { 0 };                 // 1 start, 2 jump, 3 stop
        std::atomic<double> cmdFrac { 0 };
        std::atomic<uint32_t> cmdSeed { 1 };
        std::atomic<int64_t> cmdTime { 0 };
        std::atomic<float> noteSemis { 0 };
        std::atomic<bool> ended { false };

        // FIFO (worker writes, audio reads)
        juce::AbstractFifo fifo { kFifoSize };
        std::vector<float> bufL, bufR;

        // worker thread
        StretchStream stream;
        SampleData::Ptr data;
        uint32_t wgen = 0;
        bool streaming = false;
        int64_t t0 = 0, rendered = 0;               // output time of the next sample this voice renders

        // UI
        std::atomic<bool> sounding { false };
        std::atomic<float> srcFrac { 0 }, posFrac { 0 };
    };

    class Worker;
    friend class Worker;

    void command (Voice& v, int c, double frac, uint32_t seed) noexcept;
    int workerPass (int index, int count, float* tL, float* tR);
    void finishVoice (Voice& v) noexcept;

    Voice voices[kMaxVoices];
    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<float> tmpL, tmpR;
    double rate = 48000;
    int target = 8192;
    uint64_t ageCounter = 0;
    bool sustainDown = false, keyTrack = true;
    float bend = 0, attack = 40, release = 1500;
    std::atomic<bool> transportOn { false };
    std::atomic<int> underrunCount { 0 };
};

} // namespace ss
