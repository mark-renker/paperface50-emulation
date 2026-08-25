// Paperface 50 — main panel view (three panel rows + I/O strip), banana-cable
// interaction layer, and the plugin editor.
#pragma once
#include "ModulePanels.h"
#include "../plugin/PluginProcessor.h"

namespace pf50ui {

constexpr float RAIL_IN = 0.24f;   // wooden side rails, inches
constexpr int   HEADER_H = 30;
constexpr int   ROW_GAP = 20;
constexpr float IO_H_IN = 1.24f;

inline juce::RangedAudioParameter& ModuleComp::ctlParam (int ctlIdx)
{
    auto* p = proc.apvts.getParameter (PF50Processor::paramID (mod, ctlIdx));
    jassert (p != nullptr);
    return *p;
}

inline ButtonW* ModuleComp::addButton (int ctlIdx, float x, float y, float r)
{
    auto* b = new ButtonW();
    auto* modPtr = &mod;
    auto* procPtr = &proc;
    b->onPress = [procPtr, modPtr, ctlIdx] (bool down) { procPtr->setMomentary (modPtr, ctlIdx, down); };
    buttons.add (b);
    addAndMakeVisible (b);
    b->setBounds (pxr (sq (x, y, r)));
    return b;
}

inline void buildIOStrip (ModuleComp& c, pf50::IOStrip& io, float wIn)
{
    juce::ignoreUnused (io);
    using IO = pf50::IOStrip;
    c.darkFace = true;
    const float y = 0.42f;
    float x = 0.55f;
    auto put = [&] (int jack, const juce::String& lab) { c.addJack (jack, x, y, lab); x += 0.78f; };
    put (IO::jPitch, "1V/O"); put (IO::jGate, "GATE"); put (IO::jVel, "VEL"); put (IO::jClk, "CLK");
    x += 0.35f;
    put (IO::jAudioL, "IN L"); put (IO::jAudioR, "IN R");
    x += 0.35f;
    put (IO::jMainL, "OUT L"); put (IO::jMainR, "OUT R");
    x += 0.25f;
    c.addKnob (IO::cVolume, x, y, true, false, "VOLUME");
    x += 0.75f;
    c.addLed (0, x, y);
    c.addLabel ("SERGE PAPERFACE 50  -  SOFTWARE EMULATION  -  BLACK TIE VENTURES", wIn - 3.4f, y - 0.08f, 6.2f, 0.14f);
}

// --------------------------------------------------------------------------
class PanelView : public juce::Component, public CableHost, private juce::Timer
{
public:
    explicit PanelView (PF50Processor& p) : proc (p)
    {
        hostPtr = this;

        auto widthOf = [] (pf50::Module* m) -> float
        {
            auto id = m->id;
            if (id == "vco") return 2; if (id == "vcoP") return 3;
            if (id == "ssgH" || id == "ssgP") return 3;
            if (id == "cvproH" || id == "cvproP") return 3;
            if (id == "tws") return 3; if (id == "vcfH" || id == "vcfP") return 2;
            if (id == "pos") return 3; if (id == "neg") return 2;
            if (id == "env") return 2; if (id == "gates") return 3;
            if (id == "gseq") return 2; if (id == "seq4a" || id == "seq4b") return 5;
            if (id == "clock") return 1; if (id == "router") return 2;
            if (id == "pt") return 1;
            return 2;
        };

        const std::vector<std::vector<pf50::Module*>> rows = {
            { proc.sys.vco, proc.sys.vcoP, proc.sys.ssgH, proc.sys.cvproH, proc.sys.tws, proc.sys.vcfH },
            { proc.sys.pos, proc.sys.neg, proc.sys.ssgP, proc.sys.env, proc.sys.cvproP, proc.sys.gates, proc.sys.vcfP },
            { proc.sys.gseq, proc.sys.seq4a, proc.sys.clock, proc.sys.router, proc.sys.seq4b, proc.sys.pt }
        };
        rowNames = { "SERGE HELIOS", "SERGE PARIS-FLASH", "SERGE TEMPO DI ROMA" };

        float maxRowIn = 0;
        for (auto& row : rows)
        {
            float w = 2 * RAIL_IN;
            for (auto* m : row) w += widthOf (m);
            maxRowIn = std::max (maxRowIn, w);
        }
        logicalW = (int) std::ceil (maxRowIn * PX) + 20;
        int y = HEADER_H + 14;
        for (auto& row : rows)
        {
            float rowWIn = 2 * RAIL_IN;
            for (auto* m : row) rowWIn += widthOf (m);
            int x = (logicalW - (int) (rowWIn * PX)) / 2;
            juce::Rectangle<int> rr (x, y, (int) (rowWIn * PX), (int) (MOD_H * PX));
            rowRects.push_back (rr);
            x += (int) (RAIL_IN * PX);
            for (auto* m : row)
            {
                auto* mc = new ModuleComp (*m, widthOf (m), hostPtr, proc);
                buildModule (*mc);
                mods.add (mc);
                addAndMakeVisible (mc);
                mc->setTopLeftPosition (x, y);
                x += mc->getWidth();
            }
            y += (int) (MOD_H * PX) + ROW_GAP;
        }
        // I/O strip
        {
            float wIn = maxRowIn;
            auto* mc = new ModuleComp (*proc.sys.io, wIn, hostPtr, proc);
            buildIOStrip (*mc, *proc.sys.io, wIn);
            mc->setSize ((int) (wIn * PX), (int) (IO_H_IN * PX));
            mods.add (mc);
            addAndMakeVisible (mc);
            mc->setTopLeftPosition ((logicalW - mc->getWidth()) / 2, y);
            ioRect = mc->getBounds();
            y += mc->getHeight() + 8;
        }
        logicalH = y;
        setSize (logicalW, logicalH);

        // header widgets
        presets.addItemList (PF50Processor::factoryPatchNames(), 1);
        presets.setTextWhenNothingSelected ("PATCHES");
        presets.onChange = [this]
        {
            int i = presets.getSelectedItemIndex();
            if (i >= 0) proc.loadFactoryPatch (i);
        };
        addAndMakeVisible (presets);
        presets.setBounds (logicalW - 320, 4, 200, HEADER_H - 8);

        clearBtn.setButtonText ("CLEAR");
        clearBtn.onClick = [this] { proc.clearAllCables(); };
        addAndMakeVisible (clearBtn);
        clearBtn.setBounds (logicalW - 112, 4, 92, HEADER_H - 8);

        for (auto* mc : mods)
            for (auto* j : mc->jacks)
                allJacks.push_back (j);

        startTimerHz (30);
        setOpaque (true);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::headerBg);
        g.setColour (col::ioInk);
        g.setFont (panelFont (15.0f, true));
        g.drawText ("SERGE  PAPERFACE  50TH  ANNIVERSARY  EDITION", 14, 0, 700, HEADER_H, juce::Justification::centredLeft);
        // boat backgrounds + wood rails + row nameplates
        for (size_t i = 0; i < rowRects.size(); ++i)
        {
            auto rr = rowRects[i].toFloat();
            g.setColour (juce::Colour (0xff0e0d0f));
            g.fillRect (rr.expanded (3.0f));
            juce::ignoreUnused (i);
            g.setGradientFill (juce::ColourGradient (col::wood, rr.getX(), rr.getY(), col::woodDark, rr.getX(), rr.getBottom(), false));
            g.fillRect (juce::Rectangle<float> (rr.getX(), rr.getY(), RAIL_IN * PX, rr.getHeight()));
            g.fillRect (juce::Rectangle<float> (rr.getRight() - RAIL_IN * PX, rr.getY(), RAIL_IN * PX, rr.getHeight()));
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            for (float wx : { rr.getX() + 3.0f, rr.getRight() - RAIL_IN * PX + 3.0f })
                for (int k = 0; k < 3; ++k)
                    g.drawVerticalLine ((int) (wx + k * 3.7f), rr.getY() + 4, rr.getBottom() - 4);
            // boat nameplate on the lower rail edge
            g.setColour (juce::Colour (0xff0e0d0f));
            g.fillRect (juce::Rectangle<float> (rr.getX() - 3, rr.getBottom() - 1, rr.getWidth() + 6, 15.0f));
            g.setColour (col::ioInk.withAlpha (0.8f));
            g.setFont (panelFont (10.0f, true));
            g.drawText (rowNames[(int) i], (int) rr.getX() + 8, (int) rr.getBottom() - 1, 300, 14, juce::Justification::centredLeft);
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // ---- banana cables ----
        auto cables = proc.cableSnapshot();
        int idx = 0;
        for (auto& c : cables)
        {
            auto* a = findJack (c.modA, c.jackA);
            auto* b = findJack (c.modB, c.jackB);
            if (a && b) drawCable (g, centreOf (a), centreOf (b), cableColour (idx));
            ++idx;
        }
        if (dragSrc != nullptr)
            drawCable (g, centreOf (dragSrc), dragPos, juce::Colours::white.withAlpha (0.75f));
    }

    // ---- CableHost ----
    void jackDragStart (JackW* j) override { dragSrc = j; dragPos = centreOf (j); repaint(); }
    void jackDragMove (juce::Point<float> screen) override
    {
        dragPos = getLocalPoint (nullptr, screen);
        auto* over = jackAt (dragPos);
        for (auto* j : allJacks) j->dropTarget = (j == over && j != dragSrc);
        repaint();
    }
    void jackDragEnd (juce::Point<float> screen) override
    {
        auto pos = getLocalPoint (nullptr, screen);
        auto* dst = jackAt (pos);
        if (dragSrc && dst && dst != dragSrc)
            proc.addCable (proc.sys.engine.indexOf (&dragSrc->mod), dragSrc->jack,
                           proc.sys.engine.indexOf (&dst->mod), dst->jack);
        for (auto* j : allJacks) j->dropTarget = false;
        dragSrc = nullptr;
        repaint();
    }
    void jackRightClick (JackW* j) override
    {
        juce::PopupMenu menu;
        auto cables = proc.cableSnapshot();
        int mIdx = proc.sys.engine.indexOf (&j->mod);
        std::vector<pf50::Cable> mine;
        for (auto& c : cables)
            if ((c.modA == mIdx && c.jackA == j->jack) || (c.modB == mIdx && c.jackB == j->jack))
                mine.push_back (c);
        if (mine.empty()) return;
        int id = 1;
        for (auto& c : mine)
        {
            bool aIsThis = (c.modA == mIdx && c.jackA == j->jack);
            auto& other = aIsThis ? *proc.sys.engine.mods[(size_t) c.modB] : *proc.sys.engine.mods[(size_t) c.modA];
            int oj = aIsThis ? c.jackB : c.jackA;
            menu.addItem (id++, "Remove → " + juce::String (other.id) + " " + other.jacks[(size_t) oj].name);
        }
        menu.addSeparator();
        menu.addItem (999, "Remove all cables here");
        menu.showMenuAsync (juce::PopupMenu::Options(), [this, mine, mIdx, j] (int result)
        {
            if (result == 999) proc.removeCablesAt (mIdx, j->jack);
            else if (result > 0 && result <= (int) mine.size()) proc.removeCable (mine[(size_t) result - 1]);
            repaint();
        });
    }

    int logicalW = 980, logicalH = 1200;

private:
    void timerCallback() override
    {
        for (auto* mc : mods)
            for (auto* l : mc->leds) l->refresh();
        int cv = proc.cableVersion();
        if (cv != lastCableVer) { lastCableVer = cv; repaint(); }
        // seq4 buttons show engine state via LEDs already
    }

    JackW* findJack (int modIdx, int jackIdx)
    {
        auto* m = proc.sys.engine.mods[(size_t) modIdx].get();
        for (auto* j : allJacks)
            if (&j->mod == m && j->jack == jackIdx) return j;
        return nullptr;
    }
    juce::Point<float> centreOf (JackW* j)
    {
        return getLocalPoint (j, j->getLocalBounds().getCentre().toFloat());
    }
    JackW* jackAt (juce::Point<float> p)
    {
        for (auto* j : allJacks)
        {
            auto b = getLocalArea (j, j->getLocalBounds().toFloat()).expanded (5.0f);
            if (b.contains (p)) return j;
        }
        return nullptr;
    }
    static juce::Colour cableColour (int i)
    {
        static const juce::Colour cols[] = {
            juce::Colour (0xffc0392b), juce::Colour (0xff23211e), juce::Colour (0xff2e5fb7),
            juce::Colour (0xff2f7d4f), juce::Colour (0xffd9a62e), juce::Colour (0xff77726a) };
        return cols[i % 6];
    }
    static void drawCable (juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, juce::Colour col)
    {
        float dist = a.getDistanceFrom (b);
        float sag = juce::jmin (150.0f, 22.0f + dist * 0.16f);
        juce::Point<float> m ((a.x + b.x) * 0.5f, juce::jmax (a.y, b.y) + sag);
        juce::Path p;
        p.startNewSubPath (a);
        p.quadraticTo (m, b);
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.strokePath (p, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (col.withAlpha (0.88f));
        g.strokePath (p, juce::PathStrokeType (4.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        for (auto pt : { a, b })
        {
            g.setColour (col.darker (0.4f));
            g.fillEllipse (juce::Rectangle<float> (13, 13).withCentre (pt));
            g.setColour (col);
            g.fillEllipse (juce::Rectangle<float> (8, 8).withCentre (pt));
        }
    }

    PF50Processor& proc;
    CableHost* hostPtr = nullptr;
    juce::OwnedArray<ModuleComp> mods;
    std::vector<JackW*> allJacks;
    std::vector<juce::Rectangle<int>> rowRects;
    juce::Rectangle<int> ioRect;
    juce::StringArray rowNames;
    juce::ComboBox presets;
    juce::TextButton clearBtn;
    JackW* dragSrc = nullptr;
    juce::Point<float> dragPos;
    int lastCableVer = -1;
};

// --------------------------------------------------------------------------
class PF50Editor : public juce::AudioProcessorEditor
{
public:
    explicit PF50Editor (PF50Processor& p) : AudioProcessorEditor (p), view (p)
    {
        addAndMakeVisible (view);
        float aspect = (float) view.logicalW / (float) view.logicalH;
        setResizable (true, true);
        setResizeLimits (420, (int) (420 / aspect), 2400, (int) (2400 / aspect));
        getConstrainer()->setFixedAspectRatio (aspect);
        setSize ((int) (view.logicalW * 0.78f), (int) (view.logicalH * 0.78f));
    }
    void resized() override
    {
        float s = juce::jmin ((float) getWidth() / view.logicalW, (float) getHeight() / view.logicalH);
        view.setTransform (juce::AffineTransform::scale (s));
        view.setTopLeftPosition (0, 0);
    }
    void paint (juce::Graphics& g) override { g.fillAll (pf50ui::col::headerBg); }

private:
    PanelView view;
};

} // namespace pf50ui

using PF50Editor = pf50ui::PF50Editor;
