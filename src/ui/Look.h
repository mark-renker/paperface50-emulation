// Paperface 50 — shared visual language (colors, fonts, primitive painters)
// modeled on the manual's panel drawings (cream faces, black frames, rosette
// knobs, color-coded banana jacks, orange LED rings).
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/EngineCore.h"

namespace pf50ui {

constexpr float PX = 52.0f;              // pixels per panel inch
constexpr float MOD_H = 7.0f;            // module height, inches

namespace col {
    const juce::Colour cream       (0xffefeae0);
    const juce::Colour creamShade  (0xffe4ddcf);
    const juce::Colour ink         (0xff211e19);
    const juce::Colour frame       (0xff3a362f);
    const juce::Colour boxLine     (0xff6b665c);
    const juce::Colour wood        (0xff6d4a2f);
    const juce::Colour woodDark    (0xff4e3421);
    const juce::Colour headerBg    (0xff17161a);
    const juce::Colour ioBg        (0xff26231f);
    const juce::Colour ioInk       (0xffd8d2c4);
    const juce::Colour jackWhite   (0xfff6f3ea);
    const juce::Colour jackRed     (0xffbf3a31);
    const juce::Colour jackBlack   (0xff23211e);
    const juce::Colour jackPurple  (0xff7e5ea8);
    const juce::Colour jackGrey    (0xff8f8b82);
    const juce::Colour jackHole    (0xff4a463f);
    const juce::Colour led         (0xfff2a63b);
    const juce::Colour ledOff      (0xff8a7145);
    const juce::Colour knob        (0xff191713);
    const juce::Colour knobHi      (0xff35322c);
    const juce::Colour cable       (0xff3d63c2);
}

inline juce::Font panelFont (float h, bool bold = false)
{
    auto f = juce::Font (juce::FontOptions ("Futura", h, bold ? juce::Font::bold : juce::Font::plain));
    return f;
}

inline juce::Colour jackColour (pf50::JackColor c)
{
    using JC = pf50::JackColor;
    switch (c)
    {
        case JC::White:  return col::jackWhite;
        case JC::Red:    return col::jackRed;
        case JC::Black:  return col::jackBlack;
        case JC::Purple: return col::jackPurple;
        case JC::Grey:   return col::jackGrey;
        case JC::TRS:    return col::jackBlack;
        default:         return col::jackWhite;
    }
}

// 8-lobed black rosette knob with pointer, like the drawings
inline void drawKnob (juce::Graphics& g, juce::Rectangle<float> r, float value01, bool /*big*/)
{
    auto c = r.getCentre();
    float R = r.getWidth() * 0.5f;
    juce::Path p;
    const int lobes = 8;
    for (int i = 0; i <= 96; ++i)
    {
        float a = (float) i / 96.0f * juce::MathConstants<float>::twoPi;
        float rad = R * (0.86f + 0.14f * std::cos (a * lobes));
        auto pt = c.getPointOnCircumference (rad, a);
        if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
    }
    p.closeSubPath();
    g.setColour (col::knob);
    g.fillPath (p);
    g.setColour (col::knobHi);
    g.strokePath (p, juce::PathStrokeType (1.0f));
    // skirt circle
    g.setColour (col::knobHi.withAlpha (0.55f));
    g.drawEllipse (juce::Rectangle<float> (R * 1.04f, R * 1.04f).withCentre (c), 0.8f);
    // pointer: 240° sweep, 210°..510°  (7 o'clock → 5 o'clock)
    float ang = juce::degreesToRadians (-135.0f + 270.0f * value01);
    auto tip  = c.getPointOnCircumference (R * 0.82f, ang);
    auto tail = c.getPointOnCircumference (R * 0.18f, ang);
    g.setColour (juce::Colours::white.withAlpha (0.92f));
    g.drawLine ({ tail, tip }, juce::jmax (1.6f, R * 0.13f));
}

inline void drawJack (juce::Graphics& g, juce::Rectangle<float> r, pf50::JackColor jc, bool highlight)
{
    auto c = r.getCentre();
    float R = r.getWidth() * 0.5f;
    auto body = jackColour (jc);
    g.setColour (body);
    g.fillEllipse (r);
    g.setColour (col::ink.withAlpha (0.85f));
    g.drawEllipse (r, juce::jmax (1.0f, R * 0.09f));
    g.setColour (col::jackHole);
    g.fillEllipse (juce::Rectangle<float> (R * 0.78f, R * 0.78f).withCentre (c));
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillEllipse (juce::Rectangle<float> (R * 0.4f, R * 0.4f).withCentre (c));
    if (highlight)
    {
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawEllipse (r.expanded (2.5f), 2.0f);
    }
}

inline void drawLed (juce::Graphics& g, juce::Rectangle<float> r, float bright)
{
    // the manual's amber concentric-ring "target" indicator
    auto c = r.getCentre();
    float R = r.getWidth() * 0.5f;
    if (bright > 0.02f)
    {
        g.setColour (col::led.withAlpha (0.4f * bright));
        g.fillEllipse (juce::Rectangle<float> (R * 2.6f, R * 2.6f).withCentre (c));
    }
    auto ring = col::led.withAlpha (0.55f + 0.45f * bright);
    for (float f : { 1.0f, 0.68f, 0.38f })
    {
        g.setColour (ring);
        g.drawEllipse (juce::Rectangle<float> (2 * R * f, 2 * R * f).withCentre (c), juce::jmax (1.1f, R * 0.17f));
    }
    g.setColour (bright > 0.35f ? col::led.brighter (0.3f) : col::led.withAlpha (0.8f));
    g.fillEllipse (juce::Rectangle<float> (R * 0.42f, R * 0.42f).withCentre (c));
}

inline void drawPushButton (juce::Graphics& g, juce::Rectangle<float> r, bool down)
{
    auto c = r.getCentre();
    float R = r.getWidth() * 0.5f;
    g.setColour (juce::Colour (0xffb9b4a9));
    g.fillEllipse (r);
    g.setColour (col::ink.withAlpha (0.7f));
    g.drawEllipse (r, 1.2f);
    auto btn = juce::Rectangle<float> (R * 1.34f, R * 1.34f).withCentre (c);
    g.setColour (down ? juce::Colour (0xff8f2721) : col::jackRed);
    g.fillEllipse (btn);
    g.setColour (juce::Colours::white.withAlpha (down ? 0.12f : 0.28f));
    g.fillEllipse (btn.reduced (R * 0.28f).translated (-R * 0.1f, -R * 0.12f));
}

inline void drawToggle (juce::Graphics& g, juce::Rectangle<float> r, int pos, int nPos)
{
    // bat-lever toggle: small hex base, long lever with ball end
    auto c0 = r.getCentre();
    auto base = juce::Rectangle<float> (r.getWidth() * 0.52f, r.getHeight() * 0.3f).withCentre (c0);
    g.setColour (juce::Colour (0xff9b968c));
    g.fillEllipse (base);
    g.setColour (col::ink.withAlpha (0.9f));
    g.drawEllipse (base, 1.1f);
    float t = nPos <= 1 ? 0.5f : (float) pos / (float) (nPos - 1);
    float y = juce::jmap (t, r.getY() + r.getHeight() * 0.06f, r.getBottom() - r.getHeight() * 0.06f);
    auto tip = juce::Point<float> (r.getCentreX(), y);
    g.setColour (juce::Colour (0xff474440));
    g.drawLine (c0.x, c0.y, tip.x, tip.y, r.getWidth() * 0.22f);
    g.setColour (juce::Colour (0xffe2dfd6));
    g.fillEllipse (juce::Rectangle<float> (r.getWidth() * 0.5f, r.getWidth() * 0.5f).withCentre (tip));
    g.setColour (col::ink.withAlpha (0.85f));
    g.drawEllipse (juce::Rectangle<float> (r.getWidth() * 0.5f, r.getWidth() * 0.5f).withCentre (tip), 1.0f);
}

// small waveform glyphs used beside jacks in the drawings
enum class Glyph { Saw, Sine, PulseUp, PulseDown, RampUp, RampDown, ArcAR };
inline void drawGlyph (juce::Graphics& g, juce::Rectangle<float> r, Glyph gl)
{
    juce::Path p;
    auto b = r.reduced (1.0f);
    switch (gl)
    {
        case Glyph::Saw:
            p.startNewSubPath (b.getX(), b.getBottom());
            p.lineTo (b.getX() + b.getWidth() * 0.5f, b.getY());
            p.lineTo (b.getX() + b.getWidth() * 0.5f, b.getBottom());
            p.lineTo (b.getRight(), b.getY());
            break;
        case Glyph::Sine:
        {
            for (int i = 0; i <= 24; ++i)
            {
                float x = b.getX() + b.getWidth() * i / 24.0f;
                float y = b.getCentreY() - std::sin (i / 24.0f * juce::MathConstants<float>::twoPi) * b.getHeight() * 0.42f;
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            break;
        }
        case Glyph::PulseUp:
            p.startNewSubPath (b.getX(), b.getBottom());
            p.lineTo (b.getX() + b.getWidth() * 0.25f, b.getBottom());
            p.lineTo (b.getX() + b.getWidth() * 0.25f, b.getY());
            p.lineTo (b.getX() + b.getWidth() * 0.75f, b.getY());
            p.lineTo (b.getX() + b.getWidth() * 0.75f, b.getBottom());
            p.lineTo (b.getRight(), b.getBottom());
            break;
        case Glyph::PulseDown:
            p.startNewSubPath (b.getX(), b.getY());
            p.lineTo (b.getX() + b.getWidth() * 0.25f, b.getY());
            p.lineTo (b.getX() + b.getWidth() * 0.25f, b.getBottom());
            p.lineTo (b.getX() + b.getWidth() * 0.75f, b.getBottom());
            p.lineTo (b.getX() + b.getWidth() * 0.75f, b.getY());
            p.lineTo (b.getRight(), b.getY());
            break;
        case Glyph::RampUp:
            p.startNewSubPath (b.getX(), b.getBottom());
            p.lineTo (b.getRight() - b.getWidth() * 0.3f, b.getY());
            p.lineTo (b.getRight(), b.getY());
            break;
        case Glyph::RampDown:
            p.startNewSubPath (b.getX(), b.getY());
            p.lineTo (b.getRight() - b.getWidth() * 0.3f, b.getBottom());
            p.lineTo (b.getRight(), b.getBottom());
            break;
        case Glyph::ArcAR:
            p.startNewSubPath (b.getX(), b.getBottom());
            p.quadraticTo (b.getCentreX(), b.getY() - b.getHeight() * 0.2f, b.getRight(), b.getBottom());
            break;
    }
    g.setColour (col::ink);
    g.strokePath (p, juce::PathStrokeType (1.2f));
}

} // namespace pf50ui
