/*
 * ModUI — the modulation and effects controls:
 *   LfoTile   one of the 8 LFOs: shape, rate, live trace; drag it onto a target
 *   ModChip   one LFO → target route, shown on the target; drag up/down = depth,
 *             double-click = remove, right-click = invert / remove
 *   ChipStrip the chips of one target
 *   XYPad     blends CHORUS / DUB ECHO / FILTER / SPECTRAL; drop an LFO on it
 */
#pragma once

#include "PluginProcessor.h"
#include "Look.h"

namespace ss
{

inline juce::Colour lfoColour (int i)
{
    static const juce::uint32 c[kNumLfos] = { 0xffc4412b, 0xff2f6f8f, 0xffb07d1a, 0xff2f7f6a,
                                               0xff7b4b7a, 0xff6b7a2e, 0xff4a4642, 0xffa0522d };
    return juce::Colour (c[juce::jlimit (0, kNumLfos - 1, i)]);
}

inline int lfoFromDescription (const juce::var& d)
{
    const auto s = d.toString();
    return s.startsWith ("lfo:") ? s.fromFirstOccurrenceOf ("lfo:", false, false).getIntValue() : -1;
}

/** Small chip image used while dragging an LFO. */
inline juce::Image lfoDragImage (int lfo)
{
    juce::Image img (juce::Image::ARGB, 64, 24, true);
    juce::Graphics g (img);
    g.setColour (lfoColour (lfo));
    g.fillRoundedRectangle (1, 1, 62, 22, 4);
    g.setColour (Colours::redInk);
    g.setFont (Fonts::sans (11.0f, Fonts::Bold, 0.1f));
    g.drawText ("LFO " + juce::String (lfo + 1), 0, 0, 64, 24, juce::Justification::centred);
    return img;
}

// =========================================================================

class ModChip : public juce::Component, public juce::SettableTooltipClient
{
public:
    ModChip (SlowSippinProcessor& p, int r) : proc (p), route (r) { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); }

    int routeIndex() const { return route; }

    void paint (juce::Graphics& g) override
    {
        const auto rd = proc.getMod().route (route);
        if (rd.lfo < 0) return;
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const auto col = lfoColour (rd.lfo);
        g.setColour (col.withAlpha (0.16f));
        g.fillRoundedRectangle (r, 3.0f);
        // depth bar along the bottom, from the centre
        const float cx = r.getCentreX(), bw = r.getWidth() * 0.5f * std::abs (rd.depth);
        g.setColour (col);
        g.fillRect (rd.depth >= 0 ? cx : cx - bw, r.getBottom() - 3.0f, bw, 3.0f);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        g.setColour (Colours::ink);
        g.setFont (Fonts::mono (10.5f, Fonts::Medium));
        const int pct = juce::roundToInt (rd.depth * 100.0f);
        g.drawText (juce::String (rd.lfo + 1) + (pct >= 0 ? " +" : " ") + juce::String (pct), r.withTrimmedBottom (3), juce::Justification::centred);
        setTooltip ("LFO " + juce::String (rd.lfo + 1) + ", depth " + juce::String (pct) + "%. Drag up/down to change, double-click to remove.");
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem (1, "Invert depth");
            m.addItem (2, "Remove");
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                             [sp = juce::Component::SafePointer<ModChip> (this)] (int res)
                             {
                                 if (sp == nullptr) return;
                                 auto& mod = sp->proc.getMod();
                                 if (res == 1) mod.setDepth (sp->route, -mod.route (sp->route).depth);
                                 if (res == 2) mod.removeRoute (sp->route);
                             });
            return;
        }
        startDepth = proc.getMod().route (route).depth;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        const float scale = e.mods.isShiftDown() ? 600.0f : 120.0f;
        proc.getMod().setDepth (route, juce::jlimit (-1.0f, 1.0f, startDepth - (float) e.getDistanceFromDragStartY() / scale));
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { proc.getMod().removeRoute (route); }

private:
    SlowSippinProcessor& proc;
    int route;
    float startDepth = 0;
};

// =========================================================================

class ChipStrip : public juce::Component
{
public:
    ChipStrip (SlowSippinProcessor& p, int t, bool alignRight = false) : proc (p), target (t), right (alignRight)
    {
        setInterceptsMouseClicks (false, true);
    }

    /** Rebuild when the routes changed. Returns true if anything changed. */
    bool sync()
    {
        const int v = proc.getMod().version();
        if (v == seenVersion) { for (auto* c : chips) c->repaint(); return false; }
        seenVersion = v;
        chips.clear();
        for (int r = 0; r < kMaxRoutes; ++r)
        {
            const auto rd = proc.getMod().route (r);
            if (rd.lfo >= 0 && rd.target == target) addAndMakeVisible (chips.add (new ModChip (proc, r)));
        }
        resized();
        return true;
    }

    int count() const { return chips.size(); }

    void resized() override
    {
        const int w = 44, gap = 4, h = juce::jmin (20, getHeight());
        int x = right ? getWidth() - w : 0;
        for (auto* c : chips)
        {
            c->setBounds (x, (getHeight() - h) / 2, w, h);
            x += right ? -(w + gap) : (w + gap);
        }
    }

private:
    SlowSippinProcessor& proc;
    int target;
    bool right;
    int seenVersion = -1;
    juce::OwnedArray<ModChip> chips;
};

// =========================================================================

class XYPad : public juce::Component, public juce::DragAndDropTarget, public juce::SettableTooltipClient
{
public:
    explicit XYPad (SlowSippinProcessor& p) : proc (p)
    {
        px = proc.apvts.getParameter ("fxX");
        py = proc.apvts.getParameter ("fxY");
        setTooltip ("Drag to blend the four effects. Turn up FX MIX to hear them. Drop an LFO here to move the pad.");
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::waveBg);
        g.fillRect (r);
        g.setColour (Colours::faint);
        for (int i = 1; i < 4; ++i)
        {
            g.fillRect (r.getX() + r.getWidth() * i / 4.0f, r.getY(), 1.0f, r.getHeight());
            g.fillRect (r.getX(), r.getY() + r.getHeight() * i / 4.0f, r.getWidth(), 1.0f);
        }
        g.setFont (Fonts::sans (11.0f, Fonts::Bold, 0.18f));
        g.setColour (Colours::ink2);
        auto in = r.reduced (10, 8);
        g.drawText ("CHORUS", in, juce::Justification::topLeft);
        g.drawText ("DUB ECHO", in, juce::Justification::topRight);
        g.drawText ("FILTER", in, juce::Justification::bottomLeft);
        g.drawText ("SPECTRAL", in, juce::Justification::bottomRight);

        const float mix = proc.apvts.getRawParameterValue ("fxMix")->load();
        if (mix <= 0.0f)
        {
            g.setColour (Colours::muted);
            g.setFont (Fonts::mono (11.0f));
            g.drawText ("FX MIX is 0 - turn it up to hear the effects", r.withTrimmedBottom (r.getHeight() * 0.62f), juce::Justification::centredBottom);
        }

        // base position (red puck) and modulated position (hollow ring)
        const float bx = proc.apvts.getRawParameterValue ("fxX")->load() / 100.0f;
        const float by = proc.apvts.getRawParameterValue ("fxY")->load() / 100.0f;
        const auto base = toPoint (bx, by);
        if (proc.getMod().hasRoutesFor (tFxX) || proc.getMod().hasRoutesFor (tFxY))
        {
            const auto m = toPoint (proc.modulatedValue (tFxX) / 100.0f, proc.modulatedValue (tFxY) / 100.0f);
            g.setColour (Colours::ink.withAlpha (0.5f));
            g.drawLine ({ base, m }, 1.0f);
            g.setColour (Colours::ink);
            g.drawEllipse (juce::Rectangle<float> (14, 14).withCentre (m), 2.0f);
        }
        g.setColour (Colours::red);
        g.fillEllipse (juce::Rectangle<float> (16, 16).withCentre (base));

        if (dropHover)
        {
            g.setColour (Colours::red);
            g.drawRect (r, 3.0f);
        }
        else
        {
            g.setColour (Colours::line);
            g.drawRect (r, 2.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        px->beginChangeGesture();
        py->beginChangeGesture();
        mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const float x = juce::jlimit (0.0f, 1.0f, (e.position.x - pad) / (getWidth() - 2 * pad));
        const float y = juce::jlimit (0.0f, 1.0f, 1.0f - (e.position.y - pad) / (getHeight() - 2 * pad));
        px->setValueNotifyingHost (px->convertTo0to1 (std::round (x * 100.0f)));
        py->setValueNotifyingHost (py->convertTo0to1 (std::round (y * 100.0f)));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        px->endChangeGesture();
        py->endChangeGesture();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        px->setValueNotifyingHost (px->convertTo0to1 (50.0f));
        py->setValueNotifyingHost (py->convertTo0to1 (50.0f));
    }

    bool isInterestedInDragSource (const SourceDetails& d) override { return lfoFromDescription (d.description) >= 0; }
    void itemDragEnter (const SourceDetails&) override { dropHover = true; repaint(); }
    void itemDragExit (const SourceDetails&) override { dropHover = false; repaint(); }
    void itemDropped (const SourceDetails& d) override
    {
        dropHover = false;
        repaint();
        const int lfo = lfoFromDescription (d.description);
        juce::PopupMenu m;
        m.addSectionHeader ("LFO " + juce::String (lfo + 1) + " moves the pad");
        m.addItem (1, "Left / right (X)");
        m.addItem (2, "Up / down (Y)");
        m.addItem (3, "Both (diagonal)");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                         [&mod = proc.getMod(), lfo] (int res)
                         {
                             if (res == 1 || res == 3) mod.addRoute (lfo, tFxX, 0.5f);
                             if (res == 2 || res == 3) mod.addRoute (lfo, tFxY, 0.5f);
                         });
    }

private:
    juce::Point<float> toPoint (float x, float y) const
    {
        return { pad + x * (getWidth() - 2 * pad), pad + (1.0f - y) * (getHeight() - 2 * pad) };
    }

    SlowSippinProcessor& proc;
    juce::RangedAudioParameter* px = nullptr;
    juce::RangedAudioParameter* py = nullptr;
    const float pad = 12.0f;
    bool dropHover = false;
};

// =========================================================================

class LfoTile : public juce::Component, public juce::SettableTooltipClient
{
public:
    LfoTile (SlowSippinProcessor& p, int i) : proc (p), index (i)
    {
        rateParam = proc.apvts.getParameter (lfoRateId (i));
        shapeParam = proc.apvts.getParameter (lfoShapeId (i));
        rate.setSliderStyle (juce::Slider::LinearHorizontal);
        rate.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        rate.setScrollWheelEnabled (false);
        rate.setTooltip ("LFO " + juce::String (i + 1) + " rate");
        rateAtt = std::make_unique<juce::SliderParameterAttachment> (*rateParam, rate);
        addAndMakeVisible (rate);
        setTooltip ("LFO " + juce::String (i + 1) + ": drag the header onto any control, the XY pad or the waveform. Click the shape to change it.");
        history.fill (0.0f);
    }

    void tick (float value)
    {
        history[(size_t) head] = value;
        head = (head + 1) % (int) history.size();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const auto col = lfoColour (index);
        g.setColour (Colours::waveBg);
        g.fillRect (r);
        g.setColour (Colours::line);
        g.drawRect (r, 1.5f);

        // header (drag handle)
        auto hdr = headerArea().toFloat();
        g.setColour (col);
        g.fillRect (hdr);
        g.setColour (Colours::redInk);
        g.setFont (Fonts::sans (11.5f, Fonts::Bold, 0.14f));
        g.drawText ("LFO " + juce::String (index + 1), hdr.reduced (8, 0), juce::Justification::centredLeft);
        for (int k = 0; k < 3; ++k)            // grip
            for (int j = 0; j < 2; ++j)
                g.fillEllipse (hdr.getRight() - 16.0f + j * 5.0f, hdr.getY() + 6.0f + k * 4.5f, 2.2f, 2.2f);

        // shape name (click to change)
        g.setColour (Colours::ink2);
        g.setFont (Fonts::mono (10.5f, Fonts::Medium));
        g.drawText (shapeParam->getCurrentValueAsText(), shapeArea(), juce::Justification::centredRight);

        // live trace
        auto tr = traceArea().toFloat();
        g.setColour (Colours::faint);
        g.fillRect (tr.getX(), tr.getCentreY(), tr.getWidth(), 1.0f);
        juce::Path p;
        const int n = (int) history.size();
        for (int k = 0; k < n; ++k)
        {
            const float v = history[(size_t) ((head + k) % n)];
            const float x = tr.getX() + tr.getWidth() * k / (n - 1);
            const float y = tr.getCentreY() - v * tr.getHeight() * 0.45f;
            if (k == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (col);
        g.strokePath (p, juce::PathStrokeType (1.6f));

        // rate value + route count
        g.setColour (Colours::ink);
        g.setFont (Fonts::mono (11.0f));
        const float hz = rateParam->convertFrom0to1 (rateParam->getValue());
        g.drawText (juce::String (hz, hz < 1 ? 2 : 1) + " Hz", rateTextArea(), juce::Justification::centredRight);
        int routes = 0;
        for (int k = 0; k < kMaxRoutes; ++k) if (proc.getMod().route (k).lfo == index) ++routes;
        g.setColour (Colours::muted);
        g.setFont (Fonts::mono (10.0f));
        g.drawText (routes == 0 ? juce::String ("drag me") : juce::String::fromUTF8 ("→ ") + juce::String (routes), shapeArea().withX (8).withWidth (70), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        auto bottom = r.removeFromBottom (20);
        bottom.removeFromRight (rateTextArea().getWidth());
        rate.setBounds (bottom);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (shapeArea().contains (e.getPosition()))
        {
            juce::PopupMenu m;
            const auto names = lfoShapeNames();
            const int cur = (int) std::lround (shapeParam->convertFrom0to1 (shapeParam->getValue()));
            for (int s = 0; s < names.size(); ++s) m.addItem (s + 1, names[s], true, s == cur);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                             [prm = shapeParam] (int res)
                             {
                                 if (res <= 0) return;
                                 prm->beginChangeGesture();
                                 prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (res - 1)));
                                 prm->endChangeGesture();
                             });
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() < 4 || ! (headerArea().contains (e.getMouseDownPosition()) || traceArea().contains (e.getMouseDownPosition())))
            return;
        if (auto* dc = juce::DragAndDropContainer::findParentDragContainerFor (this))
            if (! dc->isDragAndDropActive())
                dc->startDragging ("lfo:" + juce::String (index), this, juce::ScaledImage (lfoDragImage (index)));
    }

private:
    juce::Rectangle<int> headerArea() const { return getLocalBounds().reduced (1).removeFromTop (24); }
    juce::Rectangle<int> shapeArea() const { return { 0, 26, getWidth() - 8, 18 }; }
    juce::Rectangle<int> traceArea() const { return { 8, 46, getWidth() - 16, juce::jmax (10, getHeight() - 46 - 30) }; }
    juce::Rectangle<int> rateTextArea() const { return { getWidth() - 62, getHeight() - 26, 56, 20 }; }

    SlowSippinProcessor& proc;
    int index;
    juce::RangedAudioParameter* rateParam = nullptr;
    juce::RangedAudioParameter* shapeParam = nullptr;
    juce::Slider rate;
    std::unique_ptr<juce::SliderParameterAttachment> rateAtt;
    std::array<float, 90> history {};
    int head = 0;
};

} // namespace ss
