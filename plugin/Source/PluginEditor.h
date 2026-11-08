#pragma once

#include "PluginProcessor.h"
#include "Look.h"
#include "ModUI.h"

namespace ss
{

/** Source waveform: drag = select, click = start point, double-click = clear.
    Drop an LFO here to move the region window (scrub). */
class SourceWave : public juce::Component, public juce::DragAndDropTarget
{
public:
    explicit SourceWave (SlowSippinProcessor& p) : proc (p), chips (p, tWindow, true) { addAndMakeVisible (chips); }
    void paint (juce::Graphics&) override;
    void resized() override { chips.setBounds (getWidth() / 2 - 120, 4, 240, 22); }
    void syncChips() { chips.sync(); }
    bool isInterestedInDragSource (const SourceDetails& d) override { return lfoFromDescription (d.description) >= 0; }
    void itemDragEnter (const SourceDetails&) override { dropHover = true; repaint(); }
    void itemDragExit (const SourceDetails&) override { dropHover = false; repaint(); }
    void itemDropped (const SourceDetails& d) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    std::function<void()> onEmptyClick;

private:
    int sampleAt (float x) const;
    SlowSippinProcessor& proc;
    ChipStrip chips;
    int dragFrom = -1;
    bool dragged = false, dropHover = false;
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

/** LABEL · big value · slider · number box, bound to one parameter.
    Accepts LFOs by drag and drop; shows their chips and the live modulated value. */
class ControlRow : public juce::Component, public juce::DragAndDropTarget
{
public:
    ControlRow (SlowSippinProcessor& proc, const ControlSpec& s, juce::RangedAudioParameter& p, bool compact = false);
    void refresh();
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    const ControlSpec& getSpec() const { return spec; }
    int getTarget() const { return target; }
    /** 0 = not on the pad, 1 = driven by pad X, 2 = by pad Y. */
    void setPadAxis (int axis);

    bool isInterestedInDragSource (const SourceDetails& d) override { return target >= 0 && lfoFromDescription (d.description) >= 0; }
    void itemDragEnter (const SourceDetails&) override { dropHover = true; repaint(); }
    void itemDragExit (const SourceDetails&) override { dropHover = false; repaint(); }
    void itemDropped (const SourceDetails& d) override;

private:
    void apply (double v);
    double current() const { return param.convertFrom0to1 (param.getValue()); }
    int decimalsFor (double v) const;

    SlowSippinProcessor& proc;
    ControlSpec spec;
    juce::RangedAudioParameter& param;
    int target;
    bool compact;
    juce::Slider slider;
    NumberBox box;
    std::unique_ptr<ChipStrip> chips;
    double shown = std::numeric_limits<double>::quiet_NaN();
    bool dragging = false, updating = false, edited = false, dropHover = false;
    int padAxis = 0;
};

} // namespace ss

class SlowSippinEditor : public juce::AudioProcessorEditor,
                         public juce::FileDragAndDropTarget,
                         public juce::DragAndDropContainer,
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
    void toggleRecording();
    void startExport();
    void setStatus (const juce::String& s, bool error = false, int holdMs = 5000);
    void showTab (int t);
    void syncFxControls (bool force = false);
    void setChoice (const char* id, int index);
    void chooseSlot (int slot, int type);
    ss::ControlRow* fxRow (const juce::String& id) const;
    juce::String exportFileName (const juce::String& ext) const;

    SlowSippinProcessor& proc;
    ss::Look look;
    juce::TextButton recBtn { "REC" };
    juce::TextButton loadBtn { "+ SAMPLE" }, auditionBtn { "AUDITION" }, playBtn { "PLAY" }, stopBtn { "STOP" }, exportBtn { "RENDER & SAVE" };
    juce::ComboBox fmtBox, bitsBox;
    ss::SourceWave source;
    ss::OutputView output;
    juce::OwnedArray<ss::ControlRow> rows;          // ENGINE tab
    juce::OwnedArray<ss::ControlRow> fxRows;        // FX tab (FX MIX first)
    juce::TextButton engineTab { "ENGINE" }, fxTab { "EFFECTS" };
    int tab = 0;
    ss::XYPad xy;
    ss::ChipStrip xChips, yChips;
    juce::OwnedArray<ss::LfoTile> lfos;
    juce::ComboBox slotA, slotB, padXBox, padYBox;
    juce::TextButton seriesBtn { "SERIES" }, parallelBtn { "PARALLEL" };
    juce::Rectangle<int> slotAHead, slotBHead, routeLabel, xLabel, yLabel;
    int fxKey = -1;
    juce::OwnedArray<juce::ToggleButton> toggles;
    juce::OwnedArray<juce::ButtonParameterAttachment> toggleAtts;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this, 600 };

    juce::Rectangle<int> header, footer, left, right, legendArea, statsArea, srcHint, outHint, lfoStrip, xyLabels;
    juce::Array<std::pair<juce::String, juce::Rectangle<int>>> fxHeadings;
    juce::String status, statusShown;
    bool statusError = false;
    juce::uint32 statusUntil = 0;
    int lastExportState = 0;
    bool dragOver = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlowSippinEditor)
};
