// Library browser: sources (All, Prepare, Splice, Crates, Smart Crates, History) on the left, search + track
// table on the right. Rows drag onto decks and sampler slots as "track:<id>"; Splice samples drag as
// "sample:\n<path>\n<bpm>\n<loop 0/1>\n<name>".
#pragma once
#include "AppModel.h"
#include "Widgets.h"

namespace app
{

class LibraryView : public juce::Component,
                    private juce::TableListBoxModel,
                    public juce::FileDragAndDropTarget,
                    private juce::Timer
{
public:
    explicit LibraryView (AppModel& model);
    ~LibraryView() override;

    void rebuild();                         // library contents changed
    void browse (int delta, bool shift);    // FLX10 rotary
    void back();                            // FLX10 BACK: toggle focus sources <-> tracks
    void focusSearch() { search_.grabKeyboardFocus(); }
    void showSplice();                      // select the Splice source
    void tracksUpdated() { table_.repaint(); }   // analysis results: repaint without re-sorting
    void resized() override;
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override { model_.importPaths (files); }

private:
    struct Source
    {
        enum class Kind { All, Prepare, Splice, History, Crate, Smart };
        Source (Kind k, juce::String l) : kind (k), label (std::move (l)) {}
        Kind kind = Kind::All;
        juce::String label;
        std::string name;      // crate name
        int index = 0;         // history session / smart crate index
        int depth = 0;
    };

    // TableListBoxModel
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int w, int h, bool selected) override;
    void sortOrderChanged (int columnId, bool forwards) override;
    void selectedRowsChanged (int lastRow) override;
    void cellDoubleClicked (int row, int column, const juce::MouseEvent&) override;
    void cellClicked (int row, int column, const juce::MouseEvent&) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override;

    // sources list (through SourcesModel)
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected);
    void listBoxItemClicked (int row, const juce::MouseEvent& e);

    void rebuildSources();
    void refreshTracks();
    void timerCallback() override;           // rescans the Splice folder while it is shown
    bool splice() const
    {
        return currentSource_ >= 0 && currentSource_ < (int) sources_.size() && sources_[(std::size_t) currentSource_].kind == Source::Kind::Splice;
    }
    const SpliceSample* spliceAt (int row) const;
    void showSpliceMenu (int row);
    int freeSamplerSlot() const;
    void updateToolbar();
    std::vector<std::uint64_t> sourceTracks() const;
    void showTrackMenu (int row);
    void showSourceMenu (int row);
    void newCrate();
    void newSmartCrate();
    int idleDeck() const;

    struct SourcesModel final : juce::ListBoxModel
    {
        explicit SourcesModel (LibraryView& o) : owner (o) {}
        int getNumRows() override { return (int) owner.sources_.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override { owner.paintListBoxItem (row, g, w, h, selected); }
        void listBoxItemClicked (int row, const juce::MouseEvent& e) override { owner.listBoxItemClicked (row, e); }
        LibraryView& owner;
    };

    AppModel& model_;
    std::vector<Source> sources_;
    SourcesModel sourcesModel_ { *this };
    juce::ListBox sourceList_ { "Sources", &sourcesModel_ };
    juce::TableListBox table_ { "Tracks", this };
    juce::TextEditor search_;
    juce::TextButton addFiles_ { "+ Files" }, addFolder_ { "+ Folder" }, seratoImport_ { "Import Serato" }, newCrate_ { "+ Crate" }, newSmart_ { "+ Smart" };
    juce::TextButton analyse_ { "Analyse" }, spliceOpen_ { "Open Splice" }, spliceFolder_ { "Splice Folder..." }, spliceRescan_ { "Rescan" };
    std::vector<int> spliceRows_;             // indices into model_.spliceSamples() after search / sort
    int currentSource_ = 0;
    int sortColumn_ = 0;
    bool sortForwards_ = true;
    bool focusTracks_ = true;
    std::unique_ptr<juce::FileChooser> chooser_;
};

} // namespace app
