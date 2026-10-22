#include "PluginEditor.h"
using namespace juce;
namespace {
const Colour mint(0xffb5f5c4), violet(0xffb7a4f5), ink(0xff101412), panel(0xff1a211d), muted(0xff91a096);
const std::array<Colour,4> routeColours {Colour(0xff8de0ea),Colour(0xffffc180),Colour(0xffb7a4f5),Colour(0xfff2a9c7)};
int draggedSlot(const DragAndDropTarget::SourceDetails& details) {
    auto description=details.description.toString();
    if(!description.startsWith("UFO:mod:")) return -1;
    int slot=description.substring(8).getIntValue();
    return slot>=0 && slot<4 ? slot : -1;
}
void formatSlider(Slider& slider) {
    slider.textFromValueFunction=[](double v){ return String(v,std::abs(v)>=100?0:2); };
    slider.updateText();
}
}
UFOLook::UFOLook() {
    setColour(Slider::textBoxTextColourId,Colour(0xffe3ece5)); setColour(Slider::textBoxOutlineColourId,Colours::transparentBlack);
    setColour(ComboBox::backgroundColourId,panel); setColour(ComboBox::outlineColourId,Colour(0xff3b4940));
    setColour(ComboBox::textColourId,Colour(0xffe3ece5)); setColour(PopupMenu::backgroundColourId,panel);
    setColour(ToggleButton::textColourId,muted); setColour(ToggleButton::tickColourId,mint);
    setColour(TextButton::buttonColourId,panel); setColour(TextButton::textColourOffId,mint);
    setColour(Label::textColourId,muted);
}
void UFOLook::drawRotarySlider(Graphics& g,int x,int y,int w,int h,float position,float start,float end,Slider& s) {
    auto bounds=Rectangle<float>(float(x),float(y),float(w),float(h)).reduced(8);
    float radius=std::min(bounds.getWidth(),bounds.getHeight())*.5f;
    auto center=bounds.getCentre(); float angle=start+position*(end-start);
    Path track; track.addCentredArc(center.x,center.y,radius,radius,0,start,end,true);
    g.setColour(Colour(0xff36423a)); g.strokePath(track,PathStrokeType(3));
    Path arc; arc.addCentredArc(center.x,center.y,radius,radius,0,start,angle,true);
    g.setColour(s.findColour(Slider::rotarySliderFillColourId)); g.strokePath(arc,PathStrokeType(3));
    g.setColour(Colour(0xff253128)); g.fillEllipse(center.x-radius+5,center.y-radius+5,2*radius-10,2*radius-10);
    g.setColour(s.findColour(Slider::rotarySliderFillColourId));
    g.drawLine(center.x+std::sin(angle)*radius*.45f,center.y-std::cos(angle)*radius*.45f,center.x+std::sin(angle)*radius*.78f,center.y-std::cos(angle)*radius*.78f,2);
    for(size_t slot=0;slot<4;++slot) {
        auto active=s.getProperties().getWithDefault("modActive"+String(int(slot)),false);
        if(!bool(active)) continue;
        float movement=float(s.getProperties().getWithDefault("modLevel"+String(int(slot)),0.f));
        float target=start+jlimit(0.f,1.f,position+movement*.25f)*(end-start);
        if(std::abs(target-angle)<.015f) target=std::min(end,angle+.015f);
        float outer=radius+4+float(slot)*1.7f;
        Path route;route.addCentredArc(center.x,center.y,outer,outer,0,std::min(angle,target),std::max(angle,target),true);
        g.setColour(routeColours[slot].withAlpha(.85f));g.strokePath(route,PathStrokeType(1.5f));
    }
}
bool UFODropSlider::isInterestedInDragSource(const SourceDetails& details) { return destination>=0 && draggedSlot(details)>=0; }
void UFODropSlider::itemDropped(const SourceDetails& details) { highlighted=false; if(assign && draggedSlot(details)>=0) assign(draggedSlot(details)); repaint(); }
void UFODropSlider::paint(Graphics& g) {
    Slider::paint(g);
    if(highlighted){g.setColour(mint.withAlpha(.25f));g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1),5);}
}
void UFOModHandle::paint(Graphics& g) {
    auto color=routeColours[size_t(index)];g.setColour(color.withAlpha(.15f));g.fillRoundedRectangle(getLocalBounds().toFloat(),4);
    g.setColour(color);g.fillEllipse(7,7,7,7);g.setFont(FontOptions(11).withStyle("Bold"));
    g.drawText("MOD "+String::charToString(juce_wchar('A'+index))+"  /  DRAG",22,0,getWidth()-25,getHeight(),Justification::centredLeft);
}
void UFOModHandle::mouseDrag(const MouseEvent& event) {
    if(started || event.getDistanceFromDragStart()<4) return;
    if(auto* container=DragAndDropContainer::findParentDragContainerFor(this)) { started=true;container->startDragging("UFO:mod:"+String(index),this); }
}

class UFOEditor::ModEditor final : public Component {
public:
    ModEditor(UFOProcessor& processor,int index,std::function<void()> dismiss):slot(index),close("DONE") {
        String prefix="mod"+String(slot);title="MOD "+String::charToString(juce_wchar('A'+slot))+" / PATTERN & ENVELOPE";
        addAndMakeVisible(close);close.onClick=std::move(dismiss);
        length.addItemList({"8 steps","16 steps"},1);length.setComponentID(prefix+"Length");addAndMakeVisible(length);
        lengthAttachment=std::make_unique<ComboAttachment>(processor.state,prefix+"Length",length);
        auto add=[&](Slider& slider,Label& label,const String& id,const String& name,bool rotary){
            slider.setSliderStyle(rotary?Slider::RotaryHorizontalVerticalDrag:Slider::LinearVertical);
            slider.setTextBoxStyle(Slider::TextBoxBelow,false,62,18);slider.setColour(Slider::rotarySliderFillColourId,routeColours[size_t(slot)]);
            slider.setColour(Slider::trackColourId,routeColours[size_t(slot)]);slider.setName(name);slider.setComponentID(id);
            label.setText(name,dontSendNotification);label.setJustificationType(Justification::centred);label.setFont(FontOptions(11));
            addAndMakeVisible(slider);addAndMakeVisible(label);
            attachments.push_back(std::make_unique<SliderAttachment>(processor.state,id,slider));formatSlider(slider);
        };
        add(shift,shiftLabel,prefix+"Shift","ROTATE",true);
        const StringArray names{"ATTACK","DECAY","SUSTAIN","RELEASE"},suffixes{"EnvAttack","EnvDecay","EnvSustain","EnvRelease"};
        for(int i=0;i<4;++i)add(envelope[size_t(i)],envelopeLabels[size_t(i)],prefix+suffixes[i],names[i],true);
        for(int i=0;i<16;++i)add(steps[size_t(i)],stepLabels[size_t(i)],prefix+"Step"+String(i),String(i+1),false);
        length.onChange=[this]{updateSteps();};updateSteps();
    }
    void paint(Graphics& g) override {
        g.setColour(Colours::black.withAlpha(.7f));g.fillAll();
        auto b=getLocalBounds().toFloat().reduced(180,115);g.setColour(ink);g.fillRoundedRectangle(b,12);g.setColour(routeColours[size_t(slot)].withAlpha(.5f));g.drawRoundedRectangle(b,12,1);
        g.setColour(routeColours[size_t(slot)]);g.setFont(FontOptions(18).withStyle("Bold"));g.drawText(title,210,137,660,30,Justification::centredLeft);
        g.setColour(muted);g.setFont(FontOptions(12));g.drawText("Sequence steps range from -1 to +1. Length and rotation also shape Euclidean pulses.",210,182,780,24,Justification::centredLeft);
        g.drawText("POLYPHONIC ENVELOPE",210,490,500,24,Justification::centredLeft);
        g.drawText("Each note starts its own envelope. Close this panel, then drag a Mod handle onto a destination knob.",210,735,770,30,Justification::centredLeft);
    }
    void resized() override {
        close.setBounds(900,138,78,28);length.setBounds(214,222,180,28);shiftLabel.setBounds(420,210,100,18);shift.setBounds(420,229,100,90);
        for(int i=0;i<16;++i){int x=210+i*49;stepLabels[size_t(i)].setBounds(x,319,46,18);steps[size_t(i)].setBounds(x,340,46,120);}
        for(int i=0;i<4;++i){int x=230+i*180;envelopeLabels[size_t(i)].setBounds(x,525,150,20);envelope[size_t(i)].setBounds(x,550,150,140);}
    }
private:
    void updateSteps(){int count=length.getSelectedId()==1?8:16;for(int i=0;i<16;++i){steps[size_t(i)].setEnabled(i<count);stepLabels[size_t(i)].setAlpha(i<count?1.f:.35f);}}
    int slot;String title;TextButton close;ComboBox length;Slider shift;Label shiftLabel;
    std::array<Slider,16> steps;std::array<Label,16> stepLabels;
    std::array<Slider,4> envelope;std::array<Label,4> envelopeLabels;
    std::vector<std::unique_ptr<SliderAttachment>> attachments;
    std::unique_ptr<ComboAttachment> lengthAttachment;
};

void UFOEditor::dial(const String& id,const String& name,Colour color,int destination) {
    auto d=std::make_unique<Dial>();d->id=id;d->slider.destination=destination;d->slider.setComponentID(id);
    d->slider.assign=[this,destination](int slot){assignMod(slot,destination);};
    d->slider.setSliderStyle(Slider::RotaryHorizontalVerticalDrag);d->slider.setTextBoxStyle(Slider::TextBoxBelow,false,74,17);
    d->slider.setColour(Slider::rotarySliderFillColourId,color);d->slider.setName(name);d->slider.setTooltip(destination>=0?name+" / drop a Mod handle here":name);
    d->label.setText(name,dontSendNotification);d->label.setJustificationType(Justification::centred);d->label.setColour(Label::textColourId,muted);d->label.setFont(FontOptions(11));
    addAndMakeVisible(d->slider);addAndMakeVisible(d->label);
    d->attachment=std::make_unique<SliderAttachment>(processor.state,id,d->slider);formatSlider(d->slider);dials.push_back(std::move(d));
}
void UFOEditor::menu(const String& id,const String& name,StringArray options) {
    auto m=std::make_unique<Menu>();m->box.addItemList(options,1);m->box.setName(name);
    m->label.setText(name,dontSendNotification);m->label.setColour(Label::textColourId,muted);m->label.setFont(FontOptions(11));
    addAndMakeVisible(m->box);addAndMakeVisible(m->label);m->attachment=std::make_unique<ComboAttachment>(processor.state,id,m->box);menus.push_back(std::move(m));
}
void UFOEditor::toggle(const String& id,const String& name) {
    auto t=std::make_unique<Toggle>();t->button.setButtonText(name);addAndMakeVisible(t->button);t->attachment=std::make_unique<ButtonAttachment>(processor.state,id,t->button);toggles.push_back(std::move(t));
}
UFOEditor::UFOEditor(UFOProcessor& p):AudioProcessorEditor(p),processor(p),keyboard(p.keyboard,MidiKeyboardComponent::horizontalKeyboard) {
    setLookAndFeel(&theme);refreshPresets();
    presets.onChange=[this]{int id=presets.getSelectedId();if(id>=1 && id<=4)processor.loadFactory(id-1);else if(id>=100 && id<int(100+userPresets.size()))runPresetJob(false,userPresets[size_t(id-100)]);};addAndMakeVisible(presets);
    panic.onClick=[this]{processor.panicRequested.store(true);};addAndMakeVisible(panic);
    savePreset.onClick=[this]{choosePreset(true);};loadPreset.onClick=[this]{choosePreset(false);};addAndMakeVisible(savePreset);addAndMakeVisible(loadPreset);
    renderMode.setComponentID("gpuMode");renderMode.setTooltip("GPU waveform display. Switch to CPU if your host has graphics issues.");
    renderMode.onClick=[this]{const bool enable=!waveformDisplays[0]->diagnostics().active;for(auto& display:waveformDisplays)display->setGpuEnabled(enable);};addAndMakeVisible(renderMode);
    for(int layer=0;layer<2;++layer) {
        String prefix=layer==0?"upper":"lower";auto color=layer==0?mint:violet;
        waveformDisplays[size_t(layer)]=std::make_unique<GpuWaveform>(color);waveformDisplays[size_t(layer)]->setComponentID(prefix+"GpuWaveform");addAndMakeVisible(*waveformDisplays[size_t(layer)]);
        menu(prefix+"Bank","WAVE BANK",{"Solar","Hollow","Prism","Chaos"});menu(prefix+"Shaper","SHAPER",{"SVF","LPG","Resonator"});menu(prefix+"Filter","FILTER",{"Low pass","Band pass","High pass"});menu(prefix+"Octave","OCTAVE",{"-1","0","+1"});toggle(prefix+"Oneshot","One shot");
        importButtons[size_t(layer)].setButtonText("IMPORT AUDIO");importButtons[size_t(layer)].setComponentID(prefix+"Import");importButtons[size_t(layer)].onClick=[this,layer]{chooseSample(layer);};addAndMakeVisible(importButtons[size_t(layer)]);
        clearButtons[size_t(layer)].setButtonText("CLEAR");clearButtons[size_t(layer)].setComponentID(prefix+"Clear");clearButtons[size_t(layer)].onClick=[this,layer]{processor.clearSample(layer);};addAndMakeVisible(clearButtons[size_t(layer)]);
        for(auto pair:{std::pair{"Color","COLOR"},{"Movement","MOVEMENT"},{"Attack","SCAN A"},{"Decay","SCAN D"},{"Pitch","SEMITONES"},{"Cutoff","FREQUENCY"},{"Resonance","RESONANCE"},{"Slew","LPG LAG"},{"Tilt","RES TILT"},{"Spread","RES SPREAD"}}) {
            int target=String(pair.first)=="Color"?layer:String(pair.first)=="Cutoff"?2+layer:String(pair.first)=="Pitch"?4+layer:-1;dial(prefix+pair.first,pair.second,color,target);
        }
    }
    for(int i=0;i<4;++i) {
        String prefix="mod"+String(i);handles[size_t(i)]=std::make_unique<UFOModHandle>(i);addAndMakeVisible(*handles[size_t(i)]);
        editButtons[size_t(i)].setButtonText("EDIT");editButtons[size_t(i)].setComponentID("modEdit"+String(i));handles[size_t(i)]->setComponentID("modHandle"+String(i));editButtons[size_t(i)].onClick=[this,i]{openModEditor(i);};addAndMakeVisible(editButtons[size_t(i)]);
        menu(prefix+"Type","GENERATOR",{"LFO","Random","Euclidean","Envelope","Sequence"});menu(prefix+"Target","DESTINATION",{"Upper color","Lower color","Upper cutoff","Lower cutoff","Upper pitch","Lower pitch","Layer mix","Output"});toggle(prefix+"Sync","Beat sync");
        dial(prefix+"Rate","RATE",routeColours[size_t(i)]);dial(prefix+"Depth","DEPTH",routeColours[size_t(i)]);dial(prefix+"Shape","SHAPE / HITS",routeColours[size_t(i)]);
    }
    for(auto pair:{std::pair{"mix","LAYER MIX"},{"attack","AMP A"},{"decay","AMP D"},{"sustain","AMP S"},{"release","AMP R"},{"drive","DRIVE"},{"chorus","CHORUS"},{"flanger","FLANGER"},{"delay","DELAY"},{"delayTime","TIME / SEC"},{"feedback","FEEDBACK"},{"reverb","SPACE"},{"gain","OUTPUT"}})dial(pair.first,pair.second,mint,String(pair.first)=="mix"?6:String(pair.first)=="gain"?7:-1);
    addAndMakeVisible(keyboard);keyboard.setAvailableRange(24,96);keyboard.setLowestVisibleKey(36);keyboard.setKeyWidth(19);
    keyboard.setColour(MidiKeyboardComponent::whiteNoteColourId,Colour(0xffd9e2d8));keyboard.setColour(MidiKeyboardComponent::blackNoteColourId,ink);keyboard.setColour(MidiKeyboardComponent::keyDownOverlayColourId,mint.withAlpha(.6f));
    setResizable(true,true);setResizeLimits(900,705,1800,1410);getConstrainer()->setFixedAspectRatio(1200.0/940.0);setSize(1080,846);timerCallback();startTimerHz(24);
}
UFOEditor::~UFOEditor(){stopTimer();chooser.reset();modEditor.reset();for(auto& display:waveformDisplays)display.reset();setLookAndFeel(nullptr);}
void UFOEditor::refreshPresets() {
    int selected=presets.getSelectedId();presets.clear(dontSendNotification);presets.addSectionHeading("FACTORY");presets.addItemList({"Init Signal","Orbital Pad","Lunar Pluck","Deep Telemetry"},1);
    userPresets.clear();auto directory=processor.presetDirectory();if(directory.isDirectory()){auto files=directory.findChildFiles(File::findFiles,false,"*.ufo");for(const auto& file:files)userPresets.push_back(file);std::sort(userPresets.begin(),userPresets.end(),[](const File& a,const File& b){return a.getFileName().compareIgnoreCase(b.getFileName())<0;});}
    if(!userPresets.empty()){presets.addSeparator();presets.addSectionHeading("USER PATCHES");for(size_t i=0;i<userPresets.size();++i)presets.addItem(userPresets[i].getFileNameWithoutExtension(),100+int(i));}
    presets.setTextWhenNothingSelected("CURRENT PATCH / PRESETS");presets.setSelectedId(selected,dontSendNotification);
}
void UFOEditor::showError(const String& error){if(error.isNotEmpty())AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon,"UFO",error);}
void UFOEditor::choosePreset(bool save) {
    auto directory=processor.presetDirectory();chooser=std::make_unique<FileChooser>(save?"Save UFO preset":"Load UFO preset",save?directory.getChildFile("Untitled.ufo"):directory,"*.ufo");
    Component::SafePointer<UFOEditor> safe(this);
    int chooserFlags=(save?FileBrowserComponent::saveMode:FileBrowserComponent::openMode)|FileBrowserComponent::canSelectFiles;
    if(save)chooserFlags|=FileBrowserComponent::warnAboutOverwriting;
    chooser->launchAsync(chooserFlags,[safe,save](const FileChooser& dialog){if(!safe)return;auto file=dialog.getResult();if(file!=File{})safe->runPresetJob(save,save?file.withFileExtension(".ufo"):file);});
}
void UFOEditor::runPresetJob(bool save,const File& file) {
    if(processor.presetJobBusy())return;
    Component::SafePointer<UFOEditor> safe(this);
    auto finished=[safe](const String& error){if(safe){safe->updatePresetControls();safe->showError(error);}};
    const bool started=save?processor.saveUserPresetAsync(file,finished):processor.loadUserPresetAsync(file,finished);
    if(started){presetJobLabel=save?"Saving preset...":"Loading preset...";updatePresetControls();}
}
void UFOEditor::updatePresetControls() {
    const bool busy=processor.presetJobBusy();if(busy==presetBusy)return;presetBusy=busy;
    presets.setEnabled(!busy);savePreset.setEnabled(!busy);loadPreset.setEnabled(!busy);
    for(auto& button:importButtons)button.setEnabled(!busy);
    for(auto& button:clearButtons)button.setEnabled(!busy);
    presets.setSelectedId(0,dontSendNotification);
    if(busy)presets.setTextWhenNothingSelected(presetJobLabel);
    else refreshPresets();
}
void UFOEditor::chooseSample(int layer) {
    chooser=std::make_unique<FileChooser>("Import layer audio",File{},"*.wav;*.aif;*.aiff;*.flac");Component::SafePointer<UFOEditor> safe(this);
    chooser->launchAsync(FileBrowserComponent::openMode|FileBrowserComponent::canSelectFiles,[safe,layer](const FileChooser& dialog){if(!safe)return;auto file=dialog.getResult();if(file!=File{})safe->processor.importSample(layer,file);});
}
void UFOEditor::assignMod(int slot,int destination) {
    if(slot<0 || slot>3 || destination<0 || destination>7)return;
    auto* parameter=processor.state.getParameter("mod"+String(slot)+"Target");parameter->beginChangeGesture();parameter->setValueNotifyingHost(parameter->convertTo0to1(float(destination)));parameter->endChangeGesture();
}
void UFOEditor::openModEditor(int slot) {
    Component::SafePointer<UFOEditor> safe(this);modEditor=std::make_unique<ModEditor>(processor,slot,[safe]{if(safe){safe->modEditor->setVisible(false);for(auto& display:safe->waveformDisplays)display->setVisible(true);}});
    for(auto& display:waveformDisplays)display->setVisible(false);
    addAndMakeVisible(*modEditor);resized();modEditor->toFront(true);
}
void UFOEditor::timerCallback() {
    updatePresetControls();
    for(auto& dial:dials)for(int slot=0;slot<4;++slot){String prefix="mod"+String(slot);bool active=dial->slider.destination>=0 && int(processor.state.getRawParameterValue(prefix+"Target")->load())==dial->slider.destination;dial->slider.getProperties().set("modActive"+String(slot),active);dial->slider.getProperties().set("modLevel"+String(slot),processor.modMeters[size_t(slot)].load()*processor.state.getRawParameterValue(prefix+"Depth")->load());dial->slider.repaint();}
    if(ticks++%6==0)for(int layer=0;layer<2;++layer) {
        sampleNames[size_t(layer)]=processor.sampleName(layer);
        const bool loading=processor.sampleLoading(layer);
        if(loading)sampleNames[size_t(layer)]="Loading audio...";
        else if(processor.sampleStreaming(layer)) {
            const auto underruns=processor.sampleUnderruns(layer);
            const auto ioErrors=processor.sampleIoErrors(layer);
            sampleNames[size_t(layer)]="STREAM / "+(ioErrors>0 ? String(juce::uint64(ioErrors))+" read errors / " : String{})+(underruns>0 ? String(juce::uint64(underruns))+" disk underruns / " : String{})+sampleNames[size_t(layer)];
            importButtons[size_t(layer)].setTooltip("Streamed source / "+String(double(processor.sampleCacheBytes(layer))/(1024*1024),1)+" MB buffered. "+(ioErrors>0 ? "Cache pages could not be read; check storage and reimport the source." : "If disk underruns increase, reduce rapid Color movement or use faster storage."));
        } else importButtons[size_t(layer)].setTooltip("Import mono or stereo source audio.");
        importButtons[size_t(layer)].setButtonText(loading?"LOADING...":"IMPORT AUDIO");sampleErrors[size_t(layer)]=processor.sampleError(layer);waveforms[size_t(layer)]=processor.sampleWaveform(layer,256);
    }
    float modulation=0;for(size_t slot=0;slot<4;++slot)modulation=std::max(modulation,std::abs(processor.modMeters[slot].load()));
    bool allActive=true,anyActive=false;String tooltip;
    for(size_t layer=0;layer<2;++layer) {
        waveformDisplays[layer]->setData(waveforms[layer],processor.colorMeters[layer].load(),processor.samplePositions[layer].load(),modulation);
        const auto diagnostic=waveformDisplays[layer]->diagnostics();allActive=allActive&&diagnostic.active;anyActive=anyActive||diagnostic.active;
        tooltip+=String(layer==0?"Upper: ":"Lower: ")+(diagnostic.active?"OpenGL / "+diagnostic.renderer:diagnostic.error.isNotEmpty()?diagnostic.error:"CPU / waiting for graphics context")+"\n";
    }
    renderMode.setButtonText(allActive?"GPU":anyActive?"MIX":"CPU");renderMode.setTooltip(tooltip+"Click to switch GPU/CPU waveform rendering.");
    repaint();
}
void UFOEditor::paint(Graphics& g) {
    g.fillAll(ink);g.addTransform(AffineTransform::scale(float(getWidth())/1200.f,float(getHeight())/940.f));g.setColour(mint);g.setFont(FontOptions(42).withStyle("Bold"));g.drawText("UFO",24,12,130,55,Justification::centredLeft);
    g.setColour(muted);g.setFont(FontOptions(12));g.drawText("MP AUDIO / WAVESCANNING INSTRUMENT",155,30,310,22,Justification::centredLeft);
    for(int layer=0;layer<2;++layer){int y=86+layer*220;auto color=layer==0?mint:violet;g.setColour(panel);g.fillRoundedRectangle(16,float(y),1168,208,10);g.setColour(color);g.setFont(FontOptions(13).withStyle("Bold"));g.drawText(layer==0?"01 / UPPER ENGINE":"02 / LOWER ENGINE",30,y+8,180,22,Justification::centredLeft);
        g.setFont(FontOptions(11));g.setColour(sampleErrors[size_t(layer)].isEmpty()?muted:Colour(0xffffbd91));String status=sampleErrors[size_t(layer)].isNotEmpty()?sampleErrors[size_t(layer)]:sampleNames[size_t(layer)];if(status.isEmpty())status="Procedural bank / import WAV, AIFF or FLAC";g.drawText(status,218,y+8,480,22,Justification::centredLeft,true);

    }
    g.setColour(muted);g.setFont(FontOptions(12));g.drawText("MOD POD / DRAG A HANDLE ONTO A DESTINATION KNOB",24,526,900,23,Justification::centredLeft);
    for(int i=0;i<4;++i){g.setColour(panel);g.fillRoundedRectangle(float(16+i*294),552,286,175,8);}
    g.setColour(muted);g.drawText("AMPLITUDE / EFFECTS",24,735,500,20,Justification::centredLeft);
    g.setColour(Colour(0xff36423a));g.fillRoundedRectangle(1070,38,106,5,2);g.setColour(mint);g.fillRoundedRectangle(1070,38,106*std::min(1.f,processor.meter.load()),5,2);
    g.setColour(muted);g.setFont(FontOptions(10));g.drawText("14 VOICES",1060,52,116,18,Justification::centredRight);
}
void UFOEditor::resized() {
    presets.setBounds(484,24,270,30);savePreset.setBounds(766,24,70,30);loadPreset.setBounds(844,24,70,30);panic.setBounds(926,24,80,30);renderMode.setBounds(1016,24,46,30);
    for(int layer=0;layer<2;++layer){int y=86+layer*220;waveformDisplays[size_t(layer)]->setBounds(910,y+16,258,64);for(int m=0;m<4;++m){auto& item=*menus[size_t(layer*4+m)];int x=30+m*170;item.label.setBounds(x,y+36,160,16);item.box.setBounds(x,y+54,160,26);}toggles[size_t(layer)]->button.setBounds(720,y+52,120,28);importButtons[size_t(layer)].setBounds(716,y+8,116,23);clearButtons[size_t(layer)].setBounds(840,y+8,60,23);
        for(int d=0;d<10;++d){auto& item=*dials[size_t(layer*10+d)];int x=30+d*114;item.label.setBounds(x,y+94,108,18);item.slider.setBounds(x,y+112,108,90);}}
    for(int i=0;i<4;++i){int x=26+i*294;handles[size_t(i)]->setBounds(x,558,160,22);editButtons[size_t(i)].setBounds(x+202,558,68,22);for(int m=0;m<2;++m){auto& item=*menus[size_t(8+i*2+m)];item.label.setBounds(x+m*137,583,133,14);item.box.setBounds(x+m*137,599,133,24);}for(int d=0;d<3;++d){auto& item=*dials[size_t(20+i*3+d)];item.label.setBounds(x+d*90,628,88,14);item.slider.setBounds(x+d*90,643,88,65);}toggles[size_t(2+i)]->button.setBounds(x+72,706,140,20);}
    for(int d=0;d<13;++d){auto& item=*dials[size_t(32+d)];int x=20+d*90;item.label.setBounds(x,760,88,18);item.slider.setBounds(x,779,88,79);}keyboard.setBounds(20,872,1160,56);
    if(modEditor)modEditor->setBounds(0,0,1200,940);
    for(auto* child:getChildren())if(dynamic_cast<ResizableCornerComponent*>(child)==nullptr && dynamic_cast<ResizableBorderComponent*>(child)==nullptr)child->setTransform(AffineTransform::scale(float(getWidth())/1200.f,float(getHeight())/940.f));
}
