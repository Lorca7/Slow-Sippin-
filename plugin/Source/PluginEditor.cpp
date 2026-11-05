#include "PluginEditor.h"

using namespace ss;

namespace
{
juce::String fmtDur (double sec)
{
    if (! std::isfinite (sec)) return "-";
    if (sec < 10) return juce::String (sec, 2) + "s";
    if (sec < 60) return juce::String (sec, 1) + "s";
    const int h = (int) (sec / 3600), m = (int) std::fmod (sec / 60, 60), s = (int) std::fmod (sec, 60);
    if (h > 0) return juce::String (h) + "h " + juce::String (m).paddedLeft ('0', 2) + "m";
    return juce::String (m) + "m " + juce::String (s).paddedLeft ('0', 2) + "s";
}

juce::String fmtClock (double sec)
{
    sec = juce::jmax (0.0, sec);
    const int m = (int) (sec / 60);
    const double s = sec - m * 60;
    return juce::String (m) + ":" + (s < 10 ? "0" : "") + juce::String (s, 1);
}

juce::String fmtMB (double bytes) { return juce::String (bytes / 1048576.0, 1) + " MB"; }

double parseNumber (juce::String s)
{
    s = s.trim().removeCharacters (" ").replaceCharacter (',', '.').retainCharacters ("0123456789.+-eE");
    return s.isEmpty() ? std::numeric_limits<double>::quiet_NaN() : s.getDoubleValue();
}

void drawTicks (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (ss::Colours::faint);
    const int n = 48;
    for (int i = 1; i < n; ++i)
    {
        const float x = r.getX() + r.getWidth() * (float) i / n;
        const float len = i % 4 == 0 ? 8.0f : 4.0f;
        g.fillRect (x, r.getY(), 1.0f, len);
        g.fillRect (x, r.getBottom() - len, 1.0f, len);
    }
}

void drawTag (juce::Graphics& g, const juce::String& t, juce::Rectangle<float> r, bool right)
{
    g.setFont (Fonts::sans (11.0f, Fonts::Bold, 0.2f));
    const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), t) + 10.0f;
    juce::Rectangle<float> box (right ? r.getRight() - w - 8 : r.getX() + 8, r.getY() + 6, w, 17);
    g.setColour (ss::Colours::waveBg);
    g.fillRect (box);
    g.setColour (ss::Colours::muted);
    g.drawText (t, box, juce::Justification::centred);
}

double outputSeconds (const ss::OfflineSettings& s)
{
    if (s.data == nullptr) return 0;
    const auto v = ss::makeView (*s.data, s.regionA, s.regionB);
    return v.len / v.sampleRate * s.stream.stretch;
}
}

// ============================================================ SourceWave

int SourceWave::sampleAt (float x) const
{
    auto d = proc.getSample();
    if (d == nullptr) return 0;
    return (int) std::lround (juce::jlimit (0.0f, 1.0f, x / (float) getWidth()) * d->length);
}

void SourceWave::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Colours::waveBg);
    g.fillRect (r);
    drawTicks (g, r);

    auto d = proc.getSample();
    if (d == nullptr)
    {
        g.setColour (Colours::ink2);
        g.setFont (Fonts::sans (14.0f, Fonts::Bold, 0.24f));
        g.drawText (proc.isLoading() ? "LOADING..." : "DROP A SAMPLE", r.translated (0, -10), juce::Justification::centred);
        g.setColour (Colours::muted);
        g.setFont (Fonts::mono (12.0f));
        g.drawText ("WAV · AIFF · MP3 · FLAC", r.translated (0, 14), juce::Justification::centred);
    }
    else
    {
        const int W = getWidth();
        const float mid = r.getCentreY(), sc = r.getHeight() * 0.40f / juce::jmax (1e-6f, d->peak);
        const bool hasRegion = proc.hasRegion();
        const float xa = hasRegion ? (float) proc.regionA() / d->length * W : 0.0f;
        const float xb = hasRegion ? (float) proc.regionB() / d->length * W : (float) W;
        if (hasRegion)
        {
            g.setColour (Colours::red.withAlpha (0.10f));
            g.fillRect (xa, 0.0f, xb - xa, r.getHeight());
        }
        const int K = SampleData::kOverview;
        for (int x = 0; x < W; ++x)
        {
            const int k0 = x * K / W, k1 = juce::jmax (k0 + 1, (x + 1) * K / W);
            float lo = 0, hi = 0;
            for (int k = k0; k < k1 && k < K; ++k) { lo = juce::jmin (lo, d->ovMin[(size_t) k]); hi = juce::jmax (hi, d->ovMax[(size_t) k]); }
            const bool in = hasRegion && x >= (int) xa && x <= (int) xb;
            g.setColour (in ? Colours::red : Colours::grey);
            const float y0 = mid - hi * sc, y1 = mid - lo * sc;
            g.fillRect ((float) x, y0, 1.0f, juce::jmax (1.0f, y1 - y0));
        }
        if (hasRegion)
        {
            g.setColour (Colours::red);
            g.fillRect (xa - 0.75f, 0.0f, 1.5f, r.getHeight());
            if (proc.regionB() < d->length) g.fillRect (xb - 0.75f, 0.0f, 1.5f, r.getHeight());
        }

        // where the voices are reading
        float pos[VoiceEngine::kMaxVoices];
        const int nv = proc.voicePositions (pos, VoiceEngine::kMaxVoices);
        const float rs = hasRegion ? (float) proc.regionA() / d->length : 0.0f;
        const float re = hasRegion ? (float) proc.regionB() / d->length : 1.0f;
        g.setColour (Colours::ink.withAlpha (0.75f));
        for (int i = 0; i < nv; ++i)
            g.fillRect ((rs + pos[i] * (re - rs)) * W - 0.75f, 0.0f, 1.5f, r.getHeight());
        if (proc.isAuditioning())
        {
            g.setColour (Colours::ink);
            g.fillRect (proc.auditionFrac() * W - 1.0f, 0.0f, 2.0f, r.getHeight());
        }

        const double sr = d->sampleRate;
        juce::String tag;
        if (! hasRegion) tag = "FULL " + juce::String (d->length / sr, 2) + "S";
        else if (proc.regionB() >= d->length) tag = "FROM " + juce::String (proc.regionA() / sr, 2) + "S";
        else tag = "REGION " + juce::String ((proc.regionB() - proc.regionA()) / sr, 2) + "S";
        drawTag (g, tag, r, true);
    }
    drawTag (g, "SOURCE", r, false);
    g.setColour (Colours::line);
    g.drawRect (r, 2.0f);
}

void SourceWave::mouseDown (const juce::MouseEvent& e)
{
    if (proc.getSample() == nullptr) { if (onEmptyClick) onEmptyClick(); return; }
    dragFrom = sampleAt (e.position.x);
    dragged = false;
}

void SourceWave::mouseDrag (const juce::MouseEvent& e)
{
    if (dragFrom < 0 || e.getDistanceFromDragStart() < 4) return;
    dragged = true;
    const int s = sampleAt (e.position.x);
    proc.setRegion (juce::jmin (dragFrom, s), juce::jmax (dragFrom, s));
    repaint();
}

void SourceWave::mouseUp (const juce::MouseEvent& e)
{
    auto d = proc.getSample();
    if (d == nullptr || dragFrom < 0) return;
    if (! dragged && e.getNumberOfClicks() < 2)
        proc.setRegion (sampleAt (e.position.x), d->length);      // click = start point
    dragFrom = -1;
    repaint();
}

void SourceWave::mouseDoubleClick (const juce::MouseEvent&)
{
    proc.setRegion (0, 0);
    repaint();
}

// ============================================================ OutputView

void OutputView::update()
{
    const auto s = proc.offlineSettings();
    juce::String k;
    if (s.data != nullptr)
        k << s.data->name << s.regionA << '|' << s.regionB << '|' << s.stream.stretch << '|' << s.stream.pitchSemis << '|' << s.stream.weave
          << '|' << s.stream.scatter << '|' << s.stream.driftMs << '|' << (int) s.stream.reverse << '|' << (int) s.stream.monoToStereo
          << '|' << s.reverbPct << '|' << s.sizeSec;
    if (k != key) { key = k; std::fill (cols.begin(), cols.end(), 0.0f); }

    const int n = proc.readScope (scope, 2048);
    const float f = proc.transportFrac();
    if (f >= 0 && ! cols.empty())
    {
        float m = 0;
        for (int i = 0; i < n; ++i) m = juce::jmax (m, scope[i]);
        auto& c = cols[(size_t) juce::jlimit (0, (int) cols.size() - 1, (int) (f * (float) cols.size()))];
        c = juce::jmax (c, m);
    }
    repaint();
}

void OutputView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Colours::waveBg);
    g.fillRect (r);
    drawTicks (g, r);

    const auto s = proc.offlineSettings();
    const bool looping = proc.apvts.getRawParameterValue ("loop")->load() > 0.5f;
    const double total = outputSeconds (s);

    float mx = 1e-6f;
    for (float v : cols) mx = juce::jmax (mx, v);
    const float mid = r.getCentreY(), sc = r.getHeight() * 0.42f / mx;
    bool any = false;
    g.setColour (Colours::ink);
    for (size_t x = 0; x < cols.size(); ++x)
        if (cols[x] > 0)
        {
            any = true;
            g.fillRect ((float) x, mid - cols[x] * sc, 1.0f, juce::jmax (1.0f, 2 * cols[x] * sc));
        }

    if (! any && s.data != nullptr)
    {
        g.setColour (Colours::muted);
        g.setFont (Fonts::mono (12.0f));
        g.drawText ("press PLAY (or a MIDI key) - the output is drawn as you hear it", r, juce::Justification::centred);
    }

    if (s.data != nullptr)
    {
        g.setColour (Colours::red);
        const float xs = proc.getStartFrac() * r.getWidth();
        g.setOpacity (proc.isPlaying() ? 0.45f : 1.0f);
        g.fillRect (xs - 0.75f, 0.0f, 1.5f, r.getHeight());
        g.setOpacity (1.0f);
        const float f = proc.transportFrac();
        if (f >= 0) g.fillRect (f * r.getWidth() - 1.0f, 0.0f, 2.0f, r.getHeight());

        const double now = (f >= 0 ? f : proc.getStartFrac()) * total;
        drawTag (g, fmtClock (now) + " / " + (looping ? juce::String ("LOOP") : fmtClock (total)), r, true);
    }
    drawTag (g, "OUTPUT", r, false);

    // export progress overlay
    if (proc.exporter.isBusy())
    {
        g.setColour (Colours::bg.withAlpha (0.88f));
        g.fillRect (r);
        const float p = proc.exporter.progress();
        auto bar = juce::Rectangle<float> (juce::jmin (420.0f, r.getWidth() * 0.7f), 8.0f).withCentre (r.getCentre().translated (0, -8));
        g.setColour (Colours::red);
        g.fillRect (bar.withWidth (bar.getWidth() * p));
        g.setColour (Colours::line);
        g.drawRect (bar, 1.5f);
        g.setColour (Colours::ink2);
        g.setFont (Fonts::mono (12.0f, Fonts::Medium, 0.1f));
        g.drawText ("RENDERING FILE " + juce::String (juce::roundToInt (p * 100)) + "%", bar.translated (0, 22).withHeight (16).expanded (100, 0), juce::Justification::centred);
    }

    g.setColour (Colours::line);
    g.drawRect (r, 2.0f);
}

void OutputView::mouseDown (const juce::MouseEvent& e)
{
    if (proc.getSample() == nullptr || proc.exporter.isBusy()) return;
    proc.setStartFrac (e.position.x / (float) getWidth());
    repaint();
}

// ============================================================ ControlRow

ControlRow::ControlRow (const ControlSpec& s, juce::RangedAudioParameter& p) : spec (s), param (p)
{
    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setRange (0.0, 1.0, 0.0);
    slider.setDoubleClickReturnValue (true, specToPos (spec, spec.def));
    slider.setVelocityModeParameters (1.0, 1, 0.0, false);
    slider.onDragStart = [this] { dragging = true; param.beginChangeGesture(); };
    slider.onDragEnd = [this] { dragging = false; param.endChangeGesture(); };
    slider.onValueChange = [this]
    {
        if (updating) return;
        const double v = specFromPos (spec, slider.getValue());
        if (! dragging) param.beginChangeGesture();
        apply (v);
        if (! dragging) param.endChangeGesture();
    };
    slider.setTooltip (juce::String (spec.label).toLowerCase() + ": drag, or double-click to reset");
    addAndMakeVisible (slider);

    box.setJustification (juce::Justification::centred);
    box.setFont (Fonts::mono (14.0f));
    box.setSelectAllWhenFocused (true);
    box.setInputRestrictions (16, "0123456789.,-+eE");
    auto commit = [this]
    {
        const double v = parseNumber (box.getText());
        if (std::isfinite (v)) { param.beginChangeGesture(); apply (v); param.endChangeGesture(); }
        shown = std::numeric_limits<double>::quiet_NaN();
        refresh();
    };
    box.onReturnKey = [this, commit] { commit(); box.selectAll(); };
    box.onFocusLost = commit;
    box.onEscapeKey = [this] { shown = std::numeric_limits<double>::quiet_NaN(); refresh(); box.giveAwayKeyboardFocus(); };
    box.onNudge = [this] (int dir, bool big)
    {
        const double cur = current();
        double step = spec.integer ? 1.0 : std::pow (10.0, -spec.decimals) * (big ? 10.0 : 1.0);
        if (spec.scale == ControlSpec::Log) step = juce::jmax (step, cur * (big ? 0.1 : 0.01));
        param.beginChangeGesture();
        apply (cur + dir * step);
        param.endChangeGesture();
        shown = std::numeric_limits<double>::quiet_NaN();
        refresh();
        box.selectAll();
    };
    addAndMakeVisible (box);
    refresh();
}

int ControlRow::decimalsFor (double v) const
{
    if (juce::String (spec.id) == "stretch") return v >= 10000 ? 0 : v >= 1000 ? 1 : 2;
    return spec.decimals;
}

void ControlRow::apply (double v)
{
    v = specQuantize (spec, juce::jlimit ((double) spec.pMin, (double) spec.pMax, v));
    param.setValueNotifyingHost (param.convertTo0to1 ((float) v));
}

void ControlRow::refresh()
{
    const double v = current();
    if (v == shown) return;
    shown = v;
    const juce::ScopedValueSetter<bool> svs (updating, true);
    if (! dragging) slider.setValue (specToPos (spec, v), juce::dontSendNotification);
    if (! box.hasKeyboardFocus (true)) box.setText (juce::String (v, decimalsFor (v)), false);
    repaint();
}

void ControlRow::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    g.setColour (Colours::faint);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);

    const int labelW = juce::jmin (122, getWidth() / 6);
    g.setColour (Colours::ink);
    auto lf = Fonts::sans (12.5f, Fonts::Bold, 0.15f);
    g.setFont (lf);
    g.drawText (spec.label, r.withWidth (labelW), juce::Justification::centredLeft);
    if (spec.engine)
    {
        const float tw = juce::GlyphArrangement::getStringWidth (lf, spec.label);
        g.setColour (Colours::red);
        g.fillEllipse (tw + 7.0f, getHeight() * 0.5f - 2.5f, 5.0f, 5.0f);
    }

    const juce::String num = juce::String (shown, decimalsFor (shown));
    auto big = Fonts::mono (juce::jlimit (18.0f, 27.0f, getHeight() * 0.56f), Fonts::Light, 0.03f);
    g.setFont (big);
    g.setColour (Colours::ink);
    const int vx = labelW + 8;
    const float nw = juce::GlyphArrangement::getStringWidth (big, num);
    g.drawText (num, vx, 0, (int) nw + 4, getHeight(), juce::Justification::centredLeft);
    g.setColour (Colours::muted);
    g.setFont (Fonts::mono (11.0f));
    g.drawText (spec.unit, vx + (int) nw + 4, 4, 30, getHeight(), juce::Justification::centredLeft);
}

void ControlRow::resized()
{
    auto r = getLocalBounds();
    const int labelW = juce::jmin (122, getWidth() / 6);
    const int boxW = juce::jlimit (70, 112, getWidth() / 6);
    box.setBounds (r.removeFromRight (boxW).reduced (0, juce::jmax (6, (getHeight() - 36) / 2)));
    r.removeFromRight (18);
    r.removeFromLeft (labelW + 8);
    const int valueW = juce::jmax (100, r.getWidth() * 42 / 100);
    r.removeFromLeft (valueW);
    slider.setBounds (r.reduced (0, juce::jmax (4, (getHeight() - 30) / 2)));
}

// ============================================================== Editor

SlowSippinEditor::SlowSippinEditor (SlowSippinProcessor& p)
    : AudioProcessorEditor (&p), proc (p), source (p), output (p)
{
    setLookAndFeel (&look);
    juce::LookAndFeel::setDefaultLookAndFeel (&look);

    exportBtn.getProperties().set ("primary", true);
    for (auto* b : { &loadBtn, &auditionBtn, &playBtn, &stopBtn, &exportBtn }) addAndMakeVisible (b);
    loadBtn.onClick = [this] { openSample(); };
    auditionBtn.onClick = [this] { proc.toggleAudition(); };
    playBtn.onClick = [this] { proc.togglePlay(); };
    stopBtn.onClick = [this] { proc.stop(); if (proc.isAuditioning()) proc.toggleAudition(); };
    exportBtn.onClick = [this] { if (proc.exporter.isBusy()) proc.exporter.cancel(); else startExport(); };
    playBtn.setTooltip ("Play / stop (space). MIDI notes play it too.");
    auditionBtn.setTooltip ("Hear the original sample or region");
    exportBtn.setTooltip ("Render the whole stretch (region x stretch, plus reverb tail) to a file");

    fmtBox.addItem ("WAV", 1);
    fmtBox.addItem ("AIFF", 2);
    bitsBox.addItem ("16-BIT", 16);
    bitsBox.addItem ("24-BIT", 24);
    bitsBox.addItem ("32-BIT FLOAT", 32);
    fmtBox.setSelectedId (proc.exportFormat == 1 ? 2 : 1, juce::dontSendNotification);
    bitsBox.setSelectedId (proc.exportBits, juce::dontSendNotification);
    fmtBox.onChange = [this] { proc.exportFormat = fmtBox.getSelectedId() == 2 ? 1 : 0; };
    bitsBox.onChange = [this] { proc.exportBits = bitsBox.getSelectedId(); };
    addAndMakeVisible (fmtBox);
    addAndMakeVisible (bitsBox);

    source.onEmptyClick = [this] { openSample(); };
    addAndMakeVisible (source);
    addAndMakeVisible (output);

    for (const auto& c : controlSpecs())
        if (auto* prm = proc.apvts.getParameter (c.id))
            addAndMakeVisible (rows.add (new ControlRow (c, *prm)));

    for (const auto& t : toggleSpecs())
    {
        auto* b = toggles.add (new juce::ToggleButton (t.label));
        addAndMakeVisible (b);
        if (auto* prm = proc.apvts.getParameter (t.id))
            toggleAtts.add (new juce::ButtonParameterAttachment (*prm, *b));
    }
    toggles[1]->setTooltip ("A mono sample becomes wide stereo (two independent phase passes)");
    toggles[3]->setTooltip ("Loop the region forever instead of a one-shot of region x stretch");
    toggles[4]->setTooltip ("MIDI notes transpose relative to C3 (note 60)");

    proc.addChangeListener (this);
    setWantsKeyboardFocus (true);
    setResizable (true, true);
    setResizeLimits (980, 720, 2600, 1700);
    setSize (1320, 860);
    startTimerHz (30);
}

SlowSippinEditor::~SlowSippinEditor()
{
    proc.removeChangeListener (this);
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void SlowSippinEditor::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (6);
    header = r.removeFromTop (88);
    footer = r.removeFromBottom (86);
    const int lw = (int) (r.getWidth() * 0.52f);
    left = r.removeFromLeft (lw);
    right = r;

    {
        auto h = header.reduced (40, 0);
        auto b = h.removeFromRight (juce::jmax (420, h.getWidth() / 2)).withSizeKeepingCentre (juce::jmax (420, h.getWidth() / 2), 42);
        loadBtn.setBounds (b.removeFromLeft (140));
    }

    {
        auto l = left.reduced (40, 30);
        const int avail = l.getHeight() - 2 * 64;
        source.setBounds (l.removeFromTop ((int) (avail * 0.55f)));
        auto row1 = l.removeFromTop (64).withTrimmedTop (14);
        auditionBtn.setBounds (row1.removeFromLeft (130).withHeight (42));
        srcHint = row1.withTrimmedLeft (18).withHeight (42);
        l.removeFromTop (4);
        auto row2 = l.removeFromBottom (64).withTrimmedTop (16);
        output.setBounds (l);
        playBtn.setBounds (row2.removeFromLeft (110).withHeight (46));
        row2.removeFromLeft (14);
        stopBtn.setBounds (row2.removeFromLeft (100).withHeight (46));
        outHint = row2.withTrimmedLeft (18).withHeight (46);
    }

    {
        auto rr = right.reduced (40, 26);
        legendArea = rr.removeFromBottom (40);
        auto tg = rr.removeFromBottom (78);
        const int rowH = juce::jmin (56, rr.getHeight() / (int) rows.size());
        rr.removeFromTop (juce::jmax (0, rr.getHeight() - rowH * rows.size()) / 2);
        for (auto* row : rows) row->setBounds (rr.removeFromTop (rowH));
        tg.removeFromTop (14);
        auto line1 = tg.removeFromTop (30), line2 = tg.removeFromTop (30);
        const int tw = line1.getWidth() / 3;
        for (int i = 0; i < toggles.size(); ++i)
        {
            auto& ln = i < 3 ? line1 : line2;
            toggles[i]->setBounds (ln.removeFromLeft (tw).withWidth (juce::jmin (tw, 200)));
        }
    }

    {
        auto f = footer.reduced (40, 18);
        auto ex = f.removeFromRight (juce::jmin (640, f.getWidth() * 55 / 100));
        statsArea = f;
        auto top = ex.removeFromTop (42);
        exportBtn.setBounds (top.removeFromRight (170));
        top.removeFromRight (12);
        bitsBox.setBounds (top.removeFromRight (170));
        top.removeFromRight (12);
        fmtBox.setBounds (top.removeFromRight (104));
    }
}

void SlowSippinEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::bg);
    g.setColour (Colours::red);
    g.fillRect (0, 0, getWidth(), 6);
    g.setColour (Colours::line);
    g.fillRect (0, header.getBottom() - 1, getWidth(), 1);
    g.fillRect (0, footer.getY(), getWidth(), 1);
    g.fillRect (left.getRight(), left.getY(), 1, left.getHeight());

    // header
    auto h = header.reduced (40, 0);
    g.setColour (Colours::ink);
    auto title = Fonts::sans (42.0f, Fonts::ExtraBold, -0.01f);
    g.setFont (title);
    const juce::String name = "SLOW SIPPIN'";
    const float tw = juce::GlyphArrangement::getStringWidth (title, name);
    g.drawText (name, h.getX(), h.getY(), (int) tw + 6, h.getHeight(), juce::Justification::centredLeft);
    g.setColour (Colours::muted);
    g.setFont (Fonts::mono (11.5f, Fonts::Regular, 0.12f));
    g.drawText ("TIME-STRETCH", h.getX() + (int) tw + 30, h.getCentreY() - 16, 240, 16, juce::Justification::centredLeft);
    g.setFont (Fonts::mono (11.5f));
    g.drawText ("textures · pads · drones", h.getX() + (int) tw + 30, h.getCentreY(), 240, 16, juce::Justification::centredLeft);

    const int infoX = loadBtn.getRight() + 22;
    auto statusR = juce::Rectangle<int> (h.getRight() - 260, h.getY(), 260, h.getHeight());
    auto d = proc.getSample();
    g.setFont (Fonts::mono (13.0f));
    juce::String info;
    if (d != nullptr)
        info << d->name << "  ·  " << juce::String ((int) d->sampleRate) << " Hz · " << d->numChannels << "ch · " << juce::String (d->seconds(), 2) << "s";
    else
        info = proc.isLoading() ? "loading..." : "no sample - drop a file anywhere, or click + SAMPLE";
    g.setColour (d != nullptr ? Colours::ink2 : Colours::muted);
    g.drawFittedText (info, infoX, h.getY(), statusR.getX() - infoX - 16, h.getHeight(), juce::Justification::centredLeft, 1, 0.9f);

    const bool busy = proc.exporter.isBusy() || proc.isLoading();
    g.setColour (statusError ? Colours::ink : (busy || proc.isPlaying() || d != nullptr) ? Colours::red : Colours::faint);
    const float pulse = busy ? 0.4f + 0.6f * (float) std::abs (std::sin (juce::Time::getMillisecondCounter() / 300.0)) : 1.0f;
    g.setOpacity (pulse);
    g.fillEllipse ((float) statusR.getRight() - 10.0f - juce::GlyphArrangement::getStringWidth (Fonts::mono (12.0f), statusShown) - 10.0f,
                   (float) statusR.getCentreY() - 5.0f, 10.0f, 10.0f);
    g.setOpacity (1.0f);
    g.setColour (statusError ? Colours::red : Colours::muted);
    g.setFont (Fonts::mono (12.0f));
    g.drawText (statusShown, statusR, juce::Justification::centredRight);

    // hints
    g.setFont (Fonts::mono (12.0f));
    g.setColour (Colours::muted);
    g.drawText ("drag = select · click = point · dbl-click = clear", srcHint, juce::Justification::centredLeft);
    g.drawFittedText ("click the output to set the start point · space = play/stop", outHint, juce::Justification::centredLeft, 2);

    // legend
    g.setColour (Colours::red);
    g.fillEllipse ((float) legendArea.getX(), (float) legendArea.getY() + 7, 5.0f, 5.0f);
    g.setColour (Colours::muted);
    g.setFont (Fonts::mono (11.5f));
    g.drawFittedText ("engine controls - every control is live; MIDI notes play from C3 (key track),\nattack / release shape notes, RENDER & SAVE writes the full one-shot to a file",
                      legendArea.withTrimmedLeft (12), juce::Justification::topLeft, 2);

    // footer stats
    const auto s = proc.offlineSettings();
    auto st = statsArea;
    auto stat = [&] (const juce::String& k, const juce::String& v, int w, bool strong)
    {
        auto c = st.removeFromLeft (w);
        g.setColour (Colours::muted);
        g.setFont (Fonts::sans (10.5f, Fonts::SemiBold, 0.18f));
        g.drawText (k, c.removeFromTop (18), juce::Justification::topLeft);
        g.setColour (strong ? Colours::ink : Colours::ink2);
        g.setFont (Fonts::mono (17.0f, strong ? Fonts::Medium : Fonts::Regular));
        g.drawText (v, c.withHeight (30), juce::Justification::centredLeft);
    };
    const int cw = juce::jmax (150, st.getWidth() / 3);
    if (d != nullptr)
    {
        const auto v = ss::makeView (*d, s.regionA, s.regionB);
        stat ("SOURCE", fmtDur (v.len / v.sampleRate) + " · " + juce::String (v.len) + " smp", cw + 30, false);
        const bool looping = proc.apvts.getRawParameterValue ("loop")->load() > 0.5f;
        stat ("> OUTPUT", fmtDur (outputSeconds (s)) + " · 2ch" + (looping ? juce::String (" · loop") : juce::String()), cw, true);
        stat ("MEMORY", fmtMB ((double) d->length * d->numChannels * 8), cw - 30, false);
    }
    else
    {
        stat ("SOURCE", "-", cw + 30, false);
        stat ("> OUTPUT", "-", cw, true);
        stat ("MEMORY", "0.0 MB", cw - 30, false);
    }

    // export label + estimate
    g.setColour (Colours::muted);
    g.setFont (Fonts::sans (10.5f, Fonts::SemiBold, 0.18f));
    g.drawText ("EXPORT", fmtBox.getX() - 80, fmtBox.getY(), 70, fmtBox.getHeight(), juce::Justification::centredRight);
    g.setFont (Fonts::mono (12.0f));
    int64_t frames; int ch; double rate;
    Exporter::estimate (s, frames, ch, rate);
    juce::String est = "-";
    if (frames > 0)
    {
        est = fmtDur ((double) frames / rate) + " · " + juce::String (ch) + "ch · " + fmtMB ((double) frames * ch * bitsBox.getSelectedId() / 8.0);
        if (frames * 2 > Exporter::kMaxSamples) est = "too long to render to a file - select a shorter region";
    }
    g.setColour (frames * 2 > Exporter::kMaxSamples ? Colours::red : Colours::muted);
    g.drawText (est, exportBtn.getRight() - 600, exportBtn.getBottom() + 4, 600, 18, juce::Justification::centredRight);

    if (dragOver)
    {
        g.setColour (Colours::red);
        g.drawRect (getLocalBounds().reduced (3), 3);
    }
}

void SlowSippinEditor::setStatus (const juce::String& s, bool error, int holdMs)
{
    status = s;
    statusError = error;
    statusUntil = juce::Time::getMillisecondCounter() + (juce::uint32) holdMs;
}

void SlowSippinEditor::timerCallback()
{
    for (auto* r : rows) r->refresh();
    output.update();
    source.repaint();

    auto d = proc.getSample();
    const bool busy = proc.exporter.isBusy();
    exportBtn.setButtonText (busy ? "CANCEL" : "RENDER & SAVE");
    exportBtn.setEnabled (d != nullptr || busy);
    playBtn.setEnabled (d != nullptr);
    stopBtn.setEnabled (d != nullptr);
    auditionBtn.setEnabled (d != nullptr);
    playBtn.setToggleState (proc.isPlaying(), juce::dontSendNotification);
    auditionBtn.setToggleState (proc.isAuditioning(), juce::dontSendNotification);
    auditionBtn.setButtonText (proc.isAuditioning() ? "STOP" : "AUDITION");
    toggles[1]->setEnabled (d == nullptr || d->numChannels == 1);

    const int es = proc.exporter.state();
    if (es != lastExportState)
    {
        lastExportState = es;
        if (es == Exporter::Done) setStatus (proc.exporter.message(), proc.exporter.message().contains ("clipped"), 8000);
        else if (es == Exporter::Failed) setStatus ("export failed: " + proc.exporter.message(), true, 8000);
        else if (es == Exporter::Cancelled) setStatus ("export cancelled", false, 3000);
    }

    juce::String now;
    bool err = false;
    if (juce::Time::getMillisecondCounter() < statusUntil) { now = status; err = statusError; }
    else if (busy) now = "rendering file " + juce::String (juce::roundToInt (proc.exporter.progress() * 100)) + "%";
    else if (proc.isLoading()) now = "loading...";
    else if (proc.getLoadError().isNotEmpty()) { now = proc.getLoadError(); err = true; }
    else if (proc.isPlaying()) now = "playing";
    else if (d != nullptr) now = "ready";
    else now = "idle";
    if (now != statusShown || err != statusError || busy)
    {
        statusShown = now;
        statusError = err;
        repaint (header);
    }
    repaint (footer);
}

bool SlowSippinEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::spaceKey) { proc.togglePlay(); return true; }
    return false;
}

static bool isAudioFile (const juce::String& f)
{
    return juce::File (f).hasFileExtension ("wav;wave;aif;aiff;aifc;mp3;flac;ogg;m4a;caf");
}

bool SlowSippinEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files) if (isAudioFile (f)) return true;
    return false;
}

void SlowSippinEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    for (auto& f : files)
        if (isAudioFile (f)) { proc.loadSample (juce::File (f)); break; }
    repaint();
}

void SlowSippinEditor::openSample()
{
    chooser = std::make_unique<juce::FileChooser> ("Load sample", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   "*.wav;*.wave;*.aif;*.aiff;*.aifc;*.mp3;*.flac;*.ogg;*.m4a;*.caf");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) proc.loadSample (f);
                          });
}

juce::String SlowSippinEditor::exportFileName (const juce::String& ext) const
{
    auto d = proc.getSample();
    auto base = d != nullptr ? d->file.getFileNameWithoutExtension() : juce::String ("sample");
    const auto s = proc.offlineSettings();
    juce::String n = base + "_x" + juce::String (s.stream.stretch, s.stream.stretch >= 100 ? 0 : 2).replaceCharacter ('.', ',');
    if (std::abs (s.stream.pitchSemis) > 0.001) n << "_" << (s.stream.pitchSemis > 0 ? "+" : "") << juce::String (s.stream.pitchSemis, 1) << "st";
    if (s.stream.reverse) n << "_rev";
    return n + "." + ext;
}

void SlowSippinEditor::startExport()
{
    const auto s = proc.offlineSettings();
    if (s.data == nullptr) { openSample(); return; }
    int64_t frames; int ch; double rate;
    Exporter::estimate (s, frames, ch, rate);
    if (frames * 2 > Exporter::kMaxSamples) { setStatus ("too long to render - select a shorter region or lower STRETCH", true, 6000); return; }

    const bool aiff = fmtBox.getSelectedId() == 2;
    const auto ext = aiff ? juce::String ("aif") : juce::String ("wav");
    chooser = std::make_unique<juce::FileChooser> ("Save rendered audio",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (exportFileName (ext)),
                                                   aiff ? "*.aif;*.aiff" : "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, aiff, ext] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f == juce::File()) return;
                              if (! f.hasFileExtension (aiff ? "aif;aiff" : "wav")) f = f.withFileExtension (ext);
                              proc.exporter.start (proc.offlineSettings(), f, aiff ? 1 : 0, bitsBox.getSelectedId());
                          });
}
