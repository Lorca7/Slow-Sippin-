/*
 * Offline work, on background threads:
 *   Analyzer  — renders a stretch of the current settings to find its peak,
 *               which drives NORMALIZE while playing.
 *   Exporter  — renders the whole one-shot (region × stretch, plus reverb
 *               tail) and writes WAV / AIFF at 16 (dithered), 24 or 32-bit float.
 */
#pragma once

#include "StretchEngine.h"
#include "SampleData.h"
#include <atomic>

namespace ss
{

SourceView makeView (const SampleData& d, int a, int b) noexcept;

/** Write interleaved PCM. format: 0 = WAV, 1 = AIFF. bits: 16 | 24 | 32 (float). */
bool writeAudioFile (const juce::File& f, int format, int bits, const std::vector<const float*>& chans,
                     size_t frames, double sampleRate, float gain, juce::String& error);

struct OfflineSettings
{
    SampleData::Ptr data;
    int regionA = 0, regionB = 0;
    StreamSettings stream;
    float reverbPct = 0, sizeSec = 2.5f, gainDb = 0;
    bool normalize = true;
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
    ~Exporter() override { stopThread (5000); }

    static constexpr int64_t kMaxSamples = 64LL * 1024 * 1024;

    /** Output frames/channels a job would produce (for the UI estimate). */
    static void estimate (const OfflineSettings& s, int64_t& frames, int& channels, double& rate);

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
