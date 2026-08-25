// Paperface 50 — full-system graph builder (three panels + I/O strip)
#pragma once
#include "ModulesControl.h"

namespace pf50 {

struct System
{
    Engine engine;

    // Helios (voice)
    VCO*     vco    = nullptr;
    VCO*     vcoP   = nullptr;
    SSG*     ssgH   = nullptr;
    CVPro*   cvproH = nullptr;
    TWS*     tws    = nullptr;
    VCF1973* vcfH   = nullptr;
    // Paris-Flash (slopes)
    PosSlew*   pos    = nullptr;
    NegSlew*   neg    = nullptr;
    SSG*       ssgP   = nullptr;
    EnvGen*    env    = nullptr;
    CVPro*     cvproP = nullptr;
    DualGates* gates  = nullptr;
    VCF1973*   vcfP   = nullptr;
    // Tempo di Roma (sequencing)
    GateSeq*    gseq  = nullptr;
    Seq4*       seq4a = nullptr;
    MetroClock* clock = nullptr;
    BiRouter*   router= nullptr;
    Seq4*       seq4b = nullptr;
    PeakTrough* pt    = nullptr;
    // software I/O
    IOStrip*    io    = nullptr;

    System()
    {
        auto& e = engine;
        vco    = e.add<VCO>     ("vco",    "OSCILLATOR", false);
        vcoP   = e.add<VCO>     ("vcoP",   "OSCILLATOR+", true);
        ssgH   = e.add<SSG>     ("ssgH",   "SMOOTH & STEPPED GENERATOR");
        cvproH = e.add<CVPro>   ("cvproH", "DUAL CV-PRO");
        tws    = e.add<TWS>     ("tws",    "TRIPLE WAVESHAPER");
        vcfH   = e.add<VCF1973> ("vcfH",   "FILTER");
        pos    = e.add<PosSlew> ("pos",    "POSITIVE SLEW");
        neg    = e.add<NegSlew> ("neg",    "NEGATIVE SLEW");
        ssgP   = e.add<SSG>     ("ssgP",   "SMOOTH & STEPPED GENERATOR");
        env    = e.add<EnvGen>  ("env",    "ENVELOPE");
        cvproP = e.add<CVPro>   ("cvproP", "DUAL CV-PRO");
        gates  = e.add<DualGates>("gates", "DUAL GATES");
        vcfP   = e.add<VCF1973> ("vcfP",   "FILTER");
        gseq   = e.add<GateSeq> ("gseq",   "SEQUENCER");
        seq4a  = e.add<Seq4>    ("seq4a",  "SEQUENCER / PROGRAMMER");
        clock  = e.add<MetroClock>("clock","CLOCK");
        router = e.add<BiRouter>("router", "ROUTER");
        seq4b  = e.add<Seq4>    ("seq4b",  "SEQUENCER / PROGRAMMER");
        pt     = e.add<PeakTrough>("pt",   "PEAK&TR");
        io     = e.add<IOStrip> ("io",     "I/O");
    }

    void prepare (double sr) { engine.prepare (sr); }

    // convenience for tests/presets
    bool patch (Module* a, int ja, Module* b, int jb)
    {
        return engine.connect (engine.indexOf (a), ja, engine.indexOf (b), jb);
    }
};

} // namespace pf50
