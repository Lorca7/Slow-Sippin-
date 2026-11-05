#pragma once

#include "PluginProcessor.h"
#include "Look.h"

namespace ss
{

/** Source waveform: drag = select, click = start point, double-click = clear. */
class SourceWave : public juce::Component
{
public:
    explicit SourceWave (SlowSippinProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    std::function<void()> onEmptyClick;

private:
    int sampleAt (float x) const;
    SlowSippinProcessor& proc;
    int dragFrom = -1;
    bool dragged = false;
};

/** The output timeline: drawn as it plays; click sets the start point. */
class OutputView : public juce::Component
{
public:
    explicit OutputView (SlowSippinProcessor& p) : proc (p) {}
    void update();                      // called by the editor's timer
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void resized() override { cols.assign ((size_t) juce::jmax (1, getWidth()), 0.0f); }

private:
    SlowSippinProcessor& proc;
    std::vector<float> cols;
    juce::String key;
    float scope[2048] {};
};

/** Text box that commits on Return / focus loss and nudges with the arrow keys. */
class NumberBox : public juce::TextEditor
{
public:
    std::function<void (int dir, bool big)> onNudge;
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k.isKeyCode (juce::KeyPress::upKey) || k.isKeyCode (juce::KeyPress::downKey))
        {
            if (onNudge) onNudge (k.isKeyCode (juce::KeyPress::upKey) ? 1 : -1, k.getModifiers().isShiftDown());
            return true;
        }
        return juce::TextEditor::keyPressed (k);
    }
};

/** LABEL · big value · slider · number box, bound to one parameter. */
class ControlRow : public juce::Component
{
public:
    ControlRow (const ControlSpec& s, juce::RangedAudioParameter& p);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void apply (double v);
    double current() const { return param.convertFrom0to1 (param.getValue()); }
    int decimalsFor (double v) const;

    ControlSpec spec;
    juce::RangedAudioParameter& param;
    juce::Slider slider;
    NumberBox box;
    double shown = std::numeric_limits<double>::quiet_NaN();
    bool dragging = false, updating = false;
};

} // namespace ss

class SlowSippinEditor : public juce::AudioProcessorEditor,
                         public juce::FileDragAndDropTarget,
                         private juce::Timer,
                         private juce::ChangeListener
{
public:
    explicit SlowSippinEditor (SlowSippinProcessor&);
    ~SlowSippinEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    void openSample();
    void startExport();
    void setStatus (const juce::String& s, bool error = false, int holdMs = 5000);
    juce::String exportFileName (const juce::String& ext) const;

    SlowSippinProcessor& proc;
    ss::Look look;
    juce::TextButton loadBtn { "+ SAMPLE" }, auditionBtn { "AUDITION" }, playBtn { "PLAY" }, stopBtn { "STOP" }, exportBtn { "RENDER & SAVE" };
    juce::ComboBox fmtBox, bitsBox;
    ss::SourceWave source;
    ss::OutputView output;
    juce::OwnedArray<ss::ControlRow> rows;
    juce::OwnedArray<juce::ToggleButton> toggles;
    juce::OwnedArray<juce::ButtonParameterAttachment> toggleAtts;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this, 600 };

    juce::Rectangle<int> header, footer, left, right, legendArea, statsArea, srcHint, outHint;
    juce::String status, statusShown;
    bool statusError = false;
    juce::uint32 statusUntil = 0;
    int lastExportState = 0;
    bool dragOver = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlowSippinEditor)
};
