// Sampler panel: banks A-D, 8 slots with Trigger / Hold / On-Off, sync, repeat, volume. Drop library tracks,
// Splice samples (dragged from the library's Splice list) or audio files from the Finder on a slot.
#pragma once
#include "AppModel.h"
#include "Widgets.h"

namespace app
{

class SamplerSlotView : public juce::Component, public juce::DragAndDropTarget, public juce::FileDragAndDropTarget
{
public:
    SamplerSlotView (AppModel& model, int index);
    void setBank (int bank) { bank_ = bank; }
    void refresh();
    void resized() override;
    void paint (juce::Graphics& g) override;
    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        const auto s = d.description.toString();
        return s.startsWith ("track:") || s.startsWith ("sample:");
    }
    void itemDropped (const SourceDetails& d) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override;
private:
    int slot() const { return bank_ * dj::kSamplerSlotsPerBank + index_; }
    AppModel& model_;
    int index_, bank_ = 0;
    PadButton play_;
    juce::TextButton mode_, sync_ { "SYNC" }, repeat_ { "RPT" };
    juce::Slider volume_;
};

class SamplerView : public juce::Component
{
public:
    explicit SamplerView (AppModel& model);
    void refresh();
    void resized() override;
    void paint (juce::Graphics& g) override;
private:
    AppModel& model_;
    std::array<juce::TextButton, dj::kSamplerBanks> banks_;
    std::array<std::unique_ptr<SamplerSlotView>, dj::kSamplerSlotsPerBank> slots_;
};

} // namespace app
