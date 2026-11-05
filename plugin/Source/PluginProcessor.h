#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "Params.h"
#include "VoiceEngine.h"
#include "Offline.h"

class SlowSippinProcessor : public juce::AudioProcessor,
                            public juce::ChangeBroadcaster,
                            private juce::Timer
{
public:
    SlowSippinProcessor();
    ~SlowSippinProcessor() override;

    // ---- AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 30.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- sample + region (message thread)
    void loadSample (const juce::File& f, int regionA = 0, int regionB = 0, float restoreStart = -1.0f);
    ss::SampleData::Ptr getSample() const;
    bool isLoading() const noexcept { return loading.load(); }
    juce::String getLoadError() const;
    void setRegion (int a, int b);           // b <= a clears
    int regionA() const noexcept { return shared.regionA.load(); }
    int regionB() const noexcept { return shared.regionB.load(); }
    bool hasRegion() const noexcept { return shared.regionB.load() > shared.regionA.load(); }

    // ---- transport (any thread → audio thread)
    void play() noexcept { playReq.store (true); }
    void stop() noexcept { stopReq.store (true); }
    void togglePlay() noexcept { if (engine.transportActive()) stop(); else play(); }
    void toggleAudition() noexcept { auditionReq.store (true); }
    void setStartFrac (float f) noexcept;     // jumps if playing
    float getStartFrac() const noexcept { return startFrac.load(); }
    bool isPlaying() const noexcept { return engine.transportActive(); }
    float transportFrac() const noexcept { return engine.transportFrac(); }
    bool isAuditioning() const noexcept { return auditionOn.load(); }
    float auditionFrac() const noexcept { return auditionPosFrac.load(); }
    int voicePositions (float* out, int max) const noexcept { return engine.positions (out, max); }

    // ---- export
    ss::OfflineSettings offlineSettings() const;
    ss::Exporter exporter;
    int exportFormat = 0, exportBits = 24;     // remembered with the session

    /** Output scope: peak per ~10 ms, for the live display. */
    int readScope (float* dest, int max);
    float outputLevel() const noexcept { return outLevel.load(); }

    juce::AudioProcessorValueTreeState apvts;
    juce::AudioFormatManager formats;

private:
    void timerCallback() override;
    float param (const char* id) const noexcept { return params.at (id)->load(); }
    void startAudition() noexcept;

    std::map<juce::String, std::atomic<float>*> params;
    ss::EngineShared shared;
    ss::VoiceEngine engine;
    ss::Analyzer analyzer;

    mutable juce::CriticalSection sampleLock;
    ss::SampleData::Ptr sample;
    std::atomic<int> sampleVersion { 0 };
    std::atomic<int> seenVersion { 0 };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    std::atomic<bool> loading { false };
    juce::String loadError;
    std::unique_ptr<juce::Thread> loader;

    // audio-thread copies
    ss::SampleData::Ptr audioSample;          // only swapped under try-lock; released via `retired`
    juce::Array<ss::SampleData::Ptr> retired;

    std::atomic<bool> playReq { false }, stopReq { false }, auditionReq { false };
    std::atomic<float> startFrac { 0 }, jumpReq { -1 };
    std::atomic<bool> auditionOn { false };
    std::atomic<float> auditionPosFrac { 0 };
    double auditionPos = 0; int auditionA = 0, auditionB = 0;

    double sr = 48000;
    int maxBlock = 512;
    juce::AudioBuffer<float> mix, wet;
    juce::dsp::Convolution reverb { juce::dsp::Convolution::NonUniform { 1024 } };
    juce::dsp::Limiter<float> limiter;
    juce::SmoothedValue<float> sDry, sWet, sGain, sNorm;
    float loadedIrSize = -1;
    std::atomic<float> irSizeWanted { 2.5f };

    // analyzer bookkeeping (message thread)
    juce::String lastAnalysisKey;
    int analysisDelay = 0;

    juce::AbstractFifo scopeFifo { 2048 };
    std::vector<float> scopeBuf = std::vector<float> (2048);
    float scopeAcc = 0; int scopeCount = 0;
    std::atomic<float> outLevel { 0 };

    juce::Random rng;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlowSippinProcessor)
};
