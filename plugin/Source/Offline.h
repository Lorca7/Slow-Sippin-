/*
 * Offline work, on background threads:
 *   Analyzer  — renders a stretch of the current settings to find its peak,
 *               which drives NORMALIZE while playing.
 *   Exporter  — renders the whole one-shot (region × stretch, plus reverb
 *               tail) and writes WAV / AIFF at 16 (dithered), 24 or 32-bit float.
 *
 * The exporter streams: the stretch is rendered in blocks, the reverb is a
 * streaming block convolution, and the result goes to a temporary 32-bit
 * float file while the peak is measured. A second pass applies normalize +
 * gain and writes the final file. Memory use is constant, so hour-long
 * (or longer) renders are fine; WAV over 4 GB is written as RF64.
 */
#pragma once

#include "StretchEngine.h"
#include "SampleData.h"
#include "Modulation.h"
#include "Effects.h"
#include <atomic>

namespace ss
{

SourceView makeView (const SampleData& d, int a, int b) noexcept;

/** Streaming PCM file writer. format: 0 = WAV (RF64 if > 4 GB), 1 = AIFF. bits: 16 | 24 | 32 (float). */
class PcmWriter
{
public:
    bool open (const juce::File& f, int format, int bits, int channels, uint64_t frames, double sampleRate, juce::String& error);
    /** Interleaved frames; scaled by gain, dithered at 16-bit. */
    bool write (const float* interleaved, size_t frames, float gain);
    bool close (juce::String& error);

private:
    std::unique_ptr<juce::FileOutputStream> out;
    int fmt = 0, bits = 24, ch = 2, bps = 3;
    std::vector<uint8_t> buf;
    Rng rng { 0x1234567u };
};

/** Convenience: write whole channels at once (used by tests). */
bool writeAudioFile (const juce::File& f, int format, int bits, const std::vector<const float*>& chans,
                     size_t frames, double sampleRate, float gain, juce::String& error);

/** Streaming FFT convolution: feed fixed-size blocks, get the aligned convolved block back. */
class StreamConvolver
{
public:
    void prepare (const std::vector<float>& ir);
    int blockSize() const noexcept { return B; }
    /** in/out: blockSize() samples. */
    void process (const float* in, float* out);

private:
    int n = 0, B = 0, H = 0;
    RealFFT fft;
    std::vector<double> hr, hi, xr, xi, buf, acc;
};

/** Effect slots, routing, and which parameter each XY-pad axis drives (Target, -1 = off). */
struct FxSetup
{
    int typeA = FxFilter, typeB = FxEcho;
    bool series = true;
    int padX = tFltCut, padY = tEchoFb;
};

/**
 * Effect parameters at one moment: base values; a parameter mapped to a pad
 * axis takes the pad position across its whole range instead; then LFOs routed
 * to that parameter are added. routes == nullptr: use the live route table.
 */
inline FxParams makeFxParams (const FxSetup& s, const ModSystem& mod, const float* base, const float* lfo, const RouteList* routes)
{
    auto ap = [&] (int t, float b) { return routes != nullptr ? mod.apply (t, b, lfo, *routes) : mod.apply (t, b, lfo); };
    const float px = juce::jlimit (0.0f, 1.0f, ap (tFxX, base[tFxX]) / 100.0f);
    const float py = juce::jlimit (0.0f, 1.0f, ap (tFxY, base[tFxY]) / 100.0f);
    auto v = [&] (int t)
    {
        float b = base[t];
        if (s.padX == t) b = mod.fromNorm (t, px);
        if (s.padY == t) b = mod.fromNorm (t, py);
        return ap (t, b);
    };
    FxParams p;
    p.typeA = s.typeA;
    p.typeB = s.typeB;
    p.series = s.series;
    p.mix = v (tFxMix) / 100.0f;
    p.balance = v (tFxBal) / 100.0f;
    p.chRate = v (tChRate);
    p.chDepth = v (tChDepth) / 100.0f;
    p.echoTimeMs = v (tEchoTime);
    p.echoFb = v (tEchoFb) / 100.0f;
    p.echoTone = v (tEchoTone) / 100.0f;
    p.cutHz = v (tFltCut);
    p.res = v (tFltRes) / 100.0f;
    p.type = v (tFltType) / 100.0f;
    p.blur = v (tSpBlur) / 100.0f;
    p.shiftSt = v (tSpShift);
    p.thin = v (tSpThin) / 100.0f;
    return p;
}

struct OfflineSettings
{
    SampleData::Ptr data;
    int regionA = 0, regionB = 0;
    StreamSettings stream;
    float reverbPct = 0, sizeSec = 2.5f, gainDb = 0;
    bool normalize = true;

    // effects + modulation (export renders what you hear)
    std::array<float, kNumTargets> base {};    // every modulatable parameter, in its own units
    float lfoRate[kNumLfos] {};
    int lfoShape[kNumLfos] {};
    RouteList routes {};
    const ModSystem* mod = nullptr;            // ranges only
    float preGain = 1;                         // live NORMALIZE level, so the effects see the same level
    FxSetup fx;
};

class Analyzer : private juce::Thread
{
public:
    Analyzer() : juce::Thread ("Slow Sippin analyzer") { startThread (juce::Thread::Priority::low); }
    ~Analyzer() override { stopThread (3000); }

    void request (const OfflineSettings& s, double outputRate);
    /** Peak of the stretched output (before reverb/gain); 0 until known. */
    float peak() const noexcept { return lastPeak.load(); }

private:
    void run() override;
    juce::CriticalSection lock;
    OfflineSettings pending;
    double pendingRate = 48000;
    bool hasPending = false;
    juce::WaitableEvent wake;
    std::atomic<float> lastPeak { 0 };
};

class Exporter : private juce::Thread
{
public:
    enum State { Idle, Running, Done, Failed, Cancelled };

    Exporter() : juce::Thread ("Slow Sippin export") {}
    ~Exporter() override { stopThread (10000); }

    /** Longest render accepted: 12 hours. */
    static constexpr double kMaxSeconds = 12.0 * 3600.0;

    /** Output frames/channels a job would produce (for the UI estimate). */
    static void estimate (const OfflineSettings& s, int64_t& frames, int& channels, double& rate);
    static bool tooLong (int64_t frames, double rate) { return rate > 0 && (double) frames / rate > kMaxSeconds; }

    bool start (const OfflineSettings& s, const juce::File& file, int format, int bits);
    void cancel() { signalThreadShouldExit(); }
    bool isBusy() const { return isThreadRunning(); }

    float progress() const noexcept { return prog.load(); }
    State state() const noexcept { return (State) st.load(); }
    juce::String message() const { const juce::ScopedLock sl (msgLock); return msg; }
    juce::File lastFile() const { const juce::ScopedLock sl (msgLock); return outFile; }

private:
    void run() override;
    void finish (State s, const juce::String& m);

    OfflineSettings job;
    juce::File outFile;
    int fmt = 0, bitDepth = 24;
    std::atomic<float> prog { 0 };
    std::atomic<int> st { Idle };
    juce::CriticalSection msgLock;
    juce::String msg;
};

} // namespace ss
