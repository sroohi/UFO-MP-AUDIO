// Small reusable controls.
#pragma once
#include "Theme.h"
#include <functional>

namespace app
{

// Momentary-aware button: onPress / onRelease fire on mouse down / up (pads, cue, censor...).
class PadButton : public juce::Component
{
public:
    explicit PadButton (juce::String label = {}) : label_ (std::move (label)) {}
    std::function<void (bool shift)> onPress;
    std::function<void()> onRelease;
    void setLabel (const juce::String& l) { if (l != label_) { label_ = l; repaint(); } }
    void setColour (juce::Colour c) { if (c != colour_) { colour_ = c; repaint(); } }
    void setLit (bool lit) { if (lit != lit_) { lit_ = lit; repaint(); } }
    bool lit() const { return lit_; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.5f);
        const auto base = lit_ ? colour_ : Theme::panelHi;
        g.setColour (down_ ? base.brighter (0.4f) : base.withAlpha (lit_ ? 0.9f : 1.0f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (lit_ ? colour_.brighter (0.5f) : Theme::line);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
        g.setColour (lit_ ? (colour_.getPerceivedBrightness() > 0.6f ? Theme::bg : Theme::text) : Theme::dim);
        g.setFont (juce::FontOptions (juce::jmin (13.0f, r.getHeight() * 0.42f), juce::Font::bold));
        g.drawFittedText (label_, getLocalBounds().reduced (3), juce::Justification::centred, 2);
    }
    void mouseDown (const juce::MouseEvent& e) override { down_ = true; repaint(); if (onPress) onPress (e.mods.isShiftDown() || e.mods.isPopupMenu()); }
    void mouseUp (const juce::MouseEvent&) override { down_ = false; repaint(); if (onRelease) onRelease(); }

private:
    juce::String label_;
    juce::Colour colour_ { Theme::accent };
    bool lit_ = false, down_ = false;
};

// Rotary knob with a bipolar (centre-detent) or unipolar arc.
class Knob : public juce::Slider
{
public:
    explicit Knob (bool bipolar = false, juce::String name = {}) : bipolar_ (bipolar), name_ (std::move (name))
    {
        setSliderStyle (juce::Slider::RotaryVerticalDrag);
        setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        setRange (0.0, 1.0);
        setValue (bipolar ? 0.5 : 0.0, juce::dontSendNotification);
        setDoubleClickReturnValue (true, bipolar ? 0.5 : 0.0);
        setMouseDragSensitivity (180);
    }
    void setArcColour (juce::Colour c) { arc_ = c; repaint(); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float labelH = name_.isEmpty() ? 0.0f : 11.0f;
        auto k = r.withTrimmedBottom (labelH).reduced (2.0f);
        const float size = juce::jmin (k.getWidth(), k.getHeight());
        k = k.withSizeKeepingCentre (size, size);
        const float start = juce::MathConstants<float>::pi * 1.25f, end = juce::MathConstants<float>::pi * 2.75f;
        const float v = (float) valueToProportionOfLength (getValue());
        const float ang = start + v * (end - start);
        juce::Path track, fill;
        track.addCentredArc (k.getCentreX(), k.getCentreY(), size * 0.42f, size * 0.42f, 0, start, end, true);
        g.setColour (Theme::line);
        g.strokePath (track, juce::PathStrokeType (3.0f));
        const float from = bipolar_ ? (start + end) * 0.5f : start;
        fill.addCentredArc (k.getCentreX(), k.getCentreY(), size * 0.42f, size * 0.42f, 0, juce::jmin (from, ang), juce::jmax (from, ang), true);
        g.setColour (arc_);
        g.strokePath (fill, juce::PathStrokeType (3.0f));
        g.setColour (Theme::panelHi);
        g.fillEllipse (k.reduced (size * 0.18f));
        juce::Line<float> pointer (k.getCentre(), k.getCentre().getPointOnCircumference (size * 0.3f, ang));
        g.setColour (Theme::text);
        g.drawLine (pointer, 2.0f);
        if (labelH > 0)
        {
            g.setColour (Theme::dim);
            g.setFont (juce::FontOptions (9.5f));
            g.drawText (name_, r.removeFromBottom (labelH), juce::Justification::centred);
        }
    }
private:
    bool bipolar_;
    juce::String name_;
    juce::Colour arc_ { Theme::accent };
};

class Meter : public juce::Component
{
public:
    void setPeak (float p) { const float v = juce::jmax (p, level_ * 0.86f); if (std::abs (v - level_) > 1e-4f) { level_ = v; repaint(); } }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Theme::panelHi);
        g.fillRect (r);
        const float db = juce::Decibels::gainToDecibels (level_, -48.0f);
        const float f = juce::jlimit (0.0f, 1.0f, (db + 48.0f) / 48.0f);
        juce::ColourGradient grad (Theme::good, 0, r.getBottom(), Theme::bad, 0, r.getY(), false);
        grad.addColour (0.75, Theme::warn);
        g.setGradientFill (grad);
        g.fillRect (r.withTop (r.getBottom() - r.getHeight() * f));
    }
private:
    float level_ = 0.0f;
};

inline void styleToggle (juce::TextButton& b, juce::Colour on)
{
    b.setClickingTogglesState (false);
    b.setColour (juce::TextButton::buttonColourId, Theme::panelHi);
    b.setColour (juce::TextButton::buttonOnColourId, on);
    b.setColour (juce::TextButton::textColourOffId, Theme::dim);
    b.setColour (juce::TextButton::textColourOnId, on.getPerceivedBrightness() > 0.6f ? Theme::bg : Theme::text);
}

} // namespace app
