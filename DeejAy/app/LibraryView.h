// Library browser: sources (All, Prepare, History, Crates, Smart Crates) on the left, search + track table
// on the right. Rows drag onto decks and sampler slots as "track:<id>".
#pragma once
#include "AppModel.h"
#include "Widgets.h"

namespace app
{

class LibraryView : public juce::Component,
                    private juce::TableListBoxModel,
                    public juce::FileDragAndDropTarget
{
public:
    explicit LibraryView (AppModel& model);
    ~LibraryView() override;

    void rebuild();                         // library contents changed
    void browse (int delta, bool shift);    // FLX10 rotary
    void back();                            // FLX10 BACK: toggle focus sources <-> tracks
    void focusSearch() { search_.grabKeyboardFocus(); }
    void resized() override;
    void paint (juce::Graphics& g) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override { model_.importPaths (files); }

private:
    struct Source
    {
        enum class Kind { All, Prepare, History, Crate, Smart };
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
    int currentSource_ = 0;
    int sortColumn_ = 0;
    bool sortForwards_ = true;
    bool focusTracks_ = true;
    std::unique_ptr<juce::FileChooser> chooser_;
};

} // namespace app
