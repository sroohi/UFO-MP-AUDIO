#pragma once
#include "PluginProcessor.h"
#include "GpuWaveform.h"

class UFOLook final : public juce::LookAndFeel_V4 {
public:
    UFOLook();
    void drawRotarySlider(juce::Graphics&,int,int,int,int,float,float,float,juce::Slider&) override;
};

class UFODropSlider final : public juce::Slider, public juce::DragAndDropTarget {
public:
    int destination = -1;
    std::function<void(int)> assign;
    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;
    void itemDragEnter(const SourceDetails&) override { highlighted=true; repaint(); }
    void itemDragExit(const SourceDetails&) override { highlighted=false; repaint(); }
    void paint(juce::Graphics&) override;
private:
    bool highlighted=false;
};

class UFOModHandle final : public juce::Component {
public:
    explicit UFOModHandle(int slot):index(slot) { setMouseCursor(juce::MouseCursor::DraggingHandCursor); setTooltipText(); }
    void paint(juce::Graphics&) override;
    void mouseDrag(const juce::MouseEvent&) override;
private:
    int index;
    bool started=false;
    void mouseDown(const juce::MouseEvent&) override { started=false; }
    void setTooltipText() { setName("Drag Mod " + juce::String::charToString(juce::juce_wchar('A'+index)) + " onto a destination knob"); }
};

class UFOEditor final : public juce::AudioProcessorEditor, public juce::DragAndDropContainer, private juce::Timer {
public:
    explicit UFOEditor(UFOProcessor&);
    ~UFOEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    using SliderAttachment=juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment=juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment=juce::AudioProcessorValueTreeState::ButtonAttachment;
    struct Dial {
        UFODropSlider slider;
        juce::Label label;
        juce::String id;
        std::unique_ptr<SliderAttachment> attachment;
    };
    struct Menu {
        juce::ComboBox box;
        juce::Label label;
        std::unique_ptr<ComboAttachment> attachment;
    };
    struct Toggle { juce::ToggleButton button; std::unique_ptr<ButtonAttachment> attachment; };
    class ModEditor;
    UFOProcessor& processor;
    UFOLook theme;
    juce::MidiKeyboardComponent keyboard;
    juce::ComboBox presets;
    juce::TextButton panic {"PANIC"}, savePreset {"SAVE"}, loadPreset {"LOAD"},renderMode {"GPU"};
    std::array<std::unique_ptr<GpuWaveform>,2> waveformDisplays;
    std::array<juce::TextButton,2> importButtons,clearButtons;
    std::array<std::unique_ptr<UFOModHandle>,4> handles;
    std::array<juce::TextButton,4> editButtons;
    std::array<std::vector<float>,2> waveforms;
    std::array<juce::String,2> sampleNames,sampleErrors;
    std::vector<juce::File> userPresets;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<ModEditor> modEditor;
    std::vector<std::unique_ptr<Dial>> dials;
    std::vector<std::unique_ptr<Menu>> menus;
    std::vector<std::unique_ptr<Toggle>> toggles;
    int ticks=0;
    bool presetBusy=false;
    juce::String presetJobLabel="Working on preset file...";
    void dial(const juce::String&,const juce::String&,juce::Colour,int destination=-1);
    void menu(const juce::String&,const juce::String&,juce::StringArray);
    void toggle(const juce::String&,const juce::String&);
    void refreshPresets();
    void choosePreset(bool save);
    void runPresetJob(bool save,const juce::File&);
    void updatePresetControls();
    void chooseSample(int layer);
    void showError(const juce::String&);
    void assignMod(int slot,int destination);
    void openModEditor(int slot);
    void timerCallback() override;
};
