/*
 * HostTest — loads the built VST3 like a DAW would and exercises it:
 * sample loading via saved state, MIDI notes/chords/release, live parameter
 * changes, offline (bounce) rendering, state round-trip, editor snapshot,
 * plus the file exporter.   Usage: HostTest <plugin.vst3> <sample.wav> <outdir>
 */
#include <juce_audio_processors/juce_audio_processors.h>
#include <thread>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Source/Offline.h"
#include "../Source/Params.h"
#include "../Source/Recorder.h"

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("  %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

static void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

struct Stats { float peak = 0; double rms = 0; bool finite = true; };

static Stats render (juce::AudioPluginInstance& p, double seconds, juce::MidiBuffer first = {}, std::vector<float>* keep = nullptr)
{
    const int block = 256;
    juce::AudioBuffer<float> buf (2, block);
    Stats s;
    double sum = 0; int64_t n = 0;
    const int blocks = (int) (seconds * 48000 / block);
    for (int b = 0; b < blocks; ++b)
    {
        buf.clear();
        juce::MidiBuffer m;
        if (b == 0) m = first;
        p.processBlock (buf, m);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < block; ++i)
            {
                const float v = buf.getSample (c, i);
                if (! std::isfinite (v)) s.finite = false;
                s.peak = juce::jmax (s.peak, std::abs (v));
                sum += v * v; ++n;
            }
        if (keep) for (int i = 0; i < block; ++i) keep->push_back (buf.getSample (0, i));
        if (! p.isNonRealtime()) { if (b % 4 == 0) pump (1); else juce::Thread::sleep (2); }
    }
    s.rms = std::sqrt (sum / juce::jmax<int64_t> (1, n));
    return s;
}

static void setChoiceParam (juce::AudioPluginInstance& p, const juce::String& name, int index)
{
    for (auto* prm : p.getParameters())
        if (prm->getName (64).equalsIgnoreCase (name))
        {
            const int steps = juce::jmax (2, prm->getNumSteps());
            prm->setValueNotifyingHost ((float) index / (float) (steps - 1));
            return;
        }
    check (false, "choice parameter not found: " + name);
}

static void setParam (juce::AudioPluginInstance& p, const juce::String& name, float value)
{
    for (auto* prm : p.getParameters())
        if (prm->getName (64).equalsIgnoreCase (name))
            if (auto* r = dynamic_cast<juce::HostedAudioProcessorParameter*> (prm))
            {
                if (prm->isBoolean() || prm->getNumSteps() == 2)     // switches show "On"/"Off", not numbers
                {
                    prm->setValueNotifyingHost (value > 0.5f ? 1.0f : 0.0f);
                    return;
                }
                // hosted params speak 0..1: search the normalised value whose text matches
                float best = 0, bestErr = 1e9f;
                for (int i = 0; i <= 4000; ++i)
                {
                    const float t = i / 4000.0f;
                    const float v = r->getText (t, 32).getFloatValue();
                    if (std::abs (v - value) < bestErr) { bestErr = std::abs (v - value); best = t; }
                }
                prm->setValueNotifyingHost (best);
                return;
            }
    check (false, "parameter not found: " + name);
}

int main (int argc, char** argv)
{
    if (argc < 4) { std::printf ("usage: HostTest <plugin.vst3> <sample> <outdir>\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File pluginFile (argv[1]), sampleFile (argv[2]), outDir (argv[3]);
    outDir.createDirectory();

    juce::AudioPluginFormatManager fm;
    fm.addFormat (new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> descs;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile (descs, pluginFile.getFullPathName());
    check (descs.size() == 1, "VST3 found: " + (descs.isEmpty() ? juce::String ("none") : descs[0]->name + " (" + descs[0]->category + ", instrument=" + juce::String ((int) descs[0]->isInstrument) + ")"));
    if (descs.isEmpty()) return 1;

    juce::String err;
    auto inst = fm.createPluginInstance (*descs[0], 48000, 256, err);
    check (inst != nullptr, "instantiated " + err);
    if (! inst) return 1;
    inst->setPlayConfigDetails (0, 2, 48000, 256);
    inst->prepareToPlay (48000, 256);
    check (inst->getParameters().size() >= 15, "parameters exposed: " + juce::String (inst->getParameters().size()));

    // --- load a sample through saved state (what a DAW project reload does)
    juce::XmlElement params ("PARAMS");
    auto* session = params.createNewChildElement ("SESSION");
    session->setAttribute ("sample", sampleFile.getFullPathName());
    session->setAttribute ("regionA", 0);
    session->setAttribute ("regionB", 0);
    juce::MemoryBlock blob;
    juce::AudioProcessor::copyXmlToBinary (params, blob);
    juce::XmlElement vstState ("VST3PluginState");
    vstState.createNewChildElement ("IComponent")->addTextElement (blob.toBase64Encoding());
    juce::MemoryBlock outer;
    juce::AudioProcessor::copyXmlToBinary (vstState, outer);
    inst->setStateInformation (outer.getData(), (int) outer.getSize());
    pump (1500);

    setParam (*inst, "STRETCH", 30.0f);
    setParam (*inst, "RELEASE", 500.0f);
    setParam (*inst, "ATTACK", 20.0f);
    pump (800);   // let NORMALIZE measure

    std::printf ("\nsingle note:\n");
    juce::MidiBuffer on; on.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
    std::vector<float> wave;
    auto s1 = render (*inst, 4.0, on, &wave);
    check (s1.finite, "finite output");
    check (s1.rms > 0.02, "sounds (rms " + juce::String (s1.rms, 3) + ")");
    check (s1.peak <= 1.0f, "peak " + juce::String (s1.peak, 3) + " <= 1 (normalize + limiter)");
    int firstSound = -1;
    for (size_t i = 0; i < wave.size(); ++i) if (std::abs (wave[i]) > 1e-3f) { firstSound = (int) i; break; }
    check (firstSound >= 0 && firstSound < 48000 / 5, "note starts within 200 ms (" + juce::String (firstSound / 48.0, 1) + " ms)");

    std::printf ("live parameter changes while playing:\n");
    setParam (*inst, "PITCH", 7.0f);
    setParam (*inst, "WEAVE", 12.0f);
    setParam (*inst, "SCATTER", 20.0f);
    setParam (*inst, "DRIFT", 150.0f);
    setParam (*inst, "REVERB", 40.0f);
    auto s2 = render (*inst, 2.0);
    check (s2.finite && s2.rms > 0.02, "still sounding, rms " + juce::String (s2.rms, 3));

    juce::MidiBuffer off; off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
    render (*inst, 1.0, off);
    setParam (*inst, "REVERB", 0.0f);
    Stats s3;
    for (int k = 0; k < 7; ++k)
    {
        s3 = render (*inst, 0.5);
        std::printf ("    after release +%.1fs: rms %.6f peak %.6f\n", 1.0 + 0.5 * (k + 1), s3.rms, s3.peak);
    }
    check (s3.rms < 1e-4, "silent after release (rms " + juce::String (s3.rms, 6) + ")");

    std::printf ("chord:\n");
    juce::MidiBuffer chord;
    for (int nn : { 48, 55, 60, 64, 67 }) chord.addEvent (juce::MidiMessage::noteOn (1, nn, (juce::uint8) 127), 0);
    auto s4 = render (*inst, 3.0, chord);
    check (s4.finite && s4.rms > s1.rms, "5-note chord louder than one note (rms " + juce::String (s4.rms, 3) + ")");
    check (s4.peak <= 1.0f, "chord peak " + juce::String (s4.peak, 3) + " <= 1");
    juce::MidiBuffer allOff; allOff.addEvent (juce::MidiMessage::allNotesOff (1), 0);
    render (*inst, 0.5, allOff);

    std::printf ("reverb only adds (never turns the dry sound down):\n");
    {
        juce::MidiBuffer held; held.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
        setParam (*inst, "NORMALIZE", 0.0f);
        setParam (*inst, "REVERB", 0.0f);
        render (*inst, 1.0, held);
        const auto dry = render (*inst, 2.0);
        setParam (*inst, "REVERB", 100.0f);
        render (*inst, 0.5);
        const auto both = render (*inst, 2.0);
        check (both.rms >= dry.rms * 0.98, "rms with reverb " + juce::String (both.rms, 3) + " >= dry " + juce::String (dry.rms, 3));
        setParam (*inst, "REVERB", 0.0f);
        setParam (*inst, "NORMALIZE", 1.0f);
        render (*inst, 0.3, allOff);
    }

    std::printf ("parameters are independent:\n");
    {
        juce::StringArray ours;
        for (const auto& c : ss::controlSpecs()) ours.add (ss::hostName (c));
        for (const auto& t : ss::toggleSpecs()) ours.add (juce::String::fromUTF8 (t.label));
        for (int i = 0; i < ss::kNumLfos; ++i) ours.add ("LFO " + juce::String (i + 1) + " SHAPE");
        ours.addArray ({ "FX SLOT A", "FX SLOT B", "FX ROUTING", "PAD X CONTROLS", "PAD Y CONTROLS" });
        juce::Array<juce::AudioProcessorParameter*> ps;
        for (auto* prm : inst->getParameters()) if (ours.contains (prm->getName (64))) ps.add (prm);
        check (ps.size() == ours.size(), "found all " + juce::String (ours.size()) + " parameters (" + juce::String (ps.size()) + ")");
        std::vector<float> orig;
        for (auto* prm : ps) orig.push_back (prm->getValue());
        int coupled = 0;
        for (int i = 0; i < ps.size(); ++i)
        {
            std::vector<float> before;
            for (auto* prm : ps) before.push_back (prm->getValue());
            ps[i]->setValueNotifyingHost (std::fmod (before[(size_t) i] + 0.37f, 1.0f));
            render (*inst, 0.05);
            pump (60);
            for (int j = 0; j < ps.size(); ++j)
                if (j != i && std::abs (ps[j]->getValue() - before[(size_t) j]) > 1e-6f)
                {
                    ++coupled;
                    std::printf ("    changing %s moved %s\n", ps[i]->getName (64).toRawUTF8(), ps[j]->getName (64).toRawUTF8());
                }
        }
        check (coupled == 0, "changing any one parameter leaves all the others untouched");
        for (int i = 0; i < ps.size(); ++i) ps[i]->setValueNotifyingHost (orig[(size_t) i]);
        render (*inst, 0.1);
    }

    std::printf ("effects in the plugin (two slots, series and parallel):\n");
    {
        juce::MidiBuffer bOn; bOn.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
        const double dryRms = render (*inst, 2.0, bOn).rms;
        render (*inst, 0.3, allOff);
        std::printf ("    dry (FX MIX 0): rms %.3f\n", dryRms);
        setParam (*inst, "FX MIX", 100.0f);
        setChoiceParam (*inst, "PAD X CONTROLS", 0);      // pad off: the effects use their own settings
        setChoiceParam (*inst, "PAD Y CONTROLS", 0);
        const char* names[] = { "chorus", "echo", "filter", "spectral" };
        for (auto ab : { std::pair<int, int> { 2, 1 }, { 0, 3 }, { 1, 0 }, { 3, 2 } })
            for (int route = 0; route < 2; ++route)
            {
                setChoiceParam (*inst, "FX SLOT A", ab.first);
                setChoiceParam (*inst, "FX SLOT B", ab.second);
                setChoiceParam (*inst, "FX ROUTING", route);
                juce::MidiBuffer nOn; nOn.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
                const auto st = render (*inst, 2.0, nOn);
                render (*inst, 0.3, allOff);
                check (st.finite && st.rms > 0.005 && st.peak <= 1.0f, juce::String (names[ab.first]) + (route == 0 ? " -> " : " | ") + names[ab.second]
                       + ": rms " + juce::String (st.rms, 3) + ", peak " + juce::String (st.peak, 3));
            }

        // the pad plays what its axes are mapped to: X -> filter cutoff
        setChoiceParam (*inst, "FX SLOT A", 2);
        setChoiceParam (*inst, "FX SLOT B", 3);
        setChoiceParam (*inst, "FX ROUTING", 1);           // parallel ...
        setParam (*inst, "FX A / B BALANCE", 0.0f);        // ... all A: only the filter is heard
        setParam (*inst, "FILTER RESONANCE", 0.0f);
        setChoiceParam (*inst, "PAD X CONTROLS", 7);       // FILTER CUTOFF
        auto atX = [&] (float x)
        {
            setParam (*inst, "PAD X", x);
            juce::MidiBuffer nOn; nOn.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
            render (*inst, 0.5, nOn);
            const double r = render (*inst, 1.5).rms;
            render (*inst, 0.3, allOff);
            return r;
        };
        const double closed = atX (0.0f), open = atX (100.0f);
        std::printf ("    pad X -> FILTER CUTOFF: X=0 rms %.4f, X=100 rms %.3f\n", closed, open);
        check (open > 10.0 * closed, "moving pad X plays the filter cutoff");
        setChoiceParam (*inst, "PAD X CONTROLS", 0);
        setParam (*inst, "FX MIX", 0.0f);
        render (*inst, 1.0);
    }

    std::printf ("offline bounce (non-realtime):\n");
    inst->setNonRealtime (true);
    std::vector<float> w2;
    auto s5 = render (*inst, 2.0, on, &w2);
    float early = 0; for (size_t i = 0; i < 4800; ++i) early = juce::jmax (early, std::abs (w2[i]));
    check (s5.rms > 0.02, "renders offline (rms " + juce::String (s5.rms, 3) + ")");
    check (early > 1e-3f, "no start-up gap offline (peak in first 100 ms " + juce::String (early, 3) + ")");
    render (*inst, 0.2, allOff);
    inst->setNonRealtime (false);

    std::printf ("state round trip:\n");
    juce::MemoryBlock saved;
    inst->getStateInformation (saved);
    const auto txt = saved.toString();
    check (saved.getSize() > 100, "state saved (" + juce::String ((int) saved.getSize()) + " bytes)");

    std::printf ("window scrub by an LFO:\n");
    {
        // two-tone sample: 3 s of 220 Hz, then 3 s of 880 Hz; the region covers only the 220 Hz part
        const int sr = 48000, len = 6 * sr;
        std::vector<float> tt ((size_t) len);
        for (int i = 0; i < len; ++i) tt[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * (i < len / 2 ? 220.0 : 880.0) * i / sr);
        const auto ttFile = outDir.getChildFile ("twotone.wav");
        juce::String e;
        ss::writeAudioFile (ttFile, 0, 24, { tt.data() }, (size_t) len, sr, 1.0f, e);

        auto loadWith = [&] (bool withRoute)
        {
            juce::XmlElement prm ("PARAMS");
            auto* ses = prm.createNewChildElement ("SESSION");
            ses->setAttribute ("sample", ttFile.getFullPathName());
            ses->setAttribute ("regionA", 0);
            ses->setAttribute ("regionB", len / 4);
            if (withRoute)
            {
                auto* m = ses->createNewChildElement ("MOD");
                auto* r = m->createNewChildElement ("ROUTE");
                r->setAttribute ("lfo", 0);
                r->setAttribute ("target", "window");
                r->setAttribute ("depth", 0.5);   // what dropping an LFO on the waveform gives
            }
            juce::MemoryBlock b;
            juce::AudioProcessor::copyXmlToBinary (prm, b);
            juce::XmlElement vs ("VST3PluginState");
            vs.createNewChildElement ("IComponent")->addTextElement (b.toBase64Encoding());
            juce::MemoryBlock o;
            juce::AudioProcessor::copyXmlToBinary (vs, o);
            inst->setStateInformation (o.getData(), (int) o.getSize());
            pump (1200);
            setParam (*inst, "STRETCH", 2.0f);
            setParam (*inst, "SCATTER", 0.0f);
            setParam (*inst, "LFO 1", 0.25f);
            setParam (*inst, "NORMALIZE", 0.0f);
            setParam (*inst, "LOOP", 1.0f);      // keep playing for the whole measurement
            pump (300);
        };
        auto segments = [&] (int& lo, int& hi)
        {
            std::vector<float> w;
            juce::MidiBuffer nOn; nOn.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
            render (*inst, 16.0, nOn, &w);     // four full sweeps, whatever phase the free-running LFO is at
            render (*inst, 0.3, allOff);
            lo = hi = 0;
            const size_t seg = 12000;
            for (size_t s0 = 4800; s0 + seg <= w.size(); s0 += seg)
            {
                int z = 0;
                for (size_t i = s0 + 1; i < s0 + seg; ++i) if (w[i - 1] < 0 && w[i] >= 0) ++z;
                const double f = z / (seg / 48000.0);
                if (f > 150 && f < 330) ++lo;
                if (f > 600 && f < 1100) ++hi;
            }
        };
        int lo, hi;
        loadWith (false);
        segments (lo, hi);
        std::printf ("    no LFO:   %d segments at 220 Hz, %d at 880 Hz\n", lo, hi);
        check (hi == 0 && lo > 10, "without modulation the region stays on the 220 Hz part");
        loadWith (true);
        segments (lo, hi);
        std::printf ("    LFO 1 on WINDOW: %d segments at 220 Hz, %d at 880 Hz\n", lo, hi);
        check (hi >= 3 && lo >= 3, "the LFO moves the window across the sample (both halves heard)");
    }

    std::printf ("editor:\n");
    if (auto* ed = inst->createEditorIfNeeded())
    {
        // play so the source/output views have something to show
        render (*inst, 1.5, on);
        juce::Component holder;
        holder.addAndMakeVisible (ed);
        holder.setSize (ed->getWidth(), ed->getHeight());
        holder.setVisible (true);
        for (int i = 0; i < 20; ++i) { render (*inst, 0.1); pump (30); }
        auto img = holder.createComponentSnapshot (holder.getLocalBounds(), true, 1.0f);
        juce::PNGImageFormat png;
        juce::FileOutputStream os (outDir.getChildFile ("editor.png"));
        os.setPosition (0); os.truncate();
        png.writeImageToStream (img, os);
        check (img.getWidth() > 900, "editor " + juce::String (ed->getWidth()) + "x" + juce::String (ed->getHeight()) + " → editor.png");
        holder.removeChildComponent (ed);
        delete ed;
    }
    else check (false, "editor created");
    render (*inst, 0.2, allOff);
    inst->releaseResources();
    inst = nullptr;

    // --- exporter (same code as RENDER & SAVE)
    std::printf ("export:\n");
    juce::AudioFormatManager afm;
    afm.registerBasicFormats();
    juce::String e2;
    auto data = ss::SampleData::load (sampleFile, afm, e2);
    check (data != nullptr, "sample decoded " + e2);
    if (data != nullptr)
    {
        for (auto [fmt, bits, name] : { std::tuple { 0, 24, "out24.wav" }, std::tuple { 1, 16, "out16.aif" }, std::tuple { 1, 32, "out32.aif" } })
        {
            ss::OfflineSettings os;
            os.data = data;
            os.regionA = 0; os.regionB = data->length / 2;
            os.stream.stretch = 6; os.stream.scatter = 1; os.stream.weave = 4;
            os.reverbPct = fmt == 0 ? 30.0f : 0.0f; os.sizeSec = 1.5f;
            ss::Exporter ex;
            const auto f = outDir.getChildFile (name);
            ex.start (os, f, fmt, bits);
            while (ex.isBusy()) juce::Thread::sleep (20);
            check (ex.state() == ss::Exporter::Done && f.getSize() > 1000, juce::String (name) + ": " + ex.message());
        }
    }

    std::printf ("live recorder (REC button):\n");
    for (int bits : { 24, 32 })
    {
        ss::Recorder rec;
        const auto f = outDir.getChildFile ("Slow Sippin Recordings").getChildFile ("take" + juce::String (bits) + ".wav");
        juce::String e;
        const double sr = 48000;
        check (rec.start (f, sr, bits, e), "start " + juce::String (bits) + "-bit " + e);
        // an "audio thread" pushing 3 s in 256-sample blocks, in real time-ish bursts
        const int total = (int) (3 * sr), block = 256;
        std::thread audio ([&]
        {
            std::vector<float> l ((size_t) block), r ((size_t) block);
            for (int p = 0; p < total; p += block)
            {
                for (int i = 0; i < block; ++i)
                {
                    l[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * (p + i) / sr);
                    r[(size_t) i] = -l[(size_t) i];
                }
                rec.process (l.data(), r.data(), std::min (block, total - p));
                if ((p / block) % 64 == 0) juce::Thread::sleep (1);
            }
        });
        audio.join();
        const double secs = rec.seconds();
        const auto out = rec.stop();
        check (out == f && f.existsAsFile(), "file written: " + f.getFileName() + " (" + juce::String (secs, 2) + " s, dropped " + juce::String (rec.droppedSamples()) + ")");
        juce::AudioFormatManager fm2;
        fm2.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm2.createReaderFor (f));
        check (rd != nullptr && rd->numChannels == 2 && (int) rd->lengthInSamples == total && rd->sampleRate == sr && (int) rd->bitsPerSample == bits,
               "reads back: " + (rd ? juce::String (rd->lengthInSamples) + " frames, " + juce::String (rd->bitsPerSample) + "-bit, " + juce::String (rd->sampleRate) + " Hz" : juce::String ("unreadable")));
        if (rd != nullptr)
        {
            juce::AudioBuffer<float> b (2, total);
            rd->read (&b, 0, total, 0, true, true);
            double err = 0;
            for (int i = 0; i < total; ++i)
            {
                const double want = 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * i / sr);
                err = juce::jmax (err, std::abs (b.getSample (0, i) - want), std::abs (b.getSample (1, i) + want));
            }
            check (err < (bits == 32 ? 1e-6 : 1e-5), "content matches exactly (max error " + juce::String (err, 8) + ")");
        }
    }

    std::printf ("export with effects + LFO:\n");
    if (data != nullptr)
    {
        ss::ModSystem mod;
        ss::OfflineSettings os;
        os.data = data;
        for (int t = 0; t < ss::kNumTargets; ++t) os.base[(size_t) t] = ss::findSpec (ss::targetParamId (t))->def;
        os.base[ss::tStretch] = 4;
        os.base[ss::tFxMix] = 100; os.base[ss::tFxX] = 100; os.base[ss::tFxY] = 60;   // filter (open) -> dub echo, pad Y = feedback 66 %
        os.base[ss::tEchoFb] = 70;
        os.stream.stretch = 4; os.stream.scatter = 1; os.stream.weave = 4;
        os.regionA = 0; os.regionB = data->length / 3;
        for (int i = 0; i < ss::kNumLfos; ++i) { os.lfoRate[i] = 0.5f; os.lfoShape[i] = ss::Sine; }
        mod.addRoute (0, ss::tFltCut, 0.5f);
        mod.addRoute (1, ss::tWindow, 0.6f);
        mod.addRoute (2, ss::tFxX, -0.3f);
        os.fx.typeA = ss::FxFilter; os.fx.typeB = ss::FxEcho; os.fx.series = true; os.fx.padX = ss::tFltCut; os.fx.padY = ss::tEchoFb;
        os.routes = mod.snapshotRoutes();
        os.mod = &mod;
        ss::Exporter ex;
        const auto f = outDir.getChildFile ("fx_lfo.wav");
        ex.start (os, f, 0, 24);
        while (ex.isBusy()) juce::Thread::sleep (20);
        const double expect = data->length / 3.0 / data->sampleRate * 4.0;
        const double got = (double) (f.getSize() - 44) / (2 * 3) / data->sampleRate;
        check (ex.state() == ss::Exporter::Done, ex.message());
        check (got > expect + 0.5, "file " + juce::String (got, 2) + " s > one-shot " + juce::String (expect, 2) + " s (echo tail kept)");
    }

    // --- optional: very long renders (hour-long, and > 4 GB as RF64)
    for (int a = 4; a < argc && data != nullptr; ++a)
    {
        const juce::String mode (argv[a]);
        if (mode != "--long" && mode != "--huge") continue;
        const bool huge = mode == "--huge";
        ss::OfflineSettings os;
        os.data = data;
        os.stream.stretch = huge ? 2200.0 : 600.0;      // 6 s source -> 1 h, or ~3.7 h
        os.stream.scatter = 1; os.stream.weave = 4;
        os.reverbPct = 30.0f; os.sizeSec = 3.0f;
        int64_t frames; int ch; double rate;
        ss::Exporter::estimate (os, frames, ch, rate);
        std::printf ("%s render: %.2f hours, %d ch\n", huge ? "huge" : "long", frames / rate / 3600.0, ch);
        ss::Exporter ex;
        const auto f = outDir.getChildFile (huge ? "huge32.wav" : "hour24.wav");
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        ex.start (os, f, 0, huge ? 32 : 24);
        int lastPct = -1;
        while (ex.isBusy())
        {
            juce::Thread::sleep (200);
            const int pct = (int) (ex.progress() * 10);
            if (pct != lastPct) { lastPct = pct; std::printf ("    %d%%\n", pct * 10); std::fflush (stdout); }
        }
        const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        check (ex.state() == ss::Exporter::Done, ex.message() + " in " + juce::String (secs, 0) + " s");
        juce::String status = juce::File ("/proc/self/status").loadFileAsString();
        for (auto line : juce::StringArray::fromLines (status))
            if (line.startsWith ("VmHWM")) std::printf ("    peak memory of this test process: %s\n", line.fromFirstOccurrenceOf (":", false, false).trim().toRawUTF8());
        check (! f.getSiblingFile ("." + f.getFileName() + ".slowsippin-tmp").exists(), "temporary file removed");
    }

    std::printf (failures ? "\n%d FAILURE(S)\n" : "\nall host tests passed\n", failures);
    return failures ? 1 : 0;
}
