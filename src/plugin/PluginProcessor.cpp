#include "PluginProcessor.h"
#include "../ui/MainView.h"

using namespace pf50;

juce::String PF50Processor::paramID (const Module& m, int c)
{
    return juce::String (m.id) + "_" + juce::String (c);
}

static juce::String paramName (const Module& m, int c)
{
    juce::String base = juce::String (m.id).toUpperCase() + " " + m.ctls[(size_t) c].name;
    return base + " [" + juce::String (c) + "]";
}

juce::AudioProcessorValueTreeState::ParameterLayout PF50Processor::makeLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> ps;
    for (auto& m : sys.engine.mods)
        for (int c = 0; c < (int) m->ctls.size(); ++c)
        {
            auto& ct = m->ctls[(size_t) c];
            if (ct.steps > 0)
                ps.push_back (std::make_unique<juce::AudioParameterChoice> (
                    juce::ParameterID (paramID (*m, c), 1), paramName (*m, c),
                    [&] { juce::StringArray sa; for (int i = 0; i < ct.steps; ++i) sa.add (juce::String (i)); return sa; }(),
                    (int) std::lround (ct.def * (ct.steps == 2 ? 1.0f : 1.0f) * (ct.steps == 3 ? 1.0f : 1.0f))));
            else
                ps.push_back (std::make_unique<juce::AudioParameterFloat> (
                    juce::ParameterID (paramID (*m, c), 1), paramName (*m, c),
                    juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f), ct.def));
        }
    return { ps.begin(), ps.end() };
}

PF50Processor::PF50Processor()
    : AudioProcessor (
#if PF50_FX
        BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
#else
        BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)
#endif
      ),
      apvts (*this, nullptr, "PF50PARAMS", makeLayout())
{
    for (auto& m : sys.engine.mods)
        for (int c = 0; c < (int) m->ctls.size(); ++c)
        {
            paramPtrs.push_back (apvts.getRawParameterValue (paramID (*m, c)));
            auto& ct = m->ctls[(size_t) c];
            if (ct.steps == 2) // momentary-capable buttons
            {
                auto mf = std::make_unique<MomentaryFlag>();
                mf->mod = m.get(); mf->ctl = c;
                momentaries.push_back (std::move (mf));
            }
        }
}

const juce::String PF50Processor::getName() const
{
#if PF50_FX
    return "Paperface 50 FX";
#else
    return "Paperface 50";
#endif
}

bool PF50Processor::isBusesLayoutSupported (const BusesLayout& l) const
{
#if PF50_FX
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
#else
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
#endif
}

void PF50Processor::prepareToPlay (double sr, int)
{
    const juce::SpinLock::ScopedLockType sl (engineLock);
    sys.prepare (sr);
}

void PF50Processor::applyParams()
{
    size_t i = 0;
    for (auto& m : sys.engine.mods)
        for (auto& c : m->ctls)
        {
            // getRawParameterValue returns the denormalized value: for choice
            // params that is already the index (0..steps-1), which is exactly
            // what the engine's ctls expect.
            c.v = paramPtrs[i++]->load();
        }
    // momentary overrides (panel buttons held with the mouse)
    for (auto& mf : momentaries)
        if (mf->down.load()) mf->mod->ctls[(size_t) mf->ctl].v = 1.0f;
}

void PF50Processor::processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals nd;
    const int n = buf.getNumSamples();
    auto* io = sys.io;

    const juce::SpinLock::ScopedTryLockType sl (engineLock);
    if (! sl.isLocked()) { buf.clear(); return; }

    applyParams();

    // choice params arrive normalized 0..1 from getRawParameterValue; rescale 3-state
    // (handled in applyParams)

    // host clock → 16th-note pulses on the CLK jack
    double bpm = 120.0; bool playing = false; double ppq = 0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (pos->getBpm()) bpm = *pos->getBpm();
            playing = pos->getIsPlaying();
            if (pos->getPpqPosition()) ppq = *pos->getPpqPosition();
        }
    const double sr = getSampleRate();
    const double sixteenthHz = bpm / 60.0 * 4.0;

    float* outL = buf.getWritePointer (0);
    float* outR = buf.getNumChannels() > 1 ? buf.getWritePointer (1) : nullptr;
#if PF50_FX
    const float* inL = buf.getReadPointer (0);
    const float* inR = buf.getNumChannels() > 1 ? buf.getReadPointer (1) : buf.getReadPointer (0);
#endif

    auto midiIt = midi.begin();
    if (playing && ! wasPlaying) clockPhase = 0;
    wasPlaying = playing;

    for (int s = 0; s < n; ++s)
    {
        while (midiIt != midi.end() && (*midiIt).samplePosition <= s)
        {
            auto msg = (*midiIt).getMessage();
            if (msg.isNoteOn())
            {
                notesDown.add (msg.getNoteNumber());
                heldNote = msg.getNoteNumber();
                heldVel = msg.getFloatVelocity();
            }
            else if (msg.isNoteOff())
            {
                notesDown.removeValue (msg.getNoteNumber());
                if (notesDown.size() > 0) heldNote = notesDown.getLast();
            }
            else if (msg.isAllNotesOff() || msg.isAllSoundOff())
                notesDown.clear();
            ++midiIt;
        }
        io->gateV = notesDown.size() > 0 ? kCV : 0.0f;
        io->pitchV = heldNote >= 0 ? (heldNote - 36) / 12.0f : 0.0f;
        io->velV = heldVel * kCV;

        if (playing)
        {
            clockPhase += sixteenthHz / sr;
            if (clockPhase >= 1.0) clockPhase -= 1.0;
            io->clkV = clockPhase < 0.15 ? kCV : 0.0f;
        }
        else io->clkV = 0.0f;

#if PF50_FX
        io->audioL = inL[s] * kAudio;   // ±1 → ±2.5 V
        io->audioR = inR[s] * kAudio;
#else
        io->audioL = io->audioR = 0.0f;
#endif
        sys.engine.tick();

        // master = MAIN jacks + the two BAL OUT taps
        float bal = (sys.vcfH->balTap + sys.vcfP->balTap) / kAudio * 0.5f;
        float l = io->outL + bal, r = io->outR + bal;
        outL[s] = l;
        if (outR) outR[s] = r;
    }
    midi.clear();
}

// ------------------------------------------------------------- cables -------
bool PF50Processor::addCable (int mA, int jA, int mB, int jB)
{
    const juce::SpinLock::ScopedLockType sl (engineLock);
    bool ok = sys.engine.connect (mA, jA, mB, jB);
    if (ok) ++cableVer;
    return ok;
}
void PF50Processor::removeCablesAt (int m, int j)
{
    const juce::SpinLock::ScopedLockType sl (engineLock);
    sys.engine.disconnectAt (m, j); ++cableVer;
}
void PF50Processor::removeCable (const Cable& c)
{
    const juce::SpinLock::ScopedLockType sl (engineLock);
    sys.engine.removeCable (c); ++cableVer;
}
void PF50Processor::clearAllCables()
{
    const juce::SpinLock::ScopedLockType sl (engineLock);
    sys.engine.clearCables(); ++cableVer;
}
std::vector<Cable> PF50Processor::cableSnapshot()
{
    const juce::SpinLock::ScopedLockType sl (engineLock);
    return sys.engine.cables;
}
void PF50Processor::setMomentary (Module* m, int ctlIdx, bool down)
{
    for (auto& mf : momentaries)
        if (mf->mod == m && mf->ctl == ctlIdx) mf->down.store (down);
}

// ------------------------------------------------------------- state --------
juce::ValueTree PF50Processor::cablesToTree()
{
    juce::ValueTree t ("CABLES");
    for (auto& c : sys.engine.cables)
    {
        juce::ValueTree e ("C");
        e.setProperty ("a",  juce::String (sys.engine.mods[(size_t) c.modA]->id), nullptr);
        e.setProperty ("ja", c.jackA, nullptr);
        e.setProperty ("b",  juce::String (sys.engine.mods[(size_t) c.modB]->id), nullptr);
        e.setProperty ("jb", c.jackB, nullptr);
        t.appendChild (e, nullptr);
    }
    return t;
}
void PF50Processor::cablesFromTree (const juce::ValueTree& t)
{
    sys.engine.cables.clear();
    for (int i = 0; i < t.getNumChildren(); ++i)
    {
        auto e = t.getChild (i);
        auto* a = sys.engine.byId (e.getProperty ("a").toString().toStdString());
        auto* b = sys.engine.byId (e.getProperty ("b").toString().toStdString());
        if (a && b)
        {
            Cable c { sys.engine.indexOf (a), (int) e.getProperty ("ja"),
                      sys.engine.indexOf (b), (int) e.getProperty ("jb") };
            if ((size_t) c.jackA < a->jacks.size() && (size_t) c.jackB < b->jacks.size())
                sys.engine.cables.push_back (c);
        }
    }
    sys.engine.rebuild();
    ++cableVer;
}

void PF50Processor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree root ("PF50");
    root.appendChild (apvts.copyState(), nullptr);
    {
        const juce::SpinLock::ScopedLockType sl (engineLock);
        root.appendChild (cablesToTree(), nullptr);
    }
    juce::MemoryOutputStream mo (dest, true);
    root.writeToStream (mo);
}
void PF50Processor::setStateInformation (const void* data, int size)
{
    auto root = juce::ValueTree::readFromData (data, (size_t) size);
    if (! root.isValid() || ! root.hasType ("PF50")) return;
    auto params = root.getChildWithName (apvts.state.getType());
    if (params.isValid()) apvts.replaceState (params);
    auto cab = root.getChildWithName ("CABLES");
    if (cab.isValid())
    {
        const juce::SpinLock::ScopedLockType sl (engineLock);
        cablesFromTree (cab);
    }
}

// ------------------------------------------------------- factory patches ----
juce::StringArray PF50Processor::factoryPatchNames()
{
    return { "Init (empty)", "First Voice", "Serge Bongos", "Random Dance", "Sequenced Line" };
}

void PF50Processor::loadFactoryPatch (int index)
{
    auto set = [&] (Module* m, int c, float v)
    {
        if (auto* p = apvts.getParameter (paramID (*m, c)))
            p->setValueNotifyingHost (m->ctls[(size_t) c].steps == 3 ? v / 2.0f : v);
    };
    auto& s = sys;
    {
        const juce::SpinLock::ScopedLockType sl (engineLock);
        sys.engine.clearCables();
    }
    // neutral defaults for the master path
    set (s.io, IOStrip::cVolume, 0.8f);

    auto C = [&] (Module* a, int ja, Module* b, int jb) { addCable (s.engine.indexOf (a), ja, s.engine.indexOf (b), jb); };

    switch (index)
    {
        default: break;
        case 1: // First Voice — MIDI → VCO → VCF → AC gate, envelope on both
            C (s.io, IOStrip::jPitch, s.vco, VCO::jV1O);
            C (s.io, IOStrip::jGate,  s.env, EnvGen::jStart);
            C (s.vco, VCO::jSawB,     s.vcfH, VCF1973::jIn);
            C (s.vcfH, VCF1973::jLow, s.gates, DualGates::jIn0);
            C (s.env, EnvGen::jOut,   s.gates, DualGates::jLog0);
            C (s.env, EnvGen::jOut,   s.vcfH, VCF1973::jVCFreq);
            C (s.gates, DualGates::jOut0, s.io, IOStrip::jMainL);
            set (s.gates, DualGates::cMode0, 1.0f); // AC
            set (s.gates, DualGates::cGain0, 0.0f);
            set (s.vcfH, VCF1973::cFreq, 0.42f);
            set (s.vcfH, VCF1973::cQ, 0.35f);
            set (s.vcfH, VCF1973::cVCProc, 0.78f);
            set (s.env, EnvGen::cRise, 0.12f);
            set (s.env, EnvGen::cFall, 0.55f);
            break;
        case 2: // Serge Bongos — clocked envelope pings the filter into drums
            C (s.clock, MetroClock::jOut, s.env, EnvGen::jStart);
            C (s.env, EnvGen::jOut, s.gates, DualGates::jLin0);
            C (s.env, EnvGen::jOut, s.vcfP, VCF1973::jVCFreq);
            C (s.vcfP, VCF1973::jBand, s.vcfP, VCF1973::jIn); // ringing feedback
            C (s.vcfP, VCF1973::jLow, s.gates, DualGates::jIn0);
            C (s.gates, DualGates::jOut0, s.io, IOStrip::jMainL);
            set (s.clock, MetroClock::cTempo, 0.62f);
            set (s.env, EnvGen::cRise, 0.02f); set (s.env, EnvGen::cFall, 0.3f);
            set (s.vcfP, VCF1973::cQ, 0.93f);
            set (s.vcfP, VCF1973::cFreq, 0.35f);
            set (s.vcfP, VCF1973::cGain, 0.4f);
            set (s.vcfP, VCF1973::cVCProc, 0.62f);
            set (s.gates, DualGates::cMode0, 0.0f); set (s.gates, DualGates::cGain0, 1.0f);
            break;
        case 3: // Random Dance — S/H random melody through the waveshaper
            C (s.vcoP, VCO::jSHSrc, s.ssgH, SSG::jStIn);
            C (s.clock, MetroClock::jOut, s.ssgH, SSG::jStSample);
            C (s.ssgH, SSG::jStOut, s.vco, VCO::jV1O);
            C (s.vco, VCO::jSineB, s.tws, TWS::jIn0);
            C (s.tws, TWS::jB0, s.gates, DualGates::jIn0);
            C (s.clock, MetroClock::jOut, s.env, EnvGen::jStart);
            C (s.env, EnvGen::jOut, s.gates, DualGates::jLog0);
            C (s.gates, DualGates::jOut0, s.io, IOStrip::jMainL);
            set (s.clock, MetroClock::cTempo, 0.7f);
            set (s.ssgH, SSG::cStRate, 0.95f);
            set (s.tws, TWS::cAtt0, 0.55f);
            set (s.env, EnvGen::cRise, 0.05f); set (s.env, EnvGen::cFall, 0.42f);
            set (s.gates, DualGates::cMode0, 1.0f); set (s.gates, DualGates::cGain0, 0.0f);
            set (s.vco, VCO::cFreq, 0.45f);
            break;
        case 4: // Sequenced Line — SEQ4 melody, gate seq accents
            C (s.clock, MetroClock::jOut, s.seq4a, Seq4::jClock);
            C (s.seq4a, Seq4::jA, s.vco, VCO::jV1O);
            C (s.clock, MetroClock::jOut, s.env, EnvGen::jStart);
            C (s.vco, VCO::jSawB, s.vcfH, VCF1973::jIn);
            C (s.seq4a, Seq4::jB, s.vcfH, VCF1973::jVCFreq);
            C (s.vcfH, VCF1973::jLow, s.gates, DualGates::jIn0);
            C (s.env, EnvGen::jOut, s.gates, DualGates::jLog0);
            C (s.gates, DualGates::jOut0, s.io, IOStrip::jMainL);
            set (s.clock, MetroClock::cTempo, 0.68f);
            set (s.vco, VCO::cFreq, 0.3f);
            set (s.vcfH, VCF1973::cFreq, 0.4f); set (s.vcfH, VCF1973::cQ, 0.5f);
            set (s.vcfH, VCF1973::cVCProc, 0.7f);
            set (s.env, EnvGen::cRise, 0.03f); set (s.env, EnvGen::cFall, 0.4f);
            set (s.gates, DualGates::cMode0, 1.0f); set (s.gates, DualGates::cGain0, 0.0f);
            // a little melody
            for (int st = 0; st < 4; ++st) set (s.seq4a, st, 0.25f + 0.11f * st);
            break;
    }
}

juce::AudioProcessorEditor* PF50Processor::createEditor() { return new PF50Editor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PF50Processor(); }
