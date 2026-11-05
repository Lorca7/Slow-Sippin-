#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

namespace ss
{

namespace Colours
{
    const juce::Colour bg       { 0xffd9d5cc };
    const juce::Colour waveBg   { 0xffd2cec5 };
    const juce::Colour ink      { 0xff211f1c };
    const juce::Colour ink2     { 0xff3a3733 };
    const juce::Colour muted    { 0xff8a867d };
    const juce::Colour faint    { 0xffbdb8ae };
    const juce::Colour line     { 0xff2a2825 };
    const juce::Colour red      { 0xffc4412b };
    const juce::Colour redInk   { 0xfff4efe7 };
    const juce::Colour grey     { 0xff7d7970 };
    const juce::Colour box      { 0xffcecac1 };
    const juce::Colour boxLine  { 0xffc0bbb1 };
}

/** Embedded fonts: Archivo (labels) and IBM Plex Mono (numbers, info). */
struct Fonts
{
    enum Weight { Light, Regular, Medium, SemiBold, Bold, ExtraBold };

    static juce::Font sans (float h, Weight w = Bold, float kerning = 0.14f)
    {
        auto& f = get();
        auto tf = w == ExtraBold ? f.sansXB : w == SemiBold ? f.sansSB : f.sansB;
        return juce::Font (juce::FontOptions (tf).withHeight (h).withKerningFactor (kerning));
    }

    static juce::Font mono (float h, Weight w = Regular, float kerning = 0.02f)
    {
        auto& f = get();
        auto tf = w == Light ? f.monoL : w == Medium ? f.monoM : f.monoR;
        return juce::Font (juce::FontOptions (tf).withHeight (h).withKerningFactor (kerning));
    }

private:
    static Fonts& get() { static Fonts f; return f; }
    Fonts()
    {
        auto load = [] (const char* d, int s) { return juce::Typeface::createSystemTypefaceFor (d, (size_t) s); };
        sansSB = load (BinaryData::ArchivoSemiBold_ttf, BinaryData::ArchivoSemiBold_ttfSize);
        sansB  = load (BinaryData::ArchivoBold_ttf, BinaryData::ArchivoBold_ttfSize);
        sansXB = load (BinaryData::ArchivoExtraBold_ttf, BinaryData::ArchivoExtraBold_ttfSize);
        monoL  = load (BinaryData::IBMPlexMonoLight_ttf, BinaryData::IBMPlexMonoLight_ttfSize);
        monoR  = load (BinaryData::IBMPlexMonoRegular_ttf, BinaryData::IBMPlexMonoRegular_ttfSize);
        monoM  = load (BinaryData::IBMPlexMonoMedium_ttf, BinaryData::IBMPlexMonoMedium_ttfSize);
    }
    juce::Typeface::Ptr sansSB, sansB, sansXB, monoL, monoR, monoM;
};

class Look : public juce::LookAndFeel_V4
{
public:
    Look()
    {
        using namespace juce;
        setColour (ResizableWindow::backgroundColourId, Colours::bg);
        setColour (TextButton::textColourOffId, Colours::ink);
        setColour (TextButton::textColourOnId, Colours::bg);
        setColour (TextEditor::backgroundColourId, Colours::box);
        setColour (TextEditor::textColourId, Colours::ink);
        setColour (TextEditor::highlightColourId, Colours::red.withAlpha (0.3f));
        setColour (TextEditor::highlightedTextColourId, Colours::ink);
        setColour (TextEditor::outlineColourId, Colours::boxLine);
        setColour (TextEditor::focusedOutlineColourId, Colours::red);
        setColour (CaretComponent::caretColourId, Colours::red);
        setColour (ComboBox::backgroundColourId, Colours::box);
        setColour (ComboBox::textColourId, Colours::ink);
        setColour (ComboBox::outlineColourId, Colours::boxLine);
        setColour (ComboBox::arrowColourId, Colours::ink);
        setColour (PopupMenu::backgroundColourId, Colours::bg);
        setColour (PopupMenu::textColourId, Colours::ink);
        setColour (PopupMenu::highlightedBackgroundColourId, Colours::ink);
        setColour (PopupMenu::highlightedTextColourId, Colours::bg);
        setColour (Label::textColourId, Colours::ink);
        setColour (TooltipWindow::backgroundColourId, Colours::ink);
        setColour (TooltipWindow::textColourId, Colours::bg);
        setColour (AlertWindow::backgroundColourId, Colours::bg);
        setColour (AlertWindow::textColourId, Colours::ink);
        setColour (ScrollBar::thumbColourId, Colours::muted);
    }

    // ---- buttons: outlined; "primary" = red fill; toggled-on = ink fill
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.0f);
        const bool primary = b.getProperties()["primary"];
        const bool on = b.getToggleState();
        const float a = b.isEnabled() ? 1.0f : 0.35f;
        if (primary)
        {
            g.setColour ((over ? Colours::red.darker (0.15f) : Colours::red).withMultipliedAlpha (a));
            g.fillRect (r);
            return;
        }
        if (on) { g.setColour (Colours::ink.withMultipliedAlpha (a)); g.fillRect (r); }
        else if (over || down) { g.setColour (juce::Colours::black.withAlpha (0.05f)); g.fillRect (r); }
        g.setColour (Colours::line.withMultipliedAlpha (a));
        g.drawRect (r, 2.0f);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        const bool primary = b.getProperties()["primary"];
        const float a = b.isEnabled() ? 1.0f : 0.35f;
        g.setColour ((primary ? Colours::redInk : b.getToggleState() ? Colours::bg : Colours::ink).withMultipliedAlpha (a));
        g.setFont (Fonts::sans (13.0f, Fonts::SemiBold, 0.13f));
        g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
    }

    // ---- toggles: red square + bold label
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool) override
    {
        const float a = b.isEnabled() ? 1.0f : 0.35f;
        const auto h = (float) b.getHeight();
        juce::Rectangle<float> sq (1.0f, (h - 18.0f) / 2.0f, 18.0f, 18.0f);
        g.setColour (Colours::red.withMultipliedAlpha (a));
        if (b.getToggleState()) g.fillRect (sq);
        else g.drawRect (sq, 2.0f);
        if (over && b.isEnabled()) { g.setColour (Colours::red.withAlpha (0.15f)); g.fillRect (sq.expanded (3.0f)); }
        g.setColour (Colours::ink.withMultipliedAlpha (a));
        g.setFont (Fonts::sans (12.5f, Fonts::Bold, 0.14f));
        g.drawText (b.getButtonText(), b.getLocalBounds().withTrimmedLeft (30), juce::Justification::centredLeft);
    }

    // ---- slider: thin line, heavier left part, red rectangular thumb
    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle, juce::Slider& s) override
    {
        const float cy = y + h * 0.5f;
        const float tw = 11.0f, th = juce::jmin (26.0f, (float) h - 2.0f);
        g.setColour (Colours::ink2);
        g.fillRect ((float) x, cy - 0.5f, (float) w, 1.0f);
        g.setColour (Colours::line);
        g.fillRect ((float) x, cy - 1.5f, pos - x, 3.0f);
        g.setColour (s.isMouseOverOrDragging() ? Colours::red.darker (0.12f) : Colours::red);
        g.fillRect (juce::jlimit ((float) x, (float) (x + w) - tw, pos - tw * 0.5f), cy - th * 0.5f, tw, th);
    }

    int getSliderThumbRadius (juce::Slider&) override { return 6; }

    // ---- text boxes
    void fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& e) override
    {
        g.setColour (e.hasKeyboardFocus (true) ? Colours::box.brighter (0.25f) : Colours::box);
        g.fillRect (0, 0, w, h);
    }

    void drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e) override
    {
        g.setColour (e.hasKeyboardFocus (true) ? Colours::red : Colours::boxLine);
        g.drawRect (0, 0, w, h, e.hasKeyboardFocus (true) ? 2 : 1);
    }

    // ---- combo boxes
    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        g.setColour (Colours::box);
        g.fillRect (0, 0, w, h);
        g.setColour (box.hasKeyboardFocus (false) ? Colours::red : Colours::boxLine);
        g.drawRect (0, 0, w, h, 1);
        juce::Path p;
        const float ax = (float) w - 22.0f, ay = h * 0.5f - 2.5f;
        p.startNewSubPath (ax, ay);
        p.lineTo (ax + 5.0f, ay + 5.0f);
        p.lineTo (ax + 10.0f, ay);
        g.setColour (Colours::ink);
        g.strokePath (p, juce::PathStrokeType (2.0f));
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return Fonts::mono (14.0f); }
    juce::Font getPopupMenuFont() override { return Fonts::mono (14.0f); }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (12, 1, box.getWidth() - 36, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.fillAll (Colours::bg);
        g.setColour (Colours::line);
        g.drawRect (0, 0, w, h, 1);
    }
};

} // namespace ss
