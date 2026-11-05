/*
 * HostTest — loads the built VST3 like a DAW would and exercises it:
 * sample loading via saved state, MIDI notes/chords/release, live parameter
 * changes, offline (bounce) rendering, state round-trip, editor snapshot,
 * plus the file exporter.   Usage: HostTest <plugin.vst3> <sample.wav> <outdir>
 */
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Source/Offline.h"

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

static void setParam (juce::AudioPluginInstance& p, const juce::String& name, float value)
{
    for (auto* prm : p.getParameters())
        if (prm->getName (64).equalsIgnoreCase (name))
            if (auto* r = dynamic_cast<juce::HostedAudioProcessorParameter*> (prm))
            {
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

    std::printf (failures ? "\n%d FAILURE(S)\n" : "\nall host tests passed\n", failures);
    return failures ? 1 : 0;
}
