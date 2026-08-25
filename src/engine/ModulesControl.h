// Paperface 50 — slopes & sequencing module DSP
// (Positive/Negative Slew, Envelope, Dual Gates, sequencers, clock, routers,
//  Peak & Trough, and the software I/O strip). See docs/PANEL_SPEC.md.
#pragma once
#include "ModulesVoice.h"

namespace pf50 {

// --------------------------------------------------------- Positive Slew ----
// Dual. Follows falling voltages exactly, slews rising ones. START triggers an
// envelope-style rise to +5 V; pulse fires on arrival; SUSTAIN holds the top.
class PosSlew : public Module
{
public:
    // per section: IN, VC, START, SUSTAIN, OUT(black), OUT(white), PULSE, PULSE-INV
    enum J { jIn0, jVC0, jStart0, jSus0, jOutB0, jOutW0, jP0, jPI0,
             jIn1, jVC1, jStart1, jSus1, jOutB1, jOutW1, jP1, jPI1 };
    enum C { cRate0, cProc0, cRate1, cProc1 };
    PosSlew()
    {
        for (int s = 0; s < 2; ++s)
        {
            addJack ("IN",      JackType::In,  JackColor::White);
            addJack ("VC",      JackType::In,  JackColor::White);
            addJack ("START",   JackType::In,  JackColor::Red);
            addJack ("SUSTAIN", JackType::In,  JackColor::Red);
            addJack ("OUT",     JackType::Out, JackColor::Black);
            addJack ("OUT",     JackType::Out, JackColor::White);
            addJack ("PULSE",   JackType::Out, JackColor::Red);
            addJack ("PULSE",   JackType::Out, JackColor::Red); // inverted
        }
        addCtl ("RATE", 0.4f); addCtl ("PROC", 0.5f);
        addCtl ("RATE", 0.4f); addCtl ("PROC", 0.5f);
        leds.resize (2);
    }
    void prepare (double s) override { Module::prepare (s); for (int i = 0; i < 2; ++i) { y[i] = 0; latch[i] = false; edge[i] = {}; } }

    void tick() override
    {
        const float dt = 1.0f / sr;
        for (int s = 0; s < 2; ++s)
        {
            const int jb = s * 8, cb = s * 2;
            float rc = std::clamp (ctls[cb].v + av (ctls[cb+1].v) * in (jb+1) / kCV, 0.0f, 1.2f);
            float rate = expmap (rc, 0.55f, 24000.0f); // V/s (fast → sub-ms rise)
            if (edge[s].rising (in (jb+2))) latch[s] = true;

            bool sus = isHigh (in (jb+3));
            float target = latch[s] ? kCV : in (jb);
            float& v = y[s];
            if (target < v) v = sus && latch[s] ? v : target;            // falls follow exactly
            else            v += std::min (target - v, rate * dt);       // rises slew
            if (latch[s] && v >= kCV - 1.0e-3f)
            {
                v = kCV;
                if (! sus) latch[s] = false;                             // release: follow input again
            }
            // Hardware-style top comparator: pulse HIGH while the output sits
            // at +5 V (mirror of the Negative Slew's 0 V end pulse).
            float p = (v >= kCV - 0.02f) ? kCV : 0.0f;
            jacks[jb+4].value = v - kAudio;      // black, centered
            jacks[jb+5].value = v;               // white 0..5
            jacks[jb+6].value = p;
            jacks[jb+7].value = kCV - p;
            leds[(size_t) s] = v / kCV;
        }
    }
    float y[2] {}; bool latch[2] {};
    EdgeDetector edge[2];
};

// --------------------------------------------------------- Negative Slew ----
// Dual (top fast: cycles to ~3 kHz; bottom slow: down to ~20 s). Follows rising
// exactly, slews falling. Pulse fires on reaching 0 V; PULSE→IN self-cycles as
// an inverted saw. Anniversary "wildcard" input morphs the slope shape:
// ~2.5 V = linear, 0/5 V = log/exponential.
class NegSlew : public Module
{
public:
    enum J { jIn0, jVC0, jWild0, jOutB0, jOutW0, jP0,
             jIn1, jVC1, jWild1, jOutB1, jOutW1, jP1 };
    enum C { cRate0, cProc0, cRate1, cProc1 };
    NegSlew()
    {
        for (int s = 0; s < 2; ++s)
        {
            addJack ("IN",       JackType::In,  JackColor::White);
            addJack ("VC",       JackType::In,  JackColor::White);
            addJack ("WILDCARD", JackType::In,  JackColor::White);
            addJack ("OUT",      JackType::Out, JackColor::Black);
            addJack ("OUT",      JackType::Out, JackColor::White);
            addJack ("PULSE",    JackType::Out, JackColor::Red);
        }
        addCtl ("RATE", 0.5f); addCtl ("PROC", 0.5f);
        addCtl ("RATE", 0.5f); addCtl ("PROC", 0.5f);
        leds.resize (2);
    }
    void prepare (double s) override { Module::prepare (s); y[0] = y[1] = 0; }

    void tick() override
    {
        const float dt = 1.0f / sr;
        for (int s = 0; s < 2; ++s)
        {
            const int jb = s * 6, cb = s * 2;
            float rc = std::clamp (ctls[cb].v + av (ctls[cb+1].v) * in (jb+1) / kCV, 0.0f, 1.2f);
            float rate = expmap (rc, s == 0 ? 0.55f : 0.27f, s == 0 ? 34000.0f : 17000.0f); // V/s
            float target = in (jb);
            float& v = y[s];
            if (target > v) v = target;                                   // rises follow exactly
            else
            {
                float w = connected (jb+2) ? std::clamp (in (jb+2) / kCV, 0.0f, 1.0f) : 0.5f;
                float k = (w - 0.5f) * 2.0f;                              // -1 log … 0 lin … +1 expo
                float p = std::clamp ((v - target) / kCV, 1.0e-3f, 1.0f);
                float shape = std::pow (p, k);                            // rate multiplier vs position
                v -= std::min (v - target, rate * shape * dt);
            }
            // Hardware-style end comparator: the pulse output is HIGH whenever
            // the output sits at 0 V. This is what makes PULSE→IN self-start
            // and self-cycle (inverted saw), exactly as on the panel.
            jacks[jb+3].value = v - kAudio;
            jacks[jb+4].value = v;
            jacks[jb+5].value = (v <= 0.02f) ? kCV : 0.0f;
            leds[(size_t) s] = std::clamp (v / kCV, 0.0f, 1.0f);
        }
    }
    float y[2] {};
};

// --------------------------------------------------------------- Envelope ---
// Complex AR envelope (p. 17). END fires only when START-triggered; CYCLE out
// can be looped to START; WINDOW is a delayed gate; HOLD freezes; retriggers
// are ignored while running (→ sub-harmonic generation at audio rate).
class EnvGen : public Module
{
public:
    enum J { jEnd, jOut, jStart, jWindow, jCycle, jSize, jHold, jDur, jDur2 };
    enum C { cRise, cFall, cSize, cDurProc };
    EnvGen()
    {
        addJack ("END",     JackType::Out, JackColor::Red);
        addJack ("OUT",     JackType::Out, JackColor::White);
        addJack ("START",   JackType::In,  JackColor::Red);
        addJack ("WINDOW",  JackType::Out, JackColor::Red);
        addJack ("CYCLE",   JackType::Out, JackColor::Red);
        addJack ("SIZE",    JackType::In,  JackColor::White);
        addJack ("HOLD",    JackType::In,  JackColor::Red);
        addJack ("DURATION",JackType::In,  JackColor::White);
        addJack ("VC",      JackType::In,  JackColor::White); // center panel jack; summed duration CV
        addCtl ("RISE", 0.35f); addCtl ("FALL", 0.35f); addCtl ("SIZE", 0.5f); addCtl ("PROC", 0.5f);
        leds.resize (2); // out level, window
    }
    void prepare (double s) override { Module::prepare (s); y=0; st=Idle; prog=0; startEdge={}; endPg={}; cycPg={}; }

    enum St { Idle, Rise, Fall };
    void tick() override
    {
        const float dt = 1.0f / sr;
        float scale = std::exp2 (av (ctls[cDurProc].v) * (in (jDur) + in (jDur2)) / 1.25f); // ±4 oct over 5 V
        float riseT = expmap (ctls[cRise].v, 0.0008f, 8.0f) / std::max (scale, 1.0e-3f);
        float fallT = expmap (ctls[cFall].v, 0.0008f, 8.0f) / std::max (scale, 1.0e-3f);

        // A START edge that coincides with our own CYCLE pulse counts as
        // "cycled", not "triggered" — END stays quiet for cycled loops (p. 17).
        bool cycled = cycPg.active();
        if (startEdge.rising (in (jStart)) && st == Idle) { st = Rise; prog = 0; viaStart = ! cycled; }

        if (! isHigh (in (jHold)))
        {
            if (st == Rise)
            {
                y += kCV * dt / riseT; prog += 0.5f * dt / riseT;
                if (y >= kCV) { y = kCV; st = Fall; prog = 0.5f; }
            }
            else if (st == Fall)
            {
                y -= kCV * dt / fallT; prog += 0.5f * dt / fallT;
                if (y <= 0)
                {
                    y = 0; st = Idle; prog = 1.0f;
                    if (viaStart) endPg.fire (sr, 0.4f);
                    cycPg.fire (sr, 0.8f);
                    viaStart = false;
                }
            }
        }
        float winStart = std::clamp (ctls[cSize].v + in (jSize) / kCV, 0.0f, 0.98f);
        bool windowHigh = (st != Idle) && prog >= winStart;

        jacks[jOut].value    = y;
        jacks[jEnd].value    = endPg.tick();
        jacks[jCycle].value  = cycPg.tick();
        jacks[jWindow].value = windowHigh ? kCV : 0.0f;
        leds[0] = y / kCV; leds[1] = windowHigh ? 1.0f : 0.0f;
    }
    float y = 0, prog = 0; St st = Idle; bool viaStart = false;
    EdgeDetector startEdge; PulseGen endPg, cycPg;
};

// ------------------------------------------------------------- Dual Gates ---
// 2023 redesign (p. 18): per section DC (linear VCA) / AC (expo audio VCA) /
// MOD (4-quadrant multiply; ring mod at full gain), modes freely abusable.
class DualGates : public Module
{
public:
    enum J { jIn0, jLin0, jLog0, jMod0, jOut0,
             jIn1, jLin1, jLog1, jMod1, jOut1 };
    enum C { cGain0, cMode0, cGain1, cMode1 };
    DualGates()
    {
        for (int s = 0; s < 2; ++s)
        {
            addJack ("IN",  JackType::In,  JackColor::Black);
            addJack ("LIN", JackType::In,  JackColor::White);
            addJack ("LOG", JackType::In,  JackColor::White);
            addJack ("MOD", JackType::In,  JackColor::Black);
            addJack ("OUT", JackType::Out, JackColor::Black);
        }
        addCtl ("GAIN", 0.75f); addCtl ("MODE", 1.0f, 3); // 0 DC, 1 AC, 2 MOD
        addCtl ("GAIN", 0.75f); addCtl ("MODE", 1.0f, 3);
        leds.resize (2);
    }
    void tick() override
    {
        for (int s = 0; s < 2; ++s)
        {
            const int jb = s * 5, cb = s * 2;
            float x = in (jb);
            float g = ctls[cb].v;
            int mode = (int) std::lround (ctls[cb+1].v);
            float out = 0;
            float linK = connected (jb+1) ? std::clamp (in (jb+1) / kCV, 0.0f, 1.6f) : 1.0f;
            float logK = 1.0f;
            if (connected (jb+2))
            {
                float v = std::clamp (in (jb+2), 0.0f, 7.5f);
                logK = std::pow (10.0f, (v - kCV) * (50.0f / kCV) / 20.0f); // -50 dB at 0 V, unity at 5 V
            }
            float modK = connected (jb+3) ? in (jb+3) / kAudio : 1.0f;
            switch (mode)
            {
                case 0:  out = x * g * linK * (connected (jb+3) ? modK : 1.0f); break;                     // DC linear
                case 1: { float kdB = -66.0f * (1.0f - g);
                          out = x * std::pow (10.0f, kdB / 20.0f) * logK * linK
                                  * (connected (jb+3) ? modK : 1.0f); break; }                             // AC expo
                default: out = x * modK * g * linK * logK; break;                                          // MOD ring
            }
            out = CVPro::softlim (out);
            jacks[jb+4].value = out;
            leds[(size_t) s] = std::clamp (std::fabs (out) / kCV, 0.0f, 1.0f);
        }
    }
};

// ---------------------------------------------------------- Gate Sequencer --
// 8 one-hot pulse outs, CLOCK/RESET/HOLD + reset button (p. 19).
class GateSeq : public Module
{
public:
    enum J { jS1, jS2, jS3, jS4, jS5, jS6, jS7, jS8, jClock, jReset, jHold };
    enum C { cResetBtn };
    GateSeq()
    {
        for (int i = 0; i < 8; ++i) addJack ("STAGE", JackType::Out, JackColor::Red);
        addJack ("CLOCK", JackType::In, JackColor::Red);
        addJack ("RESET", JackType::In, JackColor::Red);
        addJack ("HOLD",  JackType::In, JackColor::Red);
        addCtl ("RESET", 0.0f, 2);
        leds.resize (8);
    }
    void prepare (double s) override { Module::prepare (s); stage = 0; ck = {}; rs = {}; btnPrev = false; }
    void tick() override
    {
        bool btn = ctls[cResetBtn].v > 0.5f;
        if ((btn && ! btnPrev) || rs.rising (in (jReset))) stage = 0;
        btnPrev = btn;
        bool clockEdge = ck.rising (in (jClock));
        if (clockEdge && ! isHigh (in (jHold))) stage = (stage + 1) & 7;
        for (int i = 0; i < 8; ++i)
        {
            jacks[(size_t) i].value = (i == stage ? kCV : 0.0f);
            leds[(size_t) i] = (i == stage ? 1.0f : 0.0f);
        }
    }
    int stage = 0; EdgeDetector ck, rs; bool btnPrev = false;
};

// ------------------------------------------------------ Sequencer / SEQ4 ----
// 4-stage × 3-row programmer/sequencer (pp. 20–21).
class Seq4 : public Module
{
public:
    enum J { jSel1, jSel2, jSel3, jSel4, jG1, jG2, jG3, jG4,
             jGate, jA, jB, jC, jClock, jReset, jHold, jUpDown };
    // ctls: 12 knobs rows A,B,C × stage, then 4 buttons
    Seq4()
    {
        for (int i = 0; i < 4; ++i) addJack ("SELECT", JackType::In,  JackColor::Red);
        for (int i = 0; i < 4; ++i) addJack ("GATE",   JackType::Out, JackColor::Red);
        addJack ("GATE", JackType::Out, JackColor::Red);
        addJack ("A", JackType::Out, JackColor::White);
        addJack ("B", JackType::Out, JackColor::White);
        addJack ("C", JackType::Out, JackColor::White);
        addJack ("CLOCK", JackType::In, JackColor::Red);
        addJack ("RESET", JackType::In, JackColor::Red);
        addJack ("HOLD",  JackType::In, JackColor::Red);
        addJack ("UP/DOWN", JackType::In, JackColor::Red);
        static const char* rows[3] = { "A", "B", "C" };
        for (int r = 0; r < 3; ++r)
            for (int s = 0; s < 4; ++s) addCtl (rows[r], 0.5f);
        for (int s = 0; s < 4; ++s) addCtl ("BTN", 0.0f, 2);
        leds.resize (4);
    }
    void prepare (double s) override { Module::prepare (s); stage = 0; ck={}; rs={}; for (auto& e : sel) e = {}; for (auto& b : btnPrev) b = false; }
    void tick() override
    {
        bool anyPress = false;
        for (int i = 0; i < 4; ++i)
        {
            bool b = ctls[(size_t) (12 + i)].v > 0.5f;
            if (b && ! btnPrev[i]) stage = i;
            if (b) anyPress = true;
            btnPrev[i] = b;
            if (sel[i].rising (in (jSel1 + i))) stage = i;
            if (isHigh (in (jSel1 + i))) anyPress = true;
        }
        if (rs.rising (in (jReset))) stage = 0;
        if (ck.rising (in (jClock)) && ! isHigh (in (jHold)))
        {
            int dir = connected (jUpDown) ? (isHigh (in (jUpDown)) ? 1 : -1) : 1;
            stage = (stage + dir + 4) & 3;
        }
        jacks[jA].value = ctls[(size_t) (0 + stage)].v * kCV;
        jacks[jB].value = ctls[(size_t) (4 + stage)].v * kCV;
        jacks[jC].value = ctls[(size_t) (8 + stage)].v * kCV;
        for (int i = 0; i < 4; ++i)
        {
            jacks[jG1 + i].value = (i == stage ? kCV : 0.0f);
            leds[(size_t) i] = (i == stage ? 1.0f : 0.0f);
        }
        jacks[jGate].value = anyPress ? kCV : 0.0f;
    }
    int stage = 0; EdgeDetector ck, rs, sel[4]; bool btnPrev[4] {};
};

// ------------------------------------------------------- Metronomic Clock ---
class MetroClock : public Module
{
public:
    enum J { jOut };
    enum C { cTempo, cFine };
    MetroClock()
    {
        addJack ("METRONOME", JackType::Out, JackColor::Red);
        addCtl ("TEMPO", 0.5f); addCtl ("FINE", 0.5f);
        leds.resize (1);
    }
    void prepare (double s) override { Module::prepare (s); ph = 0; }
    void tick() override
    {
        float f = expmap (ctls[cTempo].v, 0.1f, 30.0f) * (1.0f + (ctls[cFine].v - 0.5f) * 0.06f);
        ph += f / sr; if (ph >= 1.0f) ph -= 1.0f;
        bool hi = ph < std::max (0.02f, f * 0.002f * 2.0f); // ~2 ms trigger
        jacks[jOut].value = hi ? kCV : 0.0f;
        leds[0] = hi ? 1.0f : leds[0] * 0.995f;
    }
    float ph = 0;
};

// ------------------------------------------------- Bi-directional routers ---
// Three analog switches (p. 23): OFF connects A0↔B, ON connects A1↔B. Top two
// follow the SWITCH level (momentary); the bottom is a flip-flop with FLOP out.
class BiRouter : public Module
{
public:
    // per section: A1, B, A0, SWITCH (+FLOP on section 2)
    enum J { jA1_0, jB_0, jA0_0, jSw_0,
             jA1_1, jB_1, jA0_1, jSw_1,
             jA1_2, jB_2, jA0_2, jSw_2, jFlop };
    BiRouter()
    {
        for (int s = 0; s < 3; ++s)
        {
            addJack ("A1", JackType::Bi, JackColor::Grey);
            addJack ("B",  JackType::Bi, JackColor::White);
            addJack ("A0", JackType::Bi, JackColor::Grey);
            addJack ("SWITCH", JackType::In, JackColor::Red);
        }
        addJack ("FLOP", JackType::Out, JackColor::Red);
    }
    void prepare (double s) override { Module::prepare (s); ff = false; swEdge = {}; }
    void tick() override
    {
        for (int s = 0; s < 3; ++s)
        {
            const int jb = s * 4;
            bool on;
            if (s < 2) on = isHigh (in (jb+3));
            else { if (swEdge.rising (in (jb+3))) ff = ! ff; on = ff; }

            auto& jA1 = jacks[(size_t)(jb+0)];
            auto& jB  = jacks[(size_t)(jb+1)];
            auto& jA0 = jacks[(size_t)(jb+2)];
            if (jB.biDriven) // distribute: B is fed, route to A0 or A1
            {
                float v = in (jb+1);
                jA1.value = on ? v : 0.0f;
                jA0.value = on ? 0.0f : v;
                jB.value = 0.0f;
            }
            else             // select: A0/A1 feed B
            {
                jB.value  = on ? in (jb+0) : in (jb+2);
                jA1.value = 0.0f; jA0.value = 0.0f;
            }
        }
        jacks[jFlop].value = ff ? 0.0f : kCV; // FLOP is ON while switch is OFF
    }
    bool ff = false; EdgeDetector swEdge;
};

// ---------------------------------------------------------- Peak & Trough ---
class PeakTrough : public Module
{
public:
    enum J { jPeak, jTrough, jIn1, jIn2 };
    PeakTrough()
    {
        addJack ("PEAK",   JackType::Out, JackColor::White);
        addJack ("TROUGH", JackType::Out, JackColor::White);
        addJack ("IN",     JackType::In,  JackColor::White);
        addJack ("IN",     JackType::In,  JackColor::White);
    }
    void tick() override
    {
        float a = in (jIn1), b = in (jIn2);
        jacks[jPeak].value   = std::max (a, b);
        jacks[jTrough].value = std::min (a, b);
    }
};

// ------------------------------------------------------------- I/O strip ----
// Software addition: MIDI→CV, host clock, DAW audio in (FX build), master out.
class IOStrip : public Module
{
public:
    enum J { jPitch, jGate, jVel, jClk, jAudioL, jAudioR, jMainL, jMainR };
    enum C { cVolume };
    IOStrip()
    {
        addJack ("1V/O",  JackType::Out, JackColor::White);
        addJack ("GATE",  JackType::Out, JackColor::Red);
        addJack ("VEL",   JackType::Out, JackColor::White);
        addJack ("CLK",   JackType::Out, JackColor::Red);
        addJack ("IN L",  JackType::Out, JackColor::Black); // DAW → panel
        addJack ("IN R",  JackType::Out, JackColor::Black);
        addJack ("OUT L", JackType::In,  JackColor::Black); // panel → DAW
        addJack ("OUT R", JackType::In,  JackColor::Black);
        addCtl ("VOLUME", 0.8f);
        leds.resize (1);
    }
    void tick() override
    {
        jacks[jPitch].value = pitchV;
        jacks[jGate].value  = gateV;
        jacks[jVel].value   = velV;
        jacks[jClk].value   = clkV;
        jacks[jAudioL].value = audioL;
        jacks[jAudioR].value = audioR;
        float vol = ctls[cVolume].v;
        outL = std::tanh (in (jMainL) * vol / 3.0f) * 3.0f / kAudio; // soft-limited, ±1 nominal
        outR = connected (jMainR) ? std::tanh (in (jMainR) * vol / 3.0f) * 3.0f / kAudio : outL;
        leds[0] = std::clamp (std::fabs (outL), 0.0f, 1.0f);
    }
    // written by the plugin layer each sample/block:
    float pitchV = 0, gateV = 0, velV = 0, clkV = 0, audioL = 0, audioR = 0;
    // read by the plugin layer:
    float outL = 0, outR = 0;
};

} // namespace pf50
