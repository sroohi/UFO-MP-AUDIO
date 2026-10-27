// DEEJ-AY look: dark performance UI, one colour per deck, Camelot-coloured keys, RGB frequency waveforms.
#pragma once
#include <JuceHeader.h>
#include "dj/Common.h"

namespace app::Theme
{
inline const juce::Colour bg       { 0xff0d0e11 };
inline const juce::Colour panel    { 0xff16181d };
inline const juce::Colour panelHi  { 0xff20232a };
inline const juce::Colour line     { 0xff2b2f37 };
inline const juce::Colour text     { 0xffe7e9ee };
inline const juce::Colour dim      { 0xff8b919d };
inline const juce::Colour accent   { 0xff19d3ff };   // MP Audio cyan
inline const juce::Colour hot      { 0xffff6a1a };   // MP Audio orange
inline const juce::Colour good     { 0xff52e07a };
inline const juce::Colour warn     { 0xffffc23a };
inline const juce::Colour bad      { 0xffff3b47 };

inline juce::Colour deck (int i)
{
    static const juce::Colour c[4] { juce::Colour (0xff3fa9f5), juce::Colour (0xffff8a1f), juce::Colour (0xff8ce04a), juce::Colour (0xffe64acb) };
    return c[(std::size_t) juce::jlimit (0, 3, i)];
}

// Default hot cue colours (pads 1-8); a stored track colour (0xRRGGBB) wins when set.
inline juce::Colour hotCue (int i, std::uint32_t stored = 0)
{
    if (stored != 0) return juce::Colour ((juce::uint8) (stored >> 16), (juce::uint8) (stored >> 8), (juce::uint8) stored);
    static const juce::uint32 c[8] { 0xffe7322b, 0xfff28c28, 0xfff2d21b, 0xff4fd34a, 0xff1fd1d6, 0xff2f6df6, 0xff9b4df2, 0xfff24fa6 };
    return juce::Colour (c[(std::size_t) juce::jlimit (0, 7, i)]);
}

// Camelot wheel colours (1..12), the usual harmonic-mixing palette.
inline juce::Colour key (int keyIndex)
{
    if (keyIndex < 0) return dim;
    const int n = dj::camelotNumber (keyIndex);
    return juce::Colour::fromHSV ((float) ((n - 1) % 12) / 12.0f, 0.65f, dj::camelotIsMinor (keyIndex) ? 0.85f : 1.0f, 1.0f);
}

inline juce::String keyText (int keyIndex, bool camelot)
{
    if (keyIndex < 0) return "-";
    if (camelot) return juce::String (dj::camelotNumber (keyIndex)) + (dj::camelotIsMinor (keyIndex) ? "A" : "B");
    return dj::keyName (keyIndex);
}

// Frequency colour of a waveform bin: lows red, mids green, highs blue (brightness from the peak).
inline juce::Colour waveform (std::uint8_t low, std::uint8_t mid, std::uint8_t high)
{
    const float l = low / 255.0f, m = mid / 255.0f, h = high / 255.0f;
    const float mx = juce::jmax (0.001f, l, m, h);
    return juce::Colour::fromFloatRGBA (juce::jlimit (0.0f, 1.0f, l / mx), juce::jlimit (0.0f, 1.0f, m / mx * 0.9f), juce::jlimit (0.0f, 1.0f, h / mx), 1.0f);
}

inline juce::String formatTime (double seconds)
{
    if (seconds < 0) seconds = 0;
    const int m = (int) (seconds / 60.0);
    const double s = seconds - m * 60.0;
    return juce::String (m).paddedLeft ('0', 2) + ":" + juce::String (s, 1).paddedLeft ('0', 4);
}
} // namespace app::Theme
