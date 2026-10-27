// DEEJ-AY by MP Audio — application entry point.
#include "MainComponent.h"

namespace app
{

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, Theme::bg);
        setColour (juce::TextButton::buttonColourId, Theme::panelHi);
        setColour (juce::TextButton::textColourOffId, Theme::text);
        setColour (juce::ComboBox::backgroundColourId, Theme::panelHi);
        setColour (juce::ComboBox::textColourId, Theme::text);
        setColour (juce::ComboBox::outlineColourId, Theme::line);
        setColour (juce::ComboBox::arrowColourId, Theme::dim);
        setColour (juce::Slider::thumbColourId, Theme::accent);
        setColour (juce::Slider::trackColourId, Theme::panelHi);
        setColour (juce::Slider::backgroundColourId, Theme::panelHi);
        setColour (juce::Label::textColourId, Theme::text);
        setColour (juce::ToggleButton::textColourId, Theme::text);
        setColour (juce::ToggleButton::tickColourId, Theme::accent);
        setColour (juce::PopupMenu::backgroundColourId, Theme::panel);
        setColour (juce::PopupMenu::textColourId, Theme::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::accent.darker (0.4f));
        setColour (juce::TabbedComponent::backgroundColourId, Theme::bg);
        setColour (juce::TabbedButtonBar::tabTextColourId, Theme::dim);
        setColour (juce::TabbedButtonBar::frontTextColourId, Theme::accent);
        setColour (juce::ListBox::backgroundColourId, Theme::bg);
        setColour (juce::ScrollBar::thumbColourId, Theme::line);
        setColour (juce::AlertWindow::backgroundColourId, Theme::panel);
        setColour (juce::AlertWindow::textColourId, Theme::text);
        setColour (juce::TooltipWindow::backgroundColourId, Theme::panelHi);
        setColour (juce::TooltipWindow::textColourId, Theme::text);
    }

    // flat, compact buttons
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = bg;
        if (down) c = c.brighter (0.3f);
        else if (over) c = c.brighter (0.1f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (Theme::line);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
    }
    juce::Font getTextButtonFont (juce::TextButton&, int height) override
    {
        return juce::Font (juce::FontOptions (juce::jmin (12.0f, (float) height * 0.62f), juce::Font::bold));
    }
};

class DeejAyApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "DEEJ-AY"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String&) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel_);
        window_ = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        window_.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name) : DocumentWindow (name, Theme::bg, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (1200, 760, 6000, 4000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
    LookAndFeel lookAndFeel_;
    std::unique_ptr<MainWindow> window_;
};

} // namespace app

START_JUCE_APPLICATION (app::DeejAyApplication)
