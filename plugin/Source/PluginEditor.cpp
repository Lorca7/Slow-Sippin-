#include "PluginEditor.h"

using namespace ss;

static juce::String U8 (const char* s) { return juce::String::fromUTF8 (s); }

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
        g.drawText (U8 ("WAV · AIFF · MP3 · FLAC"), r.translated (0, 14), juce::Justification::centred);
    }
    else
    {
        const int W = getWidth();
        const float mid = r.getCentreY(), sc = r.getHeight() * 0.40f / juce::jmax (1e-6f, d->peak);
        const bool hasRegion = proc.hasRegion();
        // where the window is right now (WINDOW control + any LFO on it)
        const float wnd = proc.modulatedValue (tWindow);
        const auto mv = regionView (*d, proc.regionA(), proc.regionB(), wnd);
        const int ma = (int) (mv.fwd[0] - d->fwd.getReadPointer (0));
        const bool moved = hasRegion && ma != proc.regionA();
        const float xa = hasRegion ? (float) ma / d->length * W : 0.0f;
        const float xb = hasRegion ? (float) (ma + mv.len) / d->length * W : (float) W;
        if (hasRegion)
        {
            g.setColour (Colours::red.withAlpha (0.10f));
            g.fillRect (xa, 0.0f, xb - xa, r.getHeight());
        }
        if (moved)   // the drawn selection, where the window returns to
        {
            const float ha = (float) proc.regionA() / d->length * W, hb = (float) proc.regionB() / d->length * W;
            g.setColour (Colours::red.withAlpha (0.45f));
            const float dash[] = { 4.0f, 4.0f };
            g.drawDashedLine ({ ha, 2.0f, ha, r.getHeight() - 2.0f }, dash, 2, 1.0f);
            g.drawDashedLine ({ hb, 2.0f, hb, r.getHeight() - 2.0f }, dash, 2, 1.0f);
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
            if (ma + mv.len < d->length) g.fillRect (xb - 0.75f, 0.0f, 1.5f, r.getHeight());
        }
        else if (proc.getMod().hasRoutesFor (tWindow) || std::abs (wnd) > 0.01f)
        {
            g.setColour (Colours::red);
            g.setFont (Fonts::mono (11.5f));
            g.drawText ("select a region first - the WINDOW moves that selection across the sample",
                        r.withTrimmedTop (r.getHeight() - 30), juce::Justification::centred);
        }

        // where the voices are reading
        float pos[VoiceEngine::kMaxVoices];
        const int nv = proc.voicePositions (pos, VoiceEngine::kMaxVoices);
        const float rs = xa / W, re = xb / W;
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
    g.setColour (dropHover ? Colours::red : Colours::line);
    g.drawRect (r, dropHover ? 3.0f : 2.0f);
    if (dropHover)
    {
        g.setFont (Fonts::sans (12.0f, Fonts::Bold, 0.16f));
        g.drawText ("DROP TO MOVE THE WINDOW WITH THIS LFO", r.withTrimmedBottom (r.getHeight() * 0.5f), juce::Justification::centredBottom);
    }
}

void SourceWave::itemDropped (const SourceDetails& det)
{
    dropHover = false;
    const int lfo = lfoFromDescription (det.description);
    if (lfo >= 0) proc.getMod().addRoute (lfo, tWindow, 0.5f);   // full sweep across the sample
    chips.sync();
    repaint();
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

ControlRow::ControlRow (SlowSippinProcessor& pr, const ControlSpec& s, juce::RangedAudioParameter& p, bool isCompact)
    : proc (pr), spec (s), param (p), target (s.modulatable ? targetForParam (s.id) : -1), compact (isCompact)
{
    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setRange (0.0, 1.0, 0.0);
    slider.setDoubleClickReturnValue (true, specToPos (spec, spec.def));
    slider.setScrollWheelEnabled (false);   // trackpad/wheel scrolling must never change a control
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
    slider.setTooltip (juce::String::fromUTF8 (spec.label).toLowerCase() + ": drag, double-click to reset"
                       + (target >= 0 ? juce::String (" - drop an LFO here to modulate it") : juce::String()));
    addAndMakeVisible (slider);

    box.setJustification (juce::Justification::centred);
    box.setFont (Fonts::mono (compact ? 12.5f : 14.0f));
    box.setSelectAllWhenFocused (true);
    box.setInputRestrictions (16, "0123456789.,-+eE");
    // only what was actually typed is applied - focus changes never write a stale value back
    box.onTextChange = [this] { edited = true; };
    auto commit = [this]
    {
        if (edited)
        {
            const double v = parseNumber (box.getText());
            if (std::isfinite (v)) { param.beginChangeGesture(); apply (v); param.endChangeGesture(); }
        }
        edited = false;
        shown = std::numeric_limits<double>::quiet_NaN();
        refresh();
    };
    box.onReturnKey = [this, commit] { commit(); box.selectAll(); };
    box.onFocusLost = commit;
    box.onEscapeKey = [this] { edited = false; shown = std::numeric_limits<double>::quiet_NaN(); refresh(); box.giveAwayKeyboardFocus(); };
    box.onNudge = [this] (int dir, bool big)
    {
        const double cur = current();
        double step = spec.integer ? 1.0 : std::pow (10.0, -spec.decimals) * (big ? 10.0 : 1.0);
        if (spec.scale == ControlSpec::Log) step = juce::jmax (step, cur * (big ? 0.1 : 0.01));
        param.beginChangeGesture();
        apply (cur + dir * step);
        param.endChangeGesture();
        edited = false;
        shown = std::numeric_limits<double>::quiet_NaN();
        refresh();
        box.selectAll();
    };
    addAndMakeVisible (box);

    if (target >= 0)
    {
        chips = std::make_unique<ChipStrip> (proc, target);
        addAndMakeVisible (*chips);
    }
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
    if (chips != nullptr && chips->sync()) resized();
    const bool modulated = target >= 0 && proc.getMod().hasRoutesFor (target);
    if (modulated) repaint();
    const double v = current();
    if (v == shown) return;
    shown = v;
    const juce::ScopedValueSetter<bool> svs (updating, true);
    if (! dragging) slider.setValue (specToPos (spec, v), juce::dontSendNotification);
    if (! edited) box.setText (juce::String (v, decimalsFor (v)), false);
    repaint();
}

void ControlRow::itemDropped (const SourceDetails& d)
{
    dropHover = false;
    const int lfo = lfoFromDescription (d.description);
    if (lfo >= 0 && target >= 0) proc.getMod().addRoute (lfo, target, 0.5f);
    refresh();
    repaint();
}

void ControlRow::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    if (dropHover)
    {
        g.setColour (Colours::red.withAlpha (0.12f));
        g.fillRect (r);
    }
    g.setColour (Colours::faint);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);

    const bool hasChips = chips != nullptr && chips->count() > 0;
    const int labelW = compact ? juce::jmin (118, (int) (getWidth() * 0.36f)) : juce::jmin (122, getWidth() / 6);
    auto labelR = hasChips ? r.withWidth (labelW).withHeight (getHeight() / 2 + 2) : r.withWidth (labelW);
    g.setColour (Colours::ink);
    auto lf = Fonts::sans (compact ? 11.0f : 12.5f, Fonts::Bold, 0.15f);
    g.setFont (lf);
    const auto label = juce::String::fromUTF8 (spec.label);
    g.drawText (label, labelR, hasChips ? juce::Justification::bottomLeft : juce::Justification::centredLeft);
    if (spec.engine)
    {
        const float tw = juce::GlyphArrangement::getStringWidth (lf, label);
        g.setColour (Colours::red);
        const float cy = hasChips ? labelR.getBottom() - 8.0f : getHeight() * 0.5f - 2.5f;
        g.fillEllipse (tw + 7.0f, cy, 5.0f, 5.0f);
    }
    if (compact) return;

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

void ControlRow::paintOverChildren (juce::Graphics& g)
{
    if (target < 0 || ! proc.getMod().hasRoutesFor (target)) return;
    // live modulated value: a line from the base value to a ring at the current value
    const double base = current();
    const double v = proc.modulatedValue (target);
    const float y = (float) slider.getBounds().getCentreY();
    const float xb = (float) slider.getX() + (float) slider.getPositionOfValue (specToPos (spec, base));
    const float xm = (float) slider.getX() + (float) slider.getPositionOfValue (specToPos (spec, v));
    g.setColour (Colours::ink.withAlpha (0.55f));
    g.fillRect (juce::jmin (xb, xm), y - 1.0f, std::abs (xm - xb), 2.0f);
    g.setColour (Colours::ink);
    g.drawEllipse (juce::Rectangle<float> (11.0f, 11.0f).withCentre ({ xm, y }), 2.0f);
}

void ControlRow::resized()
{
    auto r = getLocalBounds();
    const int labelW = compact ? juce::jmin (118, (int) (getWidth() * 0.36f)) : juce::jmin (122, getWidth() / 6);
    const int boxW = compact ? juce::jlimit (56, 84, getWidth() / 6) : juce::jlimit (70, 112, getWidth() / 6);
    box.setBounds (r.removeFromRight (boxW).reduced (0, juce::jmax (compact ? 4 : 6, (getHeight() - (compact ? 28 : 36)) / 2)));
    r.removeFromRight (compact ? 12 : 18);
    if (chips != nullptr)
        chips->setBounds (0, getHeight() / 2 + 1, labelW + (compact ? 0 : 110), getHeight() / 2 - 2);
    r.removeFromLeft (labelW + 8);
    if (! compact)
    {
        const int valueW = juce::jmax (100, r.getWidth() * 42 / 100);
        r.removeFromLeft (valueW);
    }
    slider.setBounds (r.reduced (0, juce::jmax (4, (getHeight() - 30) / 2)));
}

// ============================================================== Editor

SlowSippinEditor::SlowSippinEditor (SlowSippinProcessor& p)
    : AudioProcessorEditor (&p), proc (p), source (p), output (p), xy (p), xChips (p, ss::tFxX), yChips (p, ss::tFxY)
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
    fmtBox.setScrollWheelEnabled (false);
    bitsBox.setScrollWheelEnabled (false);
    addAndMakeVisible (fmtBox);
    addAndMakeVisible (bitsBox);

    source.onEmptyClick = [this] { openSample(); };
    addAndMakeVisible (source);
    addAndMakeVisible (output);

    for (const auto& c : controlSpecs())
    {
        auto* prm = proc.apvts.getParameter (c.id);
        if (prm == nullptr) continue;
        if (c.group == ControlSpec::Engine || c.group == ControlSpec::Output)
            addAndMakeVisible (rows.add (new ControlRow (proc, c, *prm)));
        else if (c.group == ControlSpec::Fx)
            addChildComponent (fxRows.add (new ControlRow (proc, c, *prm, true)));
    }
    addChildComponent (xy);
    addChildComponent (xChips);
    addChildComponent (yChips);
    for (auto* t : { &engineTab, &fxTab })
    {
        t->setClickingTogglesState (false);
        addAndMakeVisible (t);
    }
    engineTab.onClick = [this] { showTab (0); };
    fxTab.onClick = [this] { showTab (1); };
    engineTab.setTooltip ("Stretch engine, window, reverb, gain, envelope");
    fxTab.setTooltip ("Chorus, dub echo, filter and spectral effects with the XY pad");
    for (int i = 0; i < ss::kNumLfos; ++i) addAndMakeVisible (lfos.add (new ss::LfoTile (proc, i)));

    for (const auto& t : toggleSpecs())
    {
        auto* b = toggles.add (new juce::ToggleButton (juce::String::fromUTF8 (t.label)));
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
    setResizeLimits (1180, 880, 2600, 1800);
    setSize (1360, 960);
    showTab (proc.uiTab);
    startTimerHz (30);
}

void SlowSippinEditor::showTab (int t)
{
    tab = t;
    proc.uiTab = t;
    for (auto* r : rows) r->setVisible (t == 0);
    for (auto* b : toggles) b->setVisible (t == 0);
    for (auto* r : fxRows) r->setVisible (t == 1);
    xy.setVisible (t == 1);
    xChips.setVisible (t == 1);
    yChips.setVisible (t == 1);
    engineTab.setToggleState (t == 0, juce::dontSendNotification);
    fxTab.setToggleState (t == 1, juce::dontSendNotification);
    repaint();
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
    header = r.removeFromTop (84);
    footer = r.removeFromBottom (80);
    lfoStrip = r.removeFromBottom (138);
    const int lw = (int) (r.getWidth() * 0.5f);
    left = r.removeFromLeft (lw);
    right = r;

    {
        auto h = header.reduced (40, 0);
        const int x = h.getX() + juce::jmax (500, (int) (h.getWidth() * 0.40f));
        loadBtn.setBounds (x, h.getCentreY() - 21, 140, 42);
    }

    {
        auto l = left.reduced (36, 22);
        const int avail = l.getHeight() - 2 * 60;
        source.setBounds (l.removeFromTop ((int) (avail * 0.55f)));
        auto row1 = l.removeFromTop (60).withTrimmedTop (12);
        auditionBtn.setBounds (row1.removeFromLeft (130).withHeight (42));
        srcHint = row1.withTrimmedLeft (18).withHeight (42);
        l.removeFromTop (4);
        auto row2 = l.removeFromBottom (60).withTrimmedTop (14);
        output.setBounds (l);
        playBtn.setBounds (row2.removeFromLeft (110).withHeight (44));
        row2.removeFromLeft (14);
        stopBtn.setBounds (row2.removeFromLeft (100).withHeight (44));
        outHint = row2.withTrimmedLeft (18).withHeight (44);
    }

    auto rr = right.reduced (36, 14);
    {
        auto tabs = rr.removeFromTop (34);
        engineTab.setBounds (tabs.removeFromLeft (118));
        tabs.removeFromLeft (8);
        fxTab.setBounds (tabs.removeFromLeft (130));
        rr.removeFromTop (12);
    }
    {
        auto e = rr;
        auto tg = e.removeFromBottom (64);
        const int rowH = juce::jmin (50, e.getHeight() / juce::jmax (1, rows.size()));
        for (auto* row : rows) row->setBounds (e.removeFromTop (rowH));
        tg.removeFromTop (4);
        auto line1 = tg.removeFromTop (30), line2 = tg.removeFromTop (30);
        const int tw = line1.getWidth() / 3;
        for (int i = 0; i < toggles.size(); ++i)
        {
            auto& ln = i < 3 ? line1 : line2;
            toggles[i]->setBounds (ln.removeFromLeft (tw).withWidth (juce::jmin (tw, 200)));
        }
    }
    {
        auto f = rr;
        const int padSize = juce::jmax (160, juce::jmin (f.getHeight() - 100, (int) (f.getWidth() * 0.47f)));
        auto col = f.removeFromLeft (padSize);
        f.removeFromLeft (24);
        xy.setBounds (col.removeFromTop (padSize));
        col.removeFromTop (6);
        xyLabels = col.removeFromTop (24);
        auto xl = xyLabels;
        xl.removeFromLeft (16);
        xChips.setBounds (xl.removeFromLeft (xl.getWidth() / 2 - 12));
        xl.removeFromLeft (28);
        yChips.setBounds (xl);
        col.removeFromTop (8);
        if (! fxRows.isEmpty()) fxRows[0]->setBounds (col.removeFromTop (juce::jmin (44, col.getHeight())));   // FX MIX (dry / wet)

        static const char* names[] = { "CHORUS", "DUB ECHO", "FILTER", "SPECTRAL" };
        static const int counts[] = { 2, 3, 3, 3 };
        const int headH = 20, gap = 6;
        const int rowH = juce::jmin (38, (f.getHeight() - 4 * (headH + gap)) / 11);
        fxHeadings.clear();
        int idx = 1;
        for (int gI = 0; gI < 4; ++gI)
        {
            fxHeadings.add ({ names[gI], f.removeFromTop (headH) });
            for (int k = 0; k < counts[gI] && idx < fxRows.size(); ++k) fxRows[idx++]->setBounds (f.removeFromTop (rowH));
            f.removeFromTop (gap);
        }
    }

    {
        auto st = lfoStrip.reduced (36, 10);
        legendArea = st.removeFromTop (22);
        st.removeFromTop (4);
        const int gap = 10, tw = (st.getWidth() - gap * (ss::kNumLfos - 1)) / ss::kNumLfos;
        for (auto* t : lfos) { t->setBounds (st.removeFromLeft (tw)); st.removeFromLeft (gap); }
    }

    {
        auto f = footer.reduced (40, 16);
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
    g.drawText (U8 ("textures · pads · drones"), h.getX() + (int) tw + 30, h.getCentreY(), 240, 16, juce::Justification::centredLeft);

    const int infoX = loadBtn.getRight() + 22;
    const int statusW = juce::jlimit (90, 360, (int) juce::GlyphArrangement::getStringWidth (Fonts::mono (12.0f), statusShown) + 30);
    auto statusR = juce::Rectangle<int> (h.getRight() - statusW, h.getY(), statusW, h.getHeight());
    auto d = proc.getSample();
    g.setFont (Fonts::mono (13.0f));
    juce::String info;
    if (d != nullptr)
        info << d->name << U8 ("  ·  ") << juce::String ((int) d->sampleRate) << U8 (" Hz · ") << d->numChannels << U8 ("ch · ") << juce::String (d->seconds(), 2) << "s";
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
    g.drawText (U8 ("drag = select · click = point · dbl-click = clear"), srcHint, juce::Justification::centredLeft);
    g.drawFittedText (U8 ("click the output to set the start point · space = play/stop"), outHint, juce::Justification::centredLeft, 2);

    // tabs
    {
        auto tb = (tab == 0 ? engineTab : fxTab).getBounds();
        g.setColour (Colours::red);
        g.fillRect (tb.getX(), tb.getBottom() + 3, tb.getWidth(), 3);
    }
    if (tab == 1)
    {
        g.setFont (Fonts::sans (11.5f, Fonts::Bold, 0.2f));
        for (auto& hd : fxHeadings)
        {
            g.setColour (Colours::ink);
            g.drawText (hd.first, hd.second, juce::Justification::bottomLeft);
            g.setColour (Colours::line);
            g.fillRect (hd.second.getX(), hd.second.getBottom(), hd.second.getWidth(), 1);
        }
        g.setColour (Colours::muted);
        g.setFont (Fonts::sans (10.5f, Fonts::SemiBold, 0.18f));
        g.drawText ("X", xyLabels.withWidth (14), juce::Justification::centredLeft);
        g.drawText ("Y", xChips.getRight() + 10, xyLabels.getY(), 14, xyLabels.getHeight(), juce::Justification::centredLeft);
    }

    // modulation strip
    g.setColour (Colours::line);
    g.fillRect (0, lfoStrip.getY(), getWidth(), 1);
    g.setColour (Colours::ink);
    g.setFont (Fonts::sans (11.5f, Fonts::Bold, 0.2f));
    g.drawText ("MODULATION", legendArea, juce::Justification::centredLeft);
    g.setColour (Colours::muted);
    g.setFont (Fonts::mono (11.0f));
    g.drawFittedText (U8 ("drag an LFO's coloured header onto any control, the XY pad or the waveform  ·  drag a chip up/down = depth  ·  double-click a chip = remove"),
                      legendArea.withTrimmedLeft (118), juce::Justification::centredLeft, 1, 0.8f);

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
        stat ("SOURCE", fmtDur (v.len / v.sampleRate) + U8 (" · ") + juce::String (v.len) + " smp", cw + 30, false);
        const bool looping = proc.apvts.getRawParameterValue ("loop")->load() > 0.5f;
        stat (U8 ("→ OUTPUT"), fmtDur (outputSeconds (s)) + U8 (" · 2ch") + (looping ? juce::String (U8 (" · loop")) : juce::String()), cw, true);
        stat ("MEMORY", fmtMB ((double) d->length * d->numChannels * 8), cw - 30, false);
    }
    else
    {
        stat ("SOURCE", "-", cw + 30, false);
        stat (U8 ("→ OUTPUT"), "-", cw, true);
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
        const double bytes = (double) frames * ch * bitsBox.getSelectedId() / 8.0;
        est = fmtDur ((double) frames / rate) + U8 (" · ") + juce::String (ch) + U8 ("ch · ")
            + (bytes > 1073741824.0 ? juce::String (bytes / 1073741824.0, 2) + " GB" : fmtMB (bytes));
        if (Exporter::tooLong (frames, rate)) est = "longer than 12 hours - select a shorter region";
    }
    g.setColour (Exporter::tooLong (frames, rate) ? Colours::red : Colours::muted);
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
    for (auto* r : fxRows) r->refresh();
    output.update();
    source.syncChips();
    source.repaint();
    xChips.sync();
    yChips.sync();
    if (tab == 1) xy.repaint();
    {
        float v[ss::kNumLfos];
        proc.lfoValuesNow (v);
        for (int i = 0; i < lfos.size(); ++i) lfos[i]->tick (v[i]);
    }

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
    if (Exporter::tooLong (frames, rate)) { setStatus ("longer than 12 hours - select a shorter region or lower STRETCH", true, 6000); return; }

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
