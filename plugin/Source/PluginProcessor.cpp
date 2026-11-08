#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
constexpr float kNormTarget = 0.94406f;      // −0.5 dBFS
constexpr uint32_t kIrSeed = 90210;

/** Loads a sample off the message thread and hands it back via callAsync. */
class LoaderThread : public juce::Thread
{
public:
    LoaderThread (juce::File f, juce::AudioFormatManager& fm, std::function<void (ss::SampleData::Ptr, juce::String)> done)
        : juce::Thread ("Slow Sippin loader"), file (std::move (f)), formats (fm), onDone (std::move (done)) {}

    void run() override
    {
        juce::String err;
        auto d = ss::SampleData::load (file, formats, err);
        if (! threadShouldExit()) onDone (d, err);
    }

private:
    juce::File file;
    juce::AudioFormatManager& formats;
    std::function<void (ss::SampleData::Ptr, juce::String)> onDone;
};
}

SlowSippinProcessor::SlowSippinProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", ss::createLayout())
{
    formats.registerBasicFormats();
    for (const auto& c : ss::controlSpecs()) params[c.id] = apvts.getRawParameterValue (c.id);
    for (const auto& t : ss::toggleSpecs()) params[t.id] = apvts.getRawParameterValue (t.id);
    for (const char* id : { "fxA", "fxB", "fxRoute", "padX", "padY" }) params[id] = apvts.getRawParameterValue (id);
    engine.shared = &shared;
    shared.mod = &mod;
    for (int t = 0; t < ss::kNumTargets; ++t) targetParam[(size_t) t] = apvts.getRawParameterValue (ss::targetParamId (t));
    for (int i = 0; i < ss::kNumLfos; ++i)
    {
        lfoRateParam[i] = apvts.getRawParameterValue (ss::lfoRateId (i));
        lfoShapeParam[i] = apvts.getRawParameterValue (ss::lfoShapeId (i));
    }
    engine.currentSample = [this] { return getSample(); };
    startTimerHz (10);
}

SlowSippinProcessor::~SlowSippinProcessor()
{
    alive->store (false);
    stopTimer();
    if (loader) loader->stopThread (10000);
    engine.shutdown();
}

bool SlowSippinProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet().isDisabled();
}

// --------------------------------------------------------------- prepare --

void SlowSippinProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    maxBlock = juce::jmax (32, samplesPerBlock);
    engine.prepare (sr, maxBlock);
    mix.setSize (2, maxBlock);
    wet.setSize (2, maxBlock);
    const juce::dsp::ProcessSpec spec { sr, (juce::uint32) maxBlock, 2 };
    reverb.prepare (spec);
    fx.prepare (sr, maxBlock);
    chunkWet.assign ((size_t) (maxBlock / kModChunk + 2), 0.0f);
    chunkGain.assign ((size_t) (maxBlock / kModChunk + 2), 1.0f);
    limiter.prepare (spec);
    limiter.setThreshold (-0.5f);
    limiter.setRelease (80.0f);
    for (auto* s : { &sDry, &sWet, &sGain, &sNorm }) s->reset (sr, 0.05);
    sNorm.reset (sr, 0.3);
    sDry.setCurrentAndTargetValue (1.0f);
    sWet.setCurrentAndTargetValue (0.0f);
    sGain.setCurrentAndTargetValue (1.0f);
    sNorm.setCurrentAndTargetValue (1.0f);

    std::vector<float> l, r;
    loadedIrSize = param ("size");
    ss::makeImpulse (sr, loadedIrSize, kIrSeed, l, r);
    juce::AudioBuffer<float> ir (2, (int) l.size());
    ir.copyFrom (0, 0, l.data(), (int) l.size());
    ir.copyFrom (1, 0, r.data(), (int) r.size());
    reverb.loadImpulseResponse (std::move (ir), sr, juce::dsp::Convolution::Stereo::yes,
                                juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    lastAnalysisKey = {};
}

void SlowSippinProcessor::releaseResources()
{
    engine.shutdown();
}

// --------------------------------------------------------------- process --

void SlowSippinProcessor::startAudition() noexcept
{
    if (audioSample == nullptr) return;
    const auto v = ss::makeView (*audioSample, shared.regionA.load(), shared.regionB.load());
    auditionA = (int) (v.fwd[0] - audioSample->fwd.getReadPointer (0));
    auditionB = auditionA + v.len;
    auditionPos = auditionA;
    auditionOn.store (true);
}

void SlowSippinProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();
    if (buffer.getNumChannels() < 2) return;
    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);
    const bool offline = isNonRealtime();

    // new sample? (try-lock: never block the audio thread)
    {
        const juce::ScopedTryLock sl (sampleLock);
        if (sl.isLocked() && seenVersion != sampleVersion.load())
        {
            audioSample = sample;          // old one stays alive in `retired`
            seenVersion = sampleVersion.load();
            engine.killAll();
            auditionOn.store (false);
        }
    }

    // parameters → engine (the render threads add LFO modulation themselves, at their own time)
    shared.stretch.store (param ("stretch"));
    shared.pitch.store (param ("pitch"));
    shared.weave.store ((int) std::lround (param ("weave")));
    shared.scatter.store (param ("scatter") / 100.0f);
    shared.driftMs.store (param ("drift"));
    shared.window.store (param ("window"));
    shared.loop.store (param ("loop") > 0.5f);
    shared.reverse.store (param ("reverse") > 0.5f);
    shared.monoStereo.store (param ("monoStereo") > 0.5f);
    engine.setKeyTrack (param ("keyTrack") > 0.5f);

    // LFOs: advance and publish the shared clock
    {
        float rates[ss::kNumLfos];
        int shapes[ss::kNumLfos];
        for (int i = 0; i < ss::kNumLfos; ++i)
        {
            rates[i] = lfoRateParam[i]->load();
            shapes[i] = juce::jlimit (0, ss::kNumShapes - 1, (int) std::lround (lfoShapeParam[i]->load()));
        }
        mod.advance (clock, n, sr, rates, shapes);
    }
    float lfoNow[ss::kNumLfos];
    mod.lfoValuesAt (clock, lfoNow);
    engine.setEnvelope (mod.apply (ss::tAttack, param ("attack"), lfoNow), mod.apply (ss::tRelease, param ("release"), lfoNow));

    // transport requests from the UI
    if (stopReq.exchange (false)) engine.stopTransport();
    if (playReq.exchange (false) && audioSample != nullptr && ! engine.transportActive())
        engine.noteOn (60, 1.0f, true, startFrac.load(), (uint32_t) rng.nextInt() | 1u, clock);
    const float j = jumpReq.exchange (-1.0f);
    if (j >= 0) engine.jumpTransport (j);
    if (auditionReq.exchange (false))
    {
        if (auditionOn.load()) auditionOn.store (false);
        else startAudition();
    }

    // MIDI, sample-accurate
    int pos = 0;
    for (const auto meta : midi)
    {
        const int t = juce::jlimit (0, n, meta.samplePosition);
        if (t > pos) { engine.render (L + pos, R + pos, t - pos, offline); pos = t; }
        const auto m = meta.getMessage();
        if (m.isNoteOn())
        {
            if (audioSample != nullptr)
                engine.noteOn (m.getNoteNumber(), 0.25f + 0.75f * m.getFloatVelocity(), false, startFrac.load(), (uint32_t) rng.nextInt() | 1u, clock + t);
        }
        else if (m.isNoteOff()) engine.noteOff (m.getNoteNumber());
        else if (m.isSustainPedalOn()) engine.setSustain (true);
        else if (m.isSustainPedalOff()) engine.setSustain (false);
        else if (m.isAllNotesOff() || m.isAllSoundOff()) engine.killAll();
        else if (m.isPitchWheel()) engine.setBend ((m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f);
    }
    if (pos < n) engine.render (L + pos, R + pos, n - pos, offline);

    // post: normalize → effects (XY blend) → reverb → gain → (limiter), modulated every 64 samples
    const bool normalize = param ("normalize") > 0.5f;
    const float pk = analyzer.peak();
    sNorm.setTargetValue (normalize && pk > 1e-6f ? juce::jmin (100.0f, kNormTarget / pk) : 1.0f);

    const auto fxSetup = currentFxSetup();
    for (int off = 0; off < n; off += maxBlock)
    {
        const int len = juce::jmin (maxBlock, n - off);
        bool anyWet = sWet.isSmoothing() || sWet.getCurrentValue() > 0.0f;
        int nc = 0;
        for (int c = 0; c < len; c += kModChunk, ++nc)
        {
            const int cl = juce::jmin (kModChunk, len - c);
            float lfo[ss::kNumLfos];
            mod.lfoValuesAt (clock + off + c, lfo);
            auto mv = [&] (int t) { return mod.apply (t, targetParam[(size_t) t]->load(), lfo); };
            float base[ss::kNumTargets];
            for (int t = 0; t < ss::kNumTargets; ++t) base[t] = targetParam[(size_t) t]->load();
            const auto fp = ss::makeFxParams (fxSetup, mod, base, lfo, nullptr);
            chunkWet[(size_t) nc] = mv (ss::tReverb) / 100.0f;
            chunkGain[(size_t) nc] = juce::Decibels::decibelsToGain (mv (ss::tGain), -100.0f);
            if (chunkWet[(size_t) nc] > 0.0f) anyWet = true;
            for (int i = 0; i < cl; ++i)
            {
                const float g = sNorm.getNextValue();
                L[off + c + i] *= g;
                R[off + c + i] *= g;
            }
            fx.process (L + off + c, R + off + c, cl, fp);
        }
        if (anyWet)
        {
            wet.copyFrom (0, 0, L + off, len);
            wet.copyFrom (1, 0, R + off, len);
            juce::dsp::AudioBlock<float> wb (wet.getArrayOfWritePointers(), 2, (size_t) len);
            reverb.process (juce::dsp::ProcessContextReplacing<float> (wb));
        }
        const float* wL = wet.getReadPointer (0);
        const float* wR = wet.getReadPointer (1);
        nc = 0;
        for (int c = 0; c < len; c += kModChunk, ++nc)
        {
            const int cl = juce::jmin (kModChunk, len - c);
            sWet.setTargetValue (chunkWet[(size_t) nc]);       // REVERB only adds reverb; the dry sound stays
            sGain.setTargetValue (chunkGain[(size_t) nc]);
            for (int i = c; i < c + cl; ++i)
            {
                const float w = sWet.getNextValue(), g = sGain.getNextValue();
                float l = L[off + i], r = R[off + i];
                if (anyWet) { l += wL[i] * w; r += wR[i] * w; }
                L[off + i] = l * g;
                R[off + i] = r * g;
            }
        }
        if (normalize)
        {
            float* chans[] = { L + off, R + off };
            juce::dsp::AudioBlock<float> ob (chans, 2, (size_t) len);
            limiter.process (juce::dsp::ProcessContextReplacing<float> (ob));
        }
    }

    // audition: the raw region, dry
    if (auditionOn.load() && audioSample != nullptr)
    {
        const auto& d = *audioSample;
        const double step = d.sampleRate / sr;
        const float* s0 = d.fwd.getReadPointer (0);
        const float* s1 = d.fwd.getReadPointer (d.numChannels > 1 ? 1 : 0);
        for (int i = 0; i < n; ++i)
        {
            const int k = (int) auditionPos;
            if (k + 1 >= auditionB) { auditionOn.store (false); break; }
            const float f = (float) (auditionPos - k);
            L[i] += s0[k] + (s0[k + 1] - s0[k]) * f;
            R[i] += s1[k] + (s1[k + 1] - s1[k]) * f;
            auditionPos += step;
        }
        auditionPosFrac.store ((float) (auditionPos / juce::jmax (1, d.length)));
    }

    // scope + level
    float blockPeak = 0;
    for (int i = 0; i < n; ++i)
    {
        const float v = juce::jmax (std::abs (L[i]), std::abs (R[i]));
        blockPeak = juce::jmax (blockPeak, v);
        scopeAcc = juce::jmax (scopeAcc, v);
        if (++scopeCount >= (int) (sr / 100))
        {
            if (scopeFifo.getFreeSpace() > 0)
            {
                const auto h = scopeFifo.write (1);
                if (h.blockSize1 > 0) scopeBuf[(size_t) h.startIndex1] = scopeAcc;
                else if (h.blockSize2 > 0) scopeBuf[(size_t) h.startIndex2] = scopeAcc;
            }
            scopeAcc = 0;
            scopeCount = 0;
        }
    }
    outLevel.store (juce::jmax (blockPeak, outLevel.load() * 0.9f));
    recorder.process (L, R, n);            // exactly what is heard, after everything
    clock += n;
    audioClock.store (clock);
}

int SlowSippinProcessor::readScope (float* dest, int max)
{
    const int k = juce::jmin (max, scopeFifo.getNumReady());
    const auto h = scopeFifo.read (k);
    int o = 0;
    for (int i = 0; i < h.blockSize1; ++i) dest[o++] = scopeBuf[(size_t) (h.startIndex1 + i)];
    for (int i = 0; i < h.blockSize2; ++i) dest[o++] = scopeBuf[(size_t) (h.startIndex2 + i)];
    return o;
}

// ------------------------------------------------------- message thread --

void SlowSippinProcessor::timerCallback()
{
    // release old samples once nobody else holds them
    {
        const juce::ScopedLock sl (sampleLock);
        for (int i = retired.size(); --i >= 0;)
            if (retired.getReference (i)->getReferenceCount() == 1 && seenVersion == sampleVersion.load())
                retired.remove (i);
    }

    // reverb size changed → new impulse (loaded in the background by juce::dsp::Convolution)
    const float size = param ("size");
    if (std::abs (size - loadedIrSize) > 1e-4f && sr > 0)
    {
        if (irSizeWanted.exchange (size) == size)
        {
            loadedIrSize = size;
            std::vector<float> l, r;
            ss::makeImpulse (sr, size, kIrSeed, l, r);
            juce::AudioBuffer<float> ir (2, (int) l.size());
            ir.copyFrom (0, 0, l.data(), (int) l.size());
            ir.copyFrom (1, 0, r.data(), (int) r.size());
            reverb.loadImpulseResponse (std::move (ir), sr, juce::dsp::Convolution::Stereo::yes,
                                        juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
        }
    }

    // NORMALIZE: re-measure the peak once the engine settings have settled
    const auto s = offlineSettings();
    juce::String key;
    key << sampleVersion.load() << '|' << s.regionA << '|' << s.regionB << '|' << s.stream.stretch << '|' << s.stream.pitchSemis
        << '|' << s.stream.weave << '|' << s.stream.scatter << '|' << s.stream.driftMs << '|' << (int) s.stream.reverse
        << '|' << (int) s.stream.monoToStereo << '|' << sr;
    if (key != lastAnalysisKey)
    {
        lastAnalysisKey = key;
        analysisDelay = 4;
    }
    else if (analysisDelay > 0 && --analysisDelay == 0)
    {
        analyzer.request (s, sr);
    }
}

void SlowSippinProcessor::loadSample (const juce::File& f, int a, int b, float restoreStart)
{
    if (loader) loader->stopThread (10000);
    loading.store (true);
    sendChangeMessage();
    loader = std::make_unique<LoaderThread> (f, formats, [this, a, b, restoreStart, flag = alive] (ss::SampleData::Ptr d, juce::String err)
    {
        juce::MessageManager::callAsync ([this, d, err, a, b, restoreStart, flag]
        {
            if (! flag->load()) return;          // plugin was closed meanwhile
            if (d != nullptr)
            {
                {
                    const juce::ScopedLock sl (sampleLock);
                    if (sample != nullptr) retired.add (sample);
                    sample = d;
                    shared.regionA.store (0);
                    shared.regionB.store (0);
                    sampleVersion.fetch_add (1);
                    loadError = {};
                }
                if (b > a && b <= d->length) setRegion (a, b);
                startFrac.store (restoreStart >= 0 ? restoreStart : 0.0f);
            }
            else
            {
                const juce::ScopedLock sl (sampleLock);
                loadError = err;
            }
            loading.store (false);
            sendChangeMessage();
        });
    });
    loader->startThread();
}

ss::SampleData::Ptr SlowSippinProcessor::getSample() const
{
    const juce::ScopedLock sl (sampleLock);
    return sample;
}

juce::String SlowSippinProcessor::getLoadError() const
{
    const juce::ScopedLock sl (sampleLock);
    return loadError;
}

void SlowSippinProcessor::setRegion (int a, int b)
{
    auto d = getSample();
    if (d == nullptr) return;
    a = juce::jlimit (0, d->length, a);
    b = juce::jlimit (0, d->length, b);
    if (b < a) std::swap (a, b);
    if (b - a < (int) (d->sampleRate * 0.01) || (a == 0 && b == d->length)) a = b = 0;
    shared.regionA.store (a);
    shared.regionB.store (b);
}

void SlowSippinProcessor::setStartFrac (float f) noexcept
{
    f = juce::jlimit (0.0f, 1.0f, f);
    startFrac.store (f);
    if (engine.transportActive()) jumpReq.store (f);
}

ss::OfflineSettings SlowSippinProcessor::offlineSettings() const
{
    ss::OfflineSettings s;
    s.data = getSample();
    s.regionA = shared.regionA.load();
    s.regionB = shared.regionB.load();
    s.stream.stretch = param ("stretch");
    s.stream.pitchSemis = param ("pitch");
    s.stream.weave = (int) std::lround (param ("weave"));
    s.stream.scatter = param ("scatter") / 100.0f;
    s.stream.driftMs = param ("drift");
    s.stream.loop = false;
    s.stream.reverse = param ("reverse") > 0.5f;
    s.stream.monoToStereo = param ("monoStereo") > 0.5f;
    s.reverbPct = param ("reverb");
    s.sizeSec = param ("size");
    s.gainDb = param ("gain");
    s.normalize = param ("normalize") > 0.5f;
    for (int t = 0; t < ss::kNumTargets; ++t) s.base[(size_t) t] = targetParam[(size_t) t]->load();
    for (int i = 0; i < ss::kNumLfos; ++i)
    {
        s.lfoRate[i] = lfoRateParam[i]->load();
        s.lfoShape[i] = juce::jlimit (0, ss::kNumShapes - 1, (int) std::lround (lfoShapeParam[i]->load()));
    }
    s.routes = mod.snapshotRoutes();
    s.mod = &mod;
    const float pk = analyzer.peak();
    s.preGain = s.normalize && pk > 1e-6f ? juce::jmin (100.0f, kNormTarget / pk) : 1.0f;
    s.fx = currentFxSetup();
    return s;
}

ss::FxSetup SlowSippinProcessor::currentFxSetup() const noexcept
{
    ss::FxSetup f;
    f.typeA = juce::jlimit (0, ss::kNumFxTypes - 1, (int) std::lround (param ("fxA")));
    f.typeB = juce::jlimit (0, ss::kNumFxTypes - 1, (int) std::lround (param ("fxB")));
    f.series = param ("fxRoute") < 0.5f;
    f.padX = ss::padChoiceToTarget ((int) std::lround (param ("padX")));
    f.padY = ss::padChoiceToTarget ((int) std::lround (param ("padY")));
    return f;
}

// ----------------------------------------------------------------- state --

void SlowSippinProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.removeChild (state.getChildWithName ("SESSION"), nullptr);
    juce::ValueTree session ("SESSION");
    if (auto d = getSample()) session.setProperty ("sample", d->file.getFullPathName(), nullptr);
    session.setProperty ("regionA", shared.regionA.load(), nullptr);
    session.setProperty ("regionB", shared.regionB.load(), nullptr);
    session.setProperty ("startFrac", startFrac.load(), nullptr);
    session.setProperty ("exportFormat", exportFormat, nullptr);
    session.setProperty ("exportBits", exportBits, nullptr);
    session.setProperty ("tab", uiTab, nullptr);
    session.appendChild (mod.toTree(), nullptr);
    state.appendChild (session, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void SlowSippinProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr) return;
    auto tree = juce::ValueTree::fromXml (*xml);
    if (! tree.hasType (apvts.state.getType())) return;
    auto session = tree.getChildWithName ("SESSION").createCopy();
    tree.removeChild (tree.getChildWithName ("SESSION"), nullptr);
    apvts.replaceState (tree);

    mod.fromTree (session.getChildWithName ("MOD"));
    exportFormat = session.getProperty ("exportFormat", 0);
    exportBits = session.getProperty ("exportBits", 24);
    uiTab = session.getProperty ("tab", 0);
    startFrac.store ((float) (double) session.getProperty ("startFrac", 0.0));
    const juce::String path = session.getProperty ("sample", "");
    const int a = session.getProperty ("regionA", 0), b = session.getProperty ("regionB", 0);
    if (path.isNotEmpty())
    {
        const juce::File f (path);
        const float keepStart = startFrac.load();
        juce::MessageManager::callAsync ([this, f, a, b, keepStart, flag = alive]
        {
            if (! flag->load()) return;
            if (! f.existsAsFile())
            {
                const juce::ScopedLock sl (sampleLock);
                loadError = "missing sample: " + f.getFullPathName();
                sendChangeMessage();
                return;
            }
            loadSample (f, a, b, keepStart);
        });
    }
}

juce::AudioProcessorEditor* SlowSippinProcessor::createEditor()
{
    return new SlowSippinEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SlowSippinProcessor();
}
