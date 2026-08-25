// Paperface 50 — panel widgets (jacks, knobs, toggles, buttons, LEDs) and the
// module face component with its inch-based layout language.
#pragma once
#include "Look.h"
#include <juce_audio_processors/juce_audio_processors.h>

class PF50Processor;

namespace pf50ui {

class JackW;
struct CableHost
{
    virtual ~CableHost() = default;
    virtual void jackDragStart (JackW*) = 0;
    virtual void jackDragMove (juce::Point<float> screen) = 0;
    virtual void jackDragEnd (juce::Point<float> screen) = 0;
    virtual void jackRightClick (JackW*) = 0;
};

class JackW : public juce::Component
{
public:
    JackW (pf50::Module& m, int jackIdx, CableHost*& h) : mod (m), jack (jackIdx), host (h)
    {
        setRepaintsOnMouseActivity (true);
    }
    void paint (juce::Graphics& g) override
    {
        bool hl = isMouseOverOrDragging() || dropTarget;
        drawJack (g, getLocalBounds().toFloat().reduced (2.0f), mod.jacks[(size_t) jack].color, hl);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! host) return;
        if (e.mods.isPopupMenu()) host->jackRightClick (this);
        else host->jackDragStart (this);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (host && ! e.mods.isPopupMenu()) host->jackDragMove (e.getScreenPosition().toFloat()); }
    void mouseUp   (const juce::MouseEvent& e) override { if (host && ! e.mods.isPopupMenu()) host->jackDragEnd (e.getScreenPosition().toFloat()); }

    pf50::Module& mod; int jack; CableHost*& host;
    bool dropTarget = false;
};

class KnobW : public juce::Component
{
public:
    KnobW (juce::RangedAudioParameter& p, bool isBig) : big (isBig),
        att (p, [this] (float v) { val = v; repaint(); })
    {
        att.sendInitialUpdate();
    }
    void paint (juce::Graphics& g) override { drawKnob (g, getLocalBounds().toFloat().reduced (1.0f), val, big); }
    void mouseDown (const juce::MouseEvent&) override { att.beginGesture(); startVal = val; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        float sens = e.mods.isShiftDown() ? 1200.0f : 240.0f;
        att.setValueAsPartOfGesture (juce::jlimit (0.0f, 1.0f, startVal - e.getDistanceFromDragStartY() / sens));
    }
    void mouseUp (const juce::MouseEvent&) override { att.endGesture(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { att.setValueAsCompleteGesture (defaultVal); }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        att.setValueAsCompleteGesture (juce::jlimit (0.0f, 1.0f, val + (float) w.deltaY * 0.08f));
    }
    float defaultVal = 0.5f;
    bool big; float val = 0, startVal = 0;
    juce::ParameterAttachment att;
};

class ToggleW : public juce::Component
{
public:
    ToggleW (juce::RangedAudioParameter& p, int nPositions) : n (nPositions),
        att (p, [this] (float v) { idx = (int) std::lround (v); repaint(); })
    {
        att.sendInitialUpdate();
    }
    void paint (juce::Graphics& g) override { drawToggle (g, getLocalBounds().toFloat(), idx, n); }
    void mouseDown (const juce::MouseEvent&) override
    {
        int next = (idx + 1) % n;
        att.setValueAsCompleteGesture ((float) next);
    }
    int n, idx = 0;
    juce::ParameterAttachment att;
};

class ButtonW : public juce::Component
{
public:
    std::function<void (bool)> onPress; // true = down
    void paint (juce::Graphics& g) override { drawPushButton (g, getLocalBounds().toFloat().reduced (1.0f), down); }
    void mouseDown (const juce::MouseEvent&) override { down = true; repaint(); if (onPress) onPress (true); }
    void mouseUp   (const juce::MouseEvent&) override { down = false; repaint(); if (onPress) onPress (false); }
    bool down = false;
};

class LedW : public juce::Component
{
public:
    LedW (pf50::Module& m, int ledIdx) : mod (m), idx (ledIdx) { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics& g) override { drawLed (g, getLocalBounds().toFloat(), bright); }
    void refresh()
    {
        float b = idx < (int) mod.leds.size() ? mod.leds[(size_t) idx] : 0.0f;
        if (std::fabs (b - bright) > 0.03f) { bright = b; repaint(); }
    }
    pf50::Module& mod; int idx; float bright = 0;
};

// ------------------------------------------------------- module face --------
class ModuleComp : public juce::Component
{
public:
    ModuleComp (pf50::Module& m, float widthInches, CableHost*& hostRef, PF50Processor& p)
        : mod (m), wIn (widthInches), host (hostRef), proc (p)
    {
        setSize ((int) (wIn * PX), (int) (MOD_H * PX));
    }

    struct TextItem { juce::String text; juce::Rectangle<float> r; float h; bool boxed; };
    std::vector<TextItem> labels;
    std::vector<std::pair<juce::Rectangle<float>, Glyph>> glyphs;
    std::vector<juce::Rectangle<float>> boxes;      // section frames
    std::vector<juce::Rectangle<float>> scales;     // "−   +" attenuverter scales
    std::vector<juce::Line<float>> lines;           // connector graphics
    bool darkFace = false;

    static juce::Rectangle<float> sq (float x, float y, float r) { return { x - r, y - r, 2 * r, 2 * r }; }
    juce::Rectangle<int> pxr (juce::Rectangle<float> inches) const
    {
        return (inches * PX).getSmallestIntegerContainer();
    }

    JackW* addJack (int jackIdx, float x, float y, const juce::String& label = {}, bool small = false)
    {
        auto* j = new JackW (mod, jackIdx, host);
        jacks.add (j);
        addAndMakeVisible (j);
        float r = small ? 0.125f : 0.165f;
        j->setBounds (pxr (sq (x, y, r + 0.05f)));
        if (label.isNotEmpty())
            labels.push_back ({ label, { x - 0.55f, y + r + 0.05f, 1.1f, 0.17f }, 0.145f, false });
        return j;
    }
    KnobW* addKnob (int ctlIdx, float x, float y, bool big, bool withScale = false, const juce::String& label = {})
    {
        auto& pid = ctlParam (ctlIdx);
        auto* k = new KnobW (pid, big);
        k->defaultVal = mod.ctls[(size_t) ctlIdx].def;
        knobs.add (k);
        addAndMakeVisible (k);
        float r = big ? 0.31f : 0.25f;
        k->setBounds (pxr (sq (x, y, r)));
        if (big) arches.push_back ({ x - r - 0.08f, y - r - 0.1f, 2 * (r + 0.08f), r + 0.18f });
        if (withScale) scales.push_back ({ x - 0.42f, y + r + 0.03f, 0.84f, 0.16f });
        if (label.isNotEmpty())
            labels.push_back ({ label, { x - 0.6f, y + r + (withScale ? 0.2f : 0.04f), 1.2f, 0.17f }, 0.15f, false });
        return k;
    }
    void addDiamond (float x, float y, float r = 0.34f)
    {
        addLine (x, y - r, x + r, y); addLine (x + r, y, x, y + r);
        addLine (x, y + r, x - r, y); addLine (x - r, y, x, y - r);
    }
    void addTRS (float x, float y) { trs.push_back ({ x, y }); }
    std::vector<juce::Rectangle<float>> arches;
    std::vector<juce::Point<float>> trs;
    ToggleW* addToggle (int ctlIdx, float x, float y, int n)
    {
        auto* t = new ToggleW (ctlParam (ctlIdx), n);
        toggles.add (t);
        addAndMakeVisible (t);
        t->setBounds (pxr ({ x - 0.14f, y - 0.26f, 0.28f, 0.52f }));
        return t;
    }
    ButtonW* addButton (int ctlIdx, float x, float y, float r = 0.24f);
    LedW* addLed (int ledIdx, float x, float y, float r = 0.13f)
    {
        auto* l = new LedW (mod, ledIdx);
        leds.add (l);
        addAndMakeVisible (l);
        l->setBounds (pxr (sq (x, y, r)));
        return l;
    }
    void addLabel (const juce::String& t, float x, float y, float w = 1.2f, float h = 0.15f, bool boxed = false)
    {
        labels.push_back ({ t, { x - w / 2, y, w, h + 0.03f }, h, boxed });
    }
    void addGlyph (Glyph gl, float x, float y, float w = 0.3f, float h = 0.2f)
    {
        glyphs.push_back ({ { x - w / 2, y - h / 2, w, h }, gl });
    }
    void addBox (float x, float y, float w, float h) { boxes.push_back ({ x, y, w, h }); }
    void addLine (float x1, float y1, float x2, float y2) { lines.push_back ({ x1, y1, x2, y2 }); }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (darkFace ? col::ioBg : col::cream);
        g.fillRect (b);
        g.setColour (darkFace ? col::ioBg.brighter (0.12f) : col::creamShade);
        g.fillRect (b.removeFromBottom (4.0f));
        g.setColour (col::frame);
        g.drawRect (getLocalBounds().toFloat(), 1.4f);

        auto inkCol = darkFace ? col::ioInk : col::ink;
        if (! darkFace)
        {
            g.setColour (inkCol);
            g.setFont (panelFont (0.205f * PX, true));
            g.drawFittedText (mod.title,
                              juce::Rectangle<float> (4.0f, 0.20f * PX, (float) getWidth() - 8.0f, 0.28f * PX).getSmallestIntegerContainer(),
                              juce::Justification::centred, 1, 0.62f);
        }

        g.setColour (darkFace ? col::ioInk.withAlpha (0.5f) : col::boxLine);
        for (auto& bx : boxes) g.drawRect (bx * PX, 1.1f);
        for (auto& ln : lines) g.drawLine ({ ln.getStart() * PX, ln.getEnd() * PX }, 1.1f);
        // candy-arch outlines over large knobs (per the manual drawings)
        for (auto& a : arches)
        {
            auto rr = a * PX;
            juce::Path p;
            p.startNewSubPath (rr.getX(), rr.getBottom());
            p.lineTo (rr.getX(), rr.getBottom() - rr.getHeight() * 0.28f);
            p.addArc (rr.getX(), rr.getY(), rr.getWidth(), rr.getHeight() * 1.44f,
                      -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, false);
            p.lineTo (rr.getRight(), rr.getBottom());
            g.strokePath (p, juce::PathStrokeType (1.1f));
        }
        // TRS balanced-out jack graphic: concentric rings
        for (auto& t : trs)
        {
            auto c = t * PX;
            g.setColour (col::ink);
            for (float rr : { 0.21f, 0.145f, 0.075f })
                g.drawEllipse (juce::Rectangle<float> (2 * rr * PX, 2 * rr * PX).withCentre (c), rr > 0.2f ? 2.2f : 1.4f);
            g.fillEllipse (juce::Rectangle<float> (0.07f * PX, 0.07f * PX).withCentre (c));
        }

        for (auto& [r, gl] : glyphs)
        {
            juce::Graphics::ScopedSaveState ss (g);
            drawGlyph (g, r * PX, gl);
        }
        for (auto& t : labels)
        {
            auto rr = t.r * PX;
            bool dim = t.text.startsWith ("~"); // software-only hint label, kept recessive
            auto txt = dim ? t.text.substring (1) : t.text;
            g.setColour (dim ? inkCol.withAlpha (0.42f) : inkCol);
            g.setFont (panelFont ((dim ? t.h * 0.85f : t.h) * PX, false));
            g.drawFittedText (txt, rr.getSmallestIntegerContainer(), juce::Justification::centred,
                              txt.containsChar ('\n') ? 2 : 1, 0.7f);
            if (t.boxed)
            {
                g.setColour (col::boxLine);
                auto tw = juce::jmax (24.0f, panelFont (t.h * PX).getStringWidthFloat (t.text) + 8.0f);
                g.drawRect (juce::Rectangle<float> (tw, rr.getHeight()).withCentre (rr.getCentre()), 0.9f);
            }
        }
        for (auto& s : scales)
        {
            auto rr = s * PX;
            g.setColour (inkCol);
            g.setFont (panelFont (rr.getHeight() * 0.95f));
            g.drawText ("-", rr.removeFromLeft (rr.getWidth() * 0.25f), juce::Justification::centredLeft);
            auto rr2 = s * PX;
            g.drawText ("+", rr2.removeFromRight (rr2.getWidth() * 0.25f), juce::Justification::centredRight);
            auto mid = s * PX;
            g.drawLine (mid.getX() + mid.getWidth() * 0.22f, mid.getCentreY(),
                        mid.getRight() - mid.getWidth() * 0.22f, mid.getCentreY(), 0.8f);
        }
    }

    pf50::Module& mod;
    float wIn;
    CableHost*& host;
    PF50Processor& proc;
    juce::OwnedArray<JackW> jacks;
    juce::OwnedArray<KnobW> knobs;
    juce::OwnedArray<ToggleW> toggles;
    juce::OwnedArray<ButtonW> buttons;
    juce::OwnedArray<LedW> leds;

private:
    juce::RangedAudioParameter& ctlParam (int ctlIdx);
};

} // namespace pf50ui
