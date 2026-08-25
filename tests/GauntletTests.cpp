// Paperface 50 — Gauntlet hard gate G2: headless DSP verification suite.
// Frozen with gauntlet/GAUNTLET.md. Pure C++17, no JUCE. Exit code = #failures.
#include "../src/engine/System.h"
#include <chrono>
#include <cstdio>
#include <functional>
#include <vector>
#include <cmath>

using namespace pf50;
static int passed = 0, failed = 0;
static const char* group = "";

static void CHECK (bool ok, const char* name, double a = 0, double b = 0)
{
    if (ok) { ++passed; std::printf ("  PASS  %-46s", name); }
    else    { ++failed; std::printf ("  FAIL  %-46s", name); }
    if (a != 0 || b != 0) std::printf ("  [%.6g / %.6g]", a, b);
    std::printf ("\n");
}
static void GROUP (const char* g) { group = g; std::printf ("\n== %s ==\n", g); }

static constexpr double SR = 48000.0;

struct Rig
{
    System sys;
    Rig() { sys.prepare (SR); }
    void step (int n, const std::function<void(int)>& pre = {})
    {
        for (int i = 0; i < n; ++i) { if (pre) pre (i); sys.engine.tick(); }
    }
};

// measure rate of rising edges on a jack over n samples
static double edgeRate (Rig& r, Module* m, int jack, int n, const std::function<void(int)>& pre = {})
{
    int edges = 0; bool prev = false; int first = -1, last = -1;
    for (int i = 0; i < n; ++i)
    {
        if (pre) pre (i);
        r.sys.engine.tick();
        bool h = m->jacks[(size_t) jack].value > kLogicHi;
        if (h && ! prev) { ++edges; if (first < 0) first = i; last = i; }
        prev = h;
    }
    if (edges < 2) return 0.0;
    return (edges - 1) * SR / double (last - first);
}

// Goertzel amplitude of a jack signal at freq f over n samples
struct Goertzel
{
    double s1 = 0, s2 = 0, coeff = 0; int n = 0;
    void init (double f) { coeff = 2.0 * std::cos (2.0 * 3.14159265358979 * f / SR); s1 = s2 = 0; n = 0; }
    void push (double x) { double s0 = x + coeff * s1 - s2; s2 = s1; s1 = s0; ++n; }
    double amp() const
    {
        double re = s1 - s2 * std::cos (2.0 * 3.14159265358979 * 0 / SR); // magnitude via power form
        (void) re;
        double p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        return 2.0 * std::sqrt (std::max (0.0, p)) / std::max (1, n);
    }
};

static double toneAmp (Rig& r, Module* m, int jack, double f, int n, const std::function<void(int)>& pre = {})
{
    Goertzel g; g.init (f);
    for (int i = 0; i < n; ++i) { if (pre) pre (i); r.sys.engine.tick(); g.push (m->jacks[(size_t) jack].value); }
    return g.amp();
}

static double db (double a, double b) { return 20.0 * std::log10 (std::max (a, 1e-12) / std::max (b, 1e-12)); }

// set FREQ ctl for a target VCO frequency
static float vcoKnobFor (double hz) { return (float) (std::log (hz / 13.0) / std::log (4200.0 / 13.0)); }

int main()
{
    std::printf ("Paperface 50 gauntlet — DSP suite (SR=%.0f)\n", SR);

    // T01 ------------------------------------------------------------------
    {
        GROUP ("T01 silence");
        Rig r; r.step (2000);
        bool zero = true, fin = true;
        for (int i = 0; i < 1000; ++i)
        {
            r.sys.engine.tick();
            if (r.sys.io->outL != 0.0f || r.sys.io->outR != 0.0f) zero = false;
            for (auto& m : r.sys.engine.mods)
                for (auto& j : m->jacks) if (! std::isfinite (j.value)) fin = false;
        }
        CHECK (zero, "unpatched system renders exact silence");
        CHECK (fin, "all jack values finite");
    }

    // T02 ------------------------------------------------------------------
    {
        GROUP ("T02 vco 1V/oct tracking");
        Rig r;
        r.sys.vco->ctls[VCO::cFreq].v = vcoKnobFor (261.626);
        r.sys.vco->ctls[VCO::cProc1].v = 0.5f; // fine centered
        r.sys.patch (r.sys.io, IOStrip::jPitch, r.sys.vco, VCO::jV1O);
        for (float volts : { -3.0f, -1.0f, 0.0f, 1.0f, 3.0f })
        {
            r.sys.io->pitchV = volts;
            r.step (4000); // settle
            // frequency via interpolated rising zero-crossings of the sine out
            int n = (int) (SR * 1.5); int nz = 0; double firstT = -1, lastT = -1; float prev = 0;
            for (int i = 0; i < n; ++i)
            {
                r.sys.engine.tick();
                float v = r.sys.vco->jacks[VCO::jSineB].value;
                if (prev < 0.0f && v >= 0.0f)
                {
                    double t = i - v / double (v - prev); // linear interp
                    if (firstT < 0) firstT = t;
                    lastT = t; ++nz;
                }
                prev = v;
            }
            double f = (nz - 1) * SR / (lastT - firstT);
            double expect = 261.626 * std::exp2 (volts);
            double cents = 1200.0 * std::log2 (f / expect);
            char nm[80]; std::snprintf (nm, 80, "tracking %+0.0f V within 3 cents", volts);
            CHECK (std::fabs (cents) <= 3.0, nm, f, cents);
        }
    }

    // T03 ------------------------------------------------------------------
    {
        GROUP ("T03 vco wave ranges");
        Rig r;
        r.sys.vco->ctls[VCO::cFreq].v = vcoKnobFor (220.0);
        float sawWmin=99, sawWmax=-99, sawBmax=-99, sinBmin=99, sinBmax=-99;
        r.step (1000);
        for (int i = 0; i < 48000; ++i)
        {
            r.sys.engine.tick();
            auto& j = r.sys.vco->jacks;
            sawWmin = std::min (sawWmin, j[VCO::jSawW].value); sawWmax = std::max (sawWmax, j[VCO::jSawW].value);
            sawBmax = std::max (sawBmax, std::fabs (j[VCO::jSawB].value));
            sinBmin = std::min (sinBmin, j[VCO::jSineB].value); sinBmax = std::max (sinBmax, j[VCO::jSineB].value);
        }
        CHECK (sawWmin > -0.4f && sawWmax < 5.4f && sawWmax > 4.0f, "saw white ~0..5V", sawWmin, sawWmax);
        CHECK (sawBmax < 3.0f && sawBmax > 2.0f, "saw black ~±2.5V", sawBmax);
        CHECK (sinBmin < -2.0f && sinBmax > 2.0f && sinBmax < 2.8f, "sine black bipolar ~±2.5V", sinBmin, sinBmax);
    }

    // T04 ------------------------------------------------------------------
    {
        GROUP ("T04 negative slew");
        Rig r;
        r.sys.patch (r.sys.neg, NegSlew::jP0, r.sys.neg, NegSlew::jIn0); // self-cycle
        r.sys.neg->ctls[NegSlew::cRate0].v = 1.0f;
        double fMax = edgeRate (r, r.sys.neg, NegSlew::jP0, (int) SR / 2);
        CHECK (fMax >= 2500.0, "top section cycles ≥ 2.5 kHz at max rate", fMax);

        Rig r2; // slope at min rate ≈ 0.55 V/s
        r2.sys.neg->y[0] = 5.0f;
        r2.sys.neg->ctls[NegSlew::cRate0].v = 0.0f;
        r2.step ((int) SR);
        float dropped = 5.0f - r2.sys.neg->y[0];
        CHECK (dropped > 0.35f && dropped < 0.85f, "min rate ≈ 0.55 V/s (slow end)", dropped);

        // wildcard curvature: time 5→2.5 vs 2.5→0.2
        auto fallTimes = [] (float wild, double& t1, double& t2)
        {
            Rig q;
            q.sys.patch (q.sys.io, IOStrip::jVel, q.sys.neg, NegSlew::jWild0);
            q.sys.io->velV = wild;
            q.sys.neg->ctls[NegSlew::cRate0].v = 0.55f;
            q.step (10);
            q.sys.neg->y[0] = 5.0f;
            int i = 0, i1 = -1, i2 = -1;
            while (i < (int) SR * 20 && i2 < 0)
            {
                q.sys.engine.tick(); ++i;
                float y = q.sys.neg->y[0];
                if (i1 < 0 && y <= 2.5f) i1 = i;
                if (y <= 0.2f) i2 = i;
            }
            t1 = i1 / SR; t2 = (i2 - i1) / SR;
        };
        double a1, a2, b1, b2, c1, c2;
        fallTimes (2.5f, a1, a2); fallTimes (5.0f, b1, b2); fallTimes (0.0f, c1, c2);
        CHECK (std::fabs (a1 / a2 - (2.3 / 2.5) * (a1+a2 > 0 ? 1 : 1)) < 0.35 || std::fabs (a1 - a2 * (2.3/2.3)) / std::max(a1,a2) < 0.5,
               "wildcard 2.5V ≈ linear (halves comparable)", a1, a2);
        CHECK (b1 < b2, "wildcard 5V: expo (fast first, slow tail)", b1, b2);
        CHECK (c1 > c2, "wildcard 0V: log (slow first, fast tail)", c1, c2);
    }

    // T05 ------------------------------------------------------------------
    {
        GROUP ("T05 positive slew envelope");
        Rig r;
        r.sys.pos->ctls[PosSlew::cRate0].v = 0.4869f; // ≈100 V/s → 50 ms rise
        r.sys.patch (r.sys.io, IOStrip::jGate, r.sys.pos, PosSlew::jStart0);
        r.sys.patch (r.sys.io, IOStrip::jVel,  r.sys.pos, PosSlew::jSus0);
        int reach = -1; bool pulseSeen = false;
        r.sys.io->velV = 5.0f; // sustain armed
        for (int i = 0; i < (int) SR; ++i)
        {
            r.sys.io->gateV = (i > 10 && i < 200) ? 5.0f : 0.0f;
            r.sys.engine.tick();
            if (reach < 0 && r.sys.pos->y[0] >= 4.99f) reach = i;
            if (r.sys.pos->jacks[PosSlew::jP0].value > kLogicHi) pulseSeen = true;
        }
        double riseMs = (reach - 10) * 1000.0 / SR;
        CHECK (reach > 0 && std::fabs (riseMs - 50.0) < 14.0, "rise time ≈ 50 ms (±25%)", riseMs);
        CHECK (pulseSeen, "pulse fires on reaching +5 V");
        CHECK (r.sys.pos->y[0] >= 4.99f, "SUSTAIN holds at top", r.sys.pos->y[0]);
        r.sys.io->velV = 0.0f; r.step (100);
        CHECK (r.sys.pos->y[0] < 0.2f, "release: falls instantly when sustain drops", r.sys.pos->y[0]);
    }

    // T06 ------------------------------------------------------------------
    {
        GROUP ("T06 smooth & stepped generator");
        Rig r;
        r.sys.patch (r.sys.ssgH, SSG::jSmCycle, r.sys.ssgH, SSG::jSmIn);
        r.sys.ssgH->ctls[SSG::cSmRate].v = 0.55f;
        r.step ((int) SR / 2);
        float mn = 99, mx = -99;
        for (int i = 0; i < (int) SR; ++i)
        { r.sys.engine.tick(); mn = std::min (mn, r.sys.ssgH->ySm); mx = std::max (mx, r.sys.ssgH->ySm); }
        CHECK (mn < 0.6f && mx > 4.0f && mx < 5.2f, "smooth cycle = 0..~5V triangle", mn, mx);
        double f1 = edgeRate (r, r.sys.ssgH, SSG::jSmCycle, (int) SR);
        r.sys.ssgH->ctls[SSG::cSmRate].v = 0.75f;
        double f2 = edgeRate (r, r.sys.ssgH, SSG::jSmCycle, (int) SR);
        CHECK (f2 > f1 * 2.0, "rate control raises cycle freq", f1, f2);

        // track & hold
        r.sys.patch (r.sys.io, IOStrip::jGate, r.sys.ssgH, SSG::jSmHold);
        r.sys.io->gateV = 5.0f; r.step (5);
        float held = r.sys.ssgH->ySm; r.step (4800);
        CHECK (std::fabs (r.sys.ssgH->ySm - held) < 1e-4f, "HOLD freezes smooth section");
        r.sys.io->gateV = 0.0f;

        // stepped staircase driven by a slow pulse
        Rig q;
        q.sys.patch (q.sys.ssgP, SSG::jStCycle, q.sys.ssgP, SSG::jStIn);
        q.sys.patch (q.sys.clock, MetroClock::jOut, q.sys.ssgP, SSG::jStSample);
        q.sys.clock->ctls[MetroClock::cTempo].v = 1.0f; // 30 Hz
        q.sys.ssgP->ctls[SSG::cStRate].v = 0.42f;
        std::vector<float> lv;
        for (int i = 0; i < (int) SR * 2; ++i)
        {
            q.sys.engine.tick();
            if (i % 400 == 0) lv.push_back (q.sys.ssgP->ySt);
        }
        int distinct = 0; float last = -99, lo = 99, hi = -99;
        for (float v : lv) { if (std::fabs (v - last) > 0.15f) { ++distinct; last = v; } lo = std::min (lo, v); hi = std::max (hi, v); }
        CHECK (distinct >= 4 && lo > -0.2f && hi < 5.3f, "stepped staircase cycles within 0..5V", distinct, hi);

        bool redOk = true; float hotMin = 99, hotMax = -99;
        for (int i = 0; i < 4800; ++i)
        {
            q.sys.engine.tick();
            float red = q.sys.ssgP->jacks[SSG::jCoupRed].value;
            if (red != 0.0f && red != kCV) redOk = false;
            hotMin = std::min (hotMin, q.sys.ssgP->jacks[SSG::jCoupHot].value);
            hotMax = std::max (hotMax, q.sys.ssgP->jacks[SSG::jCoupHot].value);
        }
        CHECK (redOk, "coupler red is strict 0/5V comparator");
        CHECK (hotMax > 10.0f || hotMin < -10.0f, "coupler hot swings past ±10V", hotMin, hotMax);
    }

    // T07 ------------------------------------------------------------------
    {
        GROUP ("T07 1973 VCF");
        auto lpAmpAt = [] (double probe, bool lowRange) -> double
        {
            Rig r;
            r.sys.vco->ctls[VCO::cFreq].v = vcoKnobFor (probe);
            r.sys.vco->ctls[VCO::cShape].v = 0.0f;
            r.sys.patch (r.sys.vco, VCO::jSineB, r.sys.vcfH, VCF1973::jIn);
            r.sys.vcfH->ctls[VCF1973::cFreq].v = 0.5f;   // fc ≈ 358 Hz
            r.sys.vcfH->ctls[VCF1973::cQ].v = 0.06f;
            r.sys.vcfH->ctls[VCF1973::cGain].v = 0.35f;  // stay in tanh-linear zone
            r.sys.vcfH->ctls[VCF1973::cLowRange].v = lowRange ? 1.0f : 0.0f;
            r.step (8000);
            return toneAmp (r, r.sys.vcfH, VCF1973::jLow, probe, (int) SR);
        };
        double fc = 357.8;
        double a1 = lpAmpAt (fc * 2.0, false), a2 = lpAmpAt (fc * 8.0, false);
        double slope = db (a1, a2) / 2.0;
        CHECK (std::fabs (slope - 12.0) < 2.5, "LP slope ≈ 12 dB/oct", slope);

        auto hpAmpAt = [] (double probe) -> double
        {
            Rig r;
            r.sys.vco->ctls[VCO::cFreq].v = vcoKnobFor (probe);
            r.sys.patch (r.sys.vco, VCO::jSineB, r.sys.vcfH, VCF1973::jIn);
            r.sys.vcfH->ctls[VCF1973::cFreq].v = 0.5f;
            r.sys.vcfH->ctls[VCF1973::cQ].v = 0.06f;
            r.sys.vcfH->ctls[VCF1973::cGain].v = 0.35f;
            r.step (8000);
            return toneAmp (r, r.sys.vcfH, VCF1973::jHigh, probe, (int) SR);
        };
        double h1 = hpAmpAt (fc / 2.0), h2 = hpAmpAt (fc / 8.0);
        double hslope = db (h1, h2) / 2.0;
        CHECK (std::fabs (hslope - 12.0) < 2.5, "HP slope ≈ 12 dB/oct", hslope);

        auto bpAmpAt = [] (double probe) -> double
        {
            Rig r;
            r.sys.vco->ctls[VCO::cFreq].v = vcoKnobFor (probe);
            r.sys.patch (r.sys.vco, VCO::jSineB, r.sys.vcfH, VCF1973::jIn);
            r.sys.vcfH->ctls[VCF1973::cFreq].v = 0.5f;
            r.sys.vcfH->ctls[VCF1973::cQ].v = 0.5f;
            r.sys.vcfH->ctls[VCF1973::cGain].v = 0.35f;
            r.step (8000);
            return toneAmp (r, r.sys.vcfH, VCF1973::jBand, probe, (int) SR);
        };
        double bC = bpAmpAt (fc), bL = bpAmpAt (fc / 4), bH = bpAmpAt (fc * 4);
        CHECK (bC > bL * 2 && bC > bH * 2, "BP peaks at cutoff", bC, std::max (bL, bH));

        double lr = lpAmpAt (fc * 2.0, true); // LOW range: fc/32 → probe far above
        CHECK (db (lr, a1) < -20.0, "LOW range drops cutoff ≥ 4 octaves", db (lr, a1));

        // stability under fast FM at max Q
        Rig r;
        r.sys.vcoP->ctls[VCO::cFreq].v = vcoKnobFor (900.0);
        r.sys.patch (r.sys.vco, VCO::jSineB, r.sys.vcfP, VCF1973::jIn);
        r.sys.patch (r.sys.vcoP, VCO::jPulse, r.sys.vcfP, VCF1973::jVCFreq);
        r.sys.vcfP->ctls[VCF1973::cQ].v = 1.0f;
        r.sys.vcfP->ctls[VCF1973::cVCProc].v = 1.0f;
        bool fin = true;
        for (int i = 0; i < (int) SR; ++i)
        {
            r.sys.engine.tick();
            for (int j : { (int) VCF1973::jLow, (int) VCF1973::jBand, (int) VCF1973::jHigh })
                if (! std::isfinite (r.sys.vcfP->jacks[(size_t) j].value)) fin = false;
        }
        CHECK (fin, "stable (finite) at max Q under pulse FM");
    }

    // T08 ------------------------------------------------------------------
    {
        GROUP ("T08 dual gates");
        // DC linearity
        {
            Rig r;
            r.sys.patch (r.sys.io, IOStrip::jAudioL, r.sys.gates, DualGates::jIn0);
            r.sys.patch (r.sys.io, IOStrip::jVel,    r.sys.gates, DualGates::jLin0);
            r.sys.gates->ctls[DualGates::cMode0].v = 0.0f;
            r.sys.gates->ctls[DualGates::cGain0].v = 1.0f;
            r.sys.io->audioL = 2.0f;
            double xs = 0, ys = 0, xy = 0, xx = 0; int n = 0;
            for (float lin : { 0.0f, 1.25f, 2.5f, 3.75f, 5.0f })
            {
                r.sys.io->velV = lin; r.step (200);
                double out = r.sys.gates->jacks[DualGates::jOut0].value;
                xs += lin; ys += out; xy += lin * out; xx += lin * lin; ++n;
            }
            double slope = (n * xy - xs * ys) / (n * xx - xs * xs);
            double r2num = 0, r2den = 0, mean = ys / n;
            for (float lin : { 0.0f, 1.25f, 2.5f, 3.75f, 5.0f })
            {
                r.sys.io->velV = lin; r.step (200);
                double out = r.sys.gates->jacks[DualGates::jOut0].value;
                double fit = slope * lin + (ys - slope * xs) / n;
                r2num += (out - fit) * (out - fit); r2den += (out - mean) * (out - mean);
            }
            double r2v = 1.0 - r2num / std::max (r2den, 1e-12);
            CHECK (r2v > 0.99, "DC mode linear in VC (r² > .99)", r2v);
        }
        // MOD ring: carrier suppression
        {
            Rig r;
            r.sys.vco->ctls[VCO::cFreq].v = vcoKnobFor (200.0);
            r.sys.vcoP->ctls[VCO::cFreq].v = vcoKnobFor (310.0);
            r.sys.patch (r.sys.vco,  VCO::jSineB, r.sys.gates, DualGates::jIn0);
            r.sys.patch (r.sys.vcoP, VCO::jSineB, r.sys.gates, DualGates::jMod0);
            r.sys.gates->ctls[DualGates::cMode0].v = 2.0f;
            r.sys.gates->ctls[DualGates::cGain0].v = 1.0f;
            r.step (4000);
            Goertzel gc, gs1, gs2; gc.init (200); gs1.init (510); gs2.init (110);
            for (int i = 0; i < (int) SR * 2; ++i)
            {
                r.sys.engine.tick();
                double v = r.sys.gates->jacks[DualGates::jOut0].value;
                gc.push (v); gs1.push (v); gs2.push (v);
            }
            double carrier = gc.amp(), side = std::max (gs1.amp(), gs2.amp());
            CHECK (db (carrier, side) < -30.0, "MOD ring suppresses carrier ≥ 30 dB", db (carrier, side));
        }
        // AC mode: CCW silence, monotonic
        {
            Rig r;
            r.sys.vco->ctls[VCO::cFreq].v = vcoKnobFor (400.0);
            r.sys.patch (r.sys.vco, VCO::jSineB, r.sys.gates, DualGates::jIn0);
            r.sys.gates->ctls[DualGates::cMode0].v = 1.0f;
            auto rms = [&] (float g)
            {
                r.sys.gates->ctls[DualGates::cGain0].v = g; r.step (2000);
                double acc = 0; int n = 4800;
                for (int i = 0; i < n; ++i) { r.sys.engine.tick(); double v = r.sys.gates->jacks[DualGates::jOut0].value; acc += v * v; }
                return std::sqrt (acc / n);
            };
            double r0 = rms (0.0f), r5 = rms (0.5f), r10 = rms (1.0f);
            CHECK (db (r0, r10) < -60.0, "AC full CCW ≤ -60 dB", db (r0, r10));
            CHECK (r0 < r5 && r5 < r10, "AC gain monotonic", r5, r10);
        }
    }

    // T09 ------------------------------------------------------------------
    {
        GROUP ("T09 sequencers");
        Rig r;
        r.sys.patch (r.sys.io, IOStrip::jClk,  r.sys.gseq, GateSeq::jClock);
        r.sys.patch (r.sys.io, IOStrip::jGate, r.sys.gseq, GateSeq::jReset);
        auto clockPulse = [&] { r.sys.io->clkV = 5; r.step (8); r.sys.io->clkV = 0; r.step (8); };
        bool oneHot = true; int seen[9] {};
        for (int c = 0; c < 10; ++c)
        {
            clockPulse();
            int hot = 0, which = -1;
            for (int i = 0; i < 8; ++i) if (r.sys.gseq->jacks[(size_t) i].value > kLogicHi) { ++hot; which = i; }
            if (hot != 1) oneHot = false; else seen[which]++;
        }
        CHECK (oneHot, "gate seq strictly one-hot");
        CHECK (seen[0] >= 1 && seen[1] >= 1, "gate seq advances and wraps 1..8");
        r.sys.io->gateV = 5; r.step (8); r.sys.io->gateV = 0; r.step (8);
        CHECK (r.sys.gseq->stage == 0, "RESET returns to stage 1");
        r.sys.patch (r.sys.io, IOStrip::jVel, r.sys.gseq, GateSeq::jHold);
        r.sys.io->velV = 5; int st = r.sys.gseq->stage;
        clockPulse(); clockPulse();
        CHECK (r.sys.gseq->stage == st, "HOLD freezes");
        r.sys.io->velV = 0;

        // SEQ4
        Rig q;
        auto& s4 = *q.sys.seq4a;
        for (int i = 0; i < 4; ++i) { s4.ctls[(size_t)(0+i)].v = 0.2f * i; s4.ctls[(size_t)(4+i)].v = 0.1f + 0.2f * i; s4.ctls[(size_t)(8+i)].v = 1.0f - 0.2f * i; }
        q.sys.patch (q.sys.io, IOStrip::jClk, q.sys.seq4a, Seq4::jClock);
        auto qPulse = [&] { q.sys.io->clkV = 5; q.step (8); q.sys.io->clkV = 0; q.step (8); };
        qPulse(); // 0 -> 1
        CHECK (std::fabs (s4.jacks[Seq4::jA].value - 0.2f * 5.0f * 1) < 0.01f, "SEQ4 A follows stage knob", s4.jacks[Seq4::jA].value);
        qPulse(); qPulse(); qPulse(); // wrap to 0
        CHECK (s4.stage == 0, "SEQ4 wraps at 4");
        q.sys.patch (q.sys.io, IOStrip::jVel, q.sys.seq4a, Seq4::jUpDown);
        q.sys.io->velV = 0; qPulse(); // cable present + LOW = backward
        CHECK (s4.stage == 3, "UP/DOWN low runs right-to-left", s4.stage);
        q.sys.io->velV = 5; qPulse();
        CHECK (s4.stage == 0, "UP/DOWN high runs left-to-right", s4.stage);
        q.sys.patch (q.sys.io, IOStrip::jGate, q.sys.seq4a, Seq4::jSel3);
        q.sys.io->gateV = 5; q.step (8);
        CHECK (s4.stage == 2 && s4.jacks[Seq4::jGate].value > kLogicHi, "SELECT jumps stage + common gate");
        CHECK (s4.jacks[Seq4::jG3].value > kLogicHi, "per-stage gate latched");
        q.sys.io->gateV = 0; q.step (8);
    }

    // T10 ------------------------------------------------------------------
    {
        GROUP ("T10 logic & routers");
        Rig r;
        r.sys.patch (r.sys.io, IOStrip::jAudioL, r.sys.pt, PeakTrough::jIn1);
        r.sys.patch (r.sys.io, IOStrip::jAudioR, r.sys.pt, PeakTrough::jIn2);
        r.sys.io->audioL = 1.0f; r.sys.io->audioR = 4.0f; r.step (4);
        CHECK (std::fabs (r.sys.pt->jacks[PeakTrough::jPeak].value - 4.0f) < 1e-3f, "PEAK = max");
        CHECK (std::fabs (r.sys.pt->jacks[PeakTrough::jTrough].value - 1.0f) < 1e-3f, "TROUGH = min");

        // router select mode
        r.sys.patch (r.sys.io, IOStrip::jAudioL, r.sys.router, BiRouter::jA0_0);
        r.sys.patch (r.sys.io, IOStrip::jAudioR, r.sys.router, BiRouter::jA1_0);
        r.sys.patch (r.sys.io, IOStrip::jGate,   r.sys.router, BiRouter::jSw_0);
        r.sys.io->gateV = 0; r.step (4);
        CHECK (std::fabs (r.sys.router->jacks[BiRouter::jB_0].value - 1.0f) < 1e-3f, "switch OFF: B=A0");
        r.sys.io->gateV = 5; r.step (4);
        CHECK (std::fabs (r.sys.router->jacks[BiRouter::jB_0].value - 4.0f) < 1e-3f, "switch ON: B=A1");

        // distribute mode
        r.sys.patch (r.sys.io, IOStrip::jVel, r.sys.router, BiRouter::jB_1);
        r.sys.patch (r.sys.io, IOStrip::jGate, r.sys.router, BiRouter::jSw_1);
        r.sys.io->velV = 3.0f; r.sys.io->gateV = 0; r.step (4);
        CHECK (std::fabs (r.sys.router->jacks[BiRouter::jA0_1].value - 3.0f) < 1e-3f
            && std::fabs (r.sys.router->jacks[BiRouter::jA1_1].value) < 1e-3f, "distribute OFF: B→A0");
        r.sys.io->gateV = 5; r.step (4);
        CHECK (std::fabs (r.sys.router->jacks[BiRouter::jA1_1].value - 3.0f) < 1e-3f, "distribute ON: B→A1");

        // flip-flop divider
        r.sys.patch (r.sys.io, IOStrip::jClk, r.sys.router, BiRouter::jSw_2);
        int flops = 0; bool prev = false;
        for (int c = 0; c < 8; ++c)
        {
            r.sys.io->clkV = 5; r.step (8); r.sys.io->clkV = 0; r.step (8);
            bool f = r.sys.router->jacks[BiRouter::jFlop].value > kLogicHi;
            if (f && ! prev) ++flops;
            prev = f;
        }
        CHECK (flops >= 3 && flops <= 5, "flip-flop divides clock by 2", flops);
    }

    // T11 ------------------------------------------------------------------
    {
        GROUP ("T11 envelope");
        Rig r;
        auto& e = *r.sys.env;
        e.ctls[EnvGen::cRise].v = (float) (std::log (0.010 / 0.0008) / std::log (8.0 / 0.0008));
        e.ctls[EnvGen::cFall].v = (float) (std::log (0.020 / 0.0008) / std::log (8.0 / 0.0008));
        r.sys.patch (r.sys.io, IOStrip::jGate, r.sys.env, EnvGen::jStart);
        int top = -1, done = -1; bool endPulse = false;
        for (int i = 0; i < (int) SR; ++i)
        {
            r.sys.io->gateV = (i > 5 && i < 100) ? 5.0f : 0.0f;
            r.sys.engine.tick();
            if (top < 0 && e.y >= 4.99f) top = i;
            if (top > 0 && done < 0 && e.st == EnvGen::Idle) done = i;
            if (e.jacks[EnvGen::jEnd].value > kLogicHi) endPulse = true;
        }
        double riseMs = (top - 5) / SR * 1000.0, totalMs = (done - 5) / SR * 1000.0;
        CHECK (std::fabs (riseMs - 10.0) < 4.0, "rise ≈ 10 ms", riseMs);
        CHECK (std::fabs (totalMs - 30.0) < 9.0, "total ≈ 30 ms", totalMs);
        CHECK (endPulse, "END fires after triggered envelope");

        // retrigger ignored while running
        int ends = 0; bool pe = false;
        for (int i = 0; i < (int) SR / 2; ++i)
        {
            r.sys.io->gateV = (i % 300 < 60) ? 5.0f : 0.0f; // 160 Hz retrigger vs 30 ms env
            r.sys.engine.tick();
            bool ep = e.jacks[EnvGen::jEnd].value > kLogicHi;
            if (ep && ! pe) ++ends;
            pe = ep;
        }
        CHECK (ends <= (int)(0.5 / 0.030) + 2 && ends >= 3, "retrigger ignored while running (sub-harmonic)", ends);
        r.sys.io->gateV = 0; r.step (4000);

        // cycle loop: CYCLE → START self-oscillates, END quiet
        Rig q;
        auto& e2 = *q.sys.env;
        e2.ctls[EnvGen::cRise].v = 0.3f; e2.ctls[EnvGen::cFall].v = 0.3f;
        q.sys.patch (q.sys.env, EnvGen::jCycle, q.sys.env, EnvGen::jStart);
        q.sys.patch (q.sys.io, IOStrip::jGate, q.sys.env, EnvGen::jStart);
        q.sys.io->gateV = 5; q.step (100); q.sys.io->gateV = 0;
        double f = edgeRate (q, q.sys.env, EnvGen::jCycle, (int) SR * 2);
        CHECK (f > 1.0, "CYCLE→START self-oscillates", f);
        bool endQuiet = true; int k = 0;
        for (int i = 0; i < (int) SR; ++i)
        {
            q.sys.engine.tick();
            if (q.sys.env->jacks[EnvGen::jEnd].value > kLogicHi) ++k;
        }
        endQuiet = (k == 0);
        CHECK (endQuiet, "END quiet while cycling (manual p.17)", k);

        // window
        Rig w;
        auto& e3 = *w.sys.env;
        e3.ctls[EnvGen::cRise].v = e.ctls[EnvGen::cRise].v; e3.ctls[EnvGen::cFall].v = e.ctls[EnvGen::cFall].v;
        e3.ctls[EnvGen::cSize].v = 0.5f;
        w.sys.patch (w.sys.io, IOStrip::jGate, w.sys.env, EnvGen::jStart);
        int wOn = -1, s0 = 5;
        for (int i = 0; i < (int) SR / 4; ++i)
        {
            w.sys.io->gateV = (i > s0 && i < s0 + 50) ? 5.0f : 0.0f;
            w.sys.engine.tick();
            if (wOn < 0 && e3.jacks[EnvGen::jWindow].value > kLogicHi) wOn = i;
        }
        double frac = ((wOn - s0) / SR * 1000.0) / 30.0;
        CHECK (wOn > 0 && frac > 0.3 && frac < 0.75, "WINDOW starts ~mid-envelope at SIZE 50%", frac);
    }

    // T12 ------------------------------------------------------------------
    {
        GROUP ("T12 feedback stress");
        Rig r; auto& s = r.sys;
        s.patch (s.vco, VCO::jSineB, s.tws, TWS::jIn0);
        s.patch (s.tws, TWS::jB0, s.gates, DualGates::jIn0);
        s.patch (s.vcoP, VCO::jSawB, s.gates, DualGates::jMod0);
        s.gates->ctls[DualGates::cMode0].v = 2.0f;
        s.patch (s.gates, DualGates::jOut0, s.cvproH, CVPro::jA1);
        s.patch (s.ssgH, SSG::jSmOut, s.cvproH, CVPro::jA2);
        s.patch (s.cvproH, CVPro::jAOut, s.vco, VCO::jCV2);          // FM feedback
        s.patch (s.ssgH, SSG::jSmCycle, s.ssgH, SSG::jSmIn);         // ssg cycling
        s.patch (s.neg, NegSlew::jP0, s.neg, NegSlew::jIn0);         // neg slew osc
        s.patch (s.neg, NegSlew::jOutB0, s.vcfP, VCF1973::jIn);
        s.patch (s.vcfP, VCF1973::jBand, s.vcfP, VCF1973::jVCFreq);  // filter self-FM
        s.patch (s.clock, MetroClock::jOut, s.router, BiRouter::jSw_2);
        s.patch (s.router, BiRouter::jFlop, s.gseq, GateSeq::jClock);
        s.patch (s.gseq, 0, s.env, EnvGen::jStart);
        s.patch (s.env, EnvGen::jOut, s.gates, DualGates::jLin0);
        s.patch (s.gates, DualGates::jOut0, s.io, IOStrip::jMainL);
        s.vcfP->ctls[VCF1973::cQ].v = 1.0f;
        s.neg->ctls[NegSlew::cRate0].v = 0.9f;
        bool ok = true; float peak = 0;
        for (int i = 0; i < (int) SR * 10; ++i)
        {
            r.sys.engine.tick();
            if ((i & 1023) == 0)
                for (auto& m : s.engine.mods)
                    for (auto& j : m->jacks)
                    {
                        if (! std::isfinite (j.value) || std::fabs (j.value) > kRails + 0.01f) ok = false;
                        peak = std::max (peak, std::fabs (j.value));
                    }
            if (! ok) break;
        }
        CHECK (ok, "10 s pathological patch: finite, within rails", peak);
    }

    // T13 ------------------------------------------------------------------
    {
        GROUP ("T13 state round-trip");
        auto build = [] (Rig& r)
        {
            auto& s = r.sys;
            s.patch (s.vco, VCO::jSawB, s.vcfH, VCF1973::jIn);
            s.patch (s.vcfH, VCF1973::jLow, s.gates, DualGates::jIn0);
            s.patch (s.env, EnvGen::jOut, s.gates, DualGates::jLin0);
            s.patch (s.env, EnvGen::jCycle, s.env, EnvGen::jStart);
            s.patch (s.io, IOStrip::jGate, s.env, EnvGen::jStart);
            s.patch (s.gates, DualGates::jOut0, s.io, IOStrip::jMainL);
            s.vco->ctls[VCO::cFreq].v = 0.4f;
            s.gates->ctls[DualGates::cMode0].v = 0.0f;
        };
        Rig a; build (a);
        // serialize = cable list + ctl values; rebuild fresh and compare renders
        Rig b;
        for (auto& c : a.sys.engine.cables) b.sys.engine.cables.push_back (c);
        b.sys.engine.rebuild();
        for (size_t m = 0; m < a.sys.engine.mods.size(); ++m)
            for (size_t c = 0; c < a.sys.engine.mods[m]->ctls.size(); ++c)
                b.sys.engine.mods[m]->ctls[c].v = a.sys.engine.mods[m]->ctls[c].v;
        bool same = true;
        for (int i = 0; i < 512; ++i)
        {
            a.sys.io->gateV = b.sys.io->gateV = (i > 5 ? 5.0f : 0.0f);
            a.sys.engine.tick(); b.sys.engine.tick();
            if (a.sys.io->outL != b.sys.io->outL) { same = false; break; }
        }
        CHECK (same, "restored graph renders bit-identically");
    }

    // T14 ------------------------------------------------------------------
    {
        GROUP ("T14 performance");
        Rig r; auto& s = r.sys;
        // default factory-ish patch, whole system live
        s.patch (s.io, IOStrip::jPitch, s.vco, VCO::jV1O);
        s.patch (s.io, IOStrip::jGate, s.env, EnvGen::jStart);
        s.patch (s.vco, VCO::jSawB, s.vcfH, VCF1973::jIn);
        s.patch (s.vcfH, VCF1973::jLow, s.gates, DualGates::jIn0);
        s.patch (s.env, EnvGen::jOut, s.gates, DualGates::jLog0);
        s.gates->ctls[DualGates::cMode0].v = 1.0f;
        s.patch (s.gates, DualGates::jOut0, s.io, IOStrip::jMainL);
        s.patch (s.ssgH, SSG::jSmCycle, s.ssgH, SSG::jSmIn);
        s.patch (s.clock, MetroClock::jOut, s.gseq, GateSeq::jClock);
        s.patch (s.neg, NegSlew::jP0, s.neg, NegSlew::jIn0);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < (int) SR * 10; ++i) r.sys.engine.tick();
        auto dt = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        double ratio = 10.0 / dt;
        CHECK (ratio >= 4.0, "≥ 4× realtime single-thread (container)", ratio);
    }

    // T15 ------------------------------------------------------------------
    {
        GROUP ("T15 noise");
        Rig r;
        // spectral trend per the frozen spec: average band power over segments
        const double bands[4] = { 100.0, 400.0, 1600.0, 6400.0 };
        double wPow[4] {}, pPow[4] {};
        float shMin = 99, shMax = -99; double shAcc = 0, shAcc2 = 0; long nSh = 0;
        for (int seg = 0; seg < 8; ++seg)
        {
            Goertzel gw[4], gp[4];
            for (int b = 0; b < 4; ++b) { gw[b].init (bands[b]); gp[b].init (bands[b]); }
            for (int i = 0; i < (int) SR / 2; ++i)
            {
                r.sys.engine.tick();
                float w = r.sys.vcoP->jacks[VCO::jWhite].value;
                float p = r.sys.vcoP->jacks[VCO::jPink].value;
                float sh = r.sys.vcoP->jacks[VCO::jSHSrc].value;
                for (int b = 0; b < 4; ++b) { gw[b].push (w); gp[b].push (p); }
                shMin = std::min (shMin, sh); shMax = std::max (shMax, sh);
                shAcc += sh; shAcc2 += sh * sh; ++nSh;
            }
            for (int b = 0; b < 4; ++b) { wPow[b] += gw[b].amp() * gw[b].amp(); pPow[b] += gp[b].amp() * gp[b].amp(); }
        }
        auto slopePerOct = [&] (const double* pw4)
        {
            // regression of 10log10(power) against log2(f)
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int b = 0; b < 4; ++b)
            {
                double x = std::log2 (bands[b] / bands[0]);
                double y = 10.0 * std::log10 (std::max (pw4[b], 1e-18));
                sx += x; sy += y; sxx += x * x; sxy += x * y;
            }
            return (4 * sxy - sx * sy) / (4 * sxx - sx * sx);
        };
        double ws = slopePerOct (wPow), ps = slopePerOct (pPow);
        CHECK (std::fabs (ws) < 6.0, "white ≈ flat (±6 dB/oct trend)", ws);
        CHECK (std::fabs (ps + 3.0) < 1.5, "pink falls ~3±1.5 dB/oct", ps);
        double mean = shAcc / nSh, var = shAcc2 / nSh - mean * mean;
        CHECK (shMin >= -0.01f && shMax <= 5.01f && std::sqrt (var) > 0.25, "S/H source wanders within 0..5V", shMin, shMax);
    }

    // T16 ------------------------------------------------------------------
    {
        GROUP ("T16 dual cv-pro");
        Rig r;
        r.sys.patch (r.sys.io, IOStrip::jAudioL, r.sys.cvproH, CVPro::jA1);
        r.sys.io->audioL = 3.0f;
        auto& cv = *r.sys.cvproH;
        cv.ctls[CVPro::cA1].v = 1.0f; r.step (4);
        CHECK (std::fabs (cv.jacks[CVPro::jAOut].value - 3.0f) < 1e-3f, "attenuverter +1 passes");
        cv.ctls[CVPro::cA1].v = 0.0f; r.step (4);
        CHECK (std::fabs (cv.jacks[CVPro::jAOut].value + 3.0f) < 1e-3f, "attenuverter -1 inverts");
        cv.ctls[CVPro::cA1].v = 0.5f; cv.ctls[CVPro::cAOff].v = 0.5f; r.step (4);
        CHECK (std::fabs (cv.jacks[CVPro::jAOut].value - 2.5f) < 1e-3f, "offset adds 0..5V");
        r.sys.patch (r.sys.io, IOStrip::jAudioR, r.sys.cvproH, CVPro::jA2);
        r.sys.io->audioL = 12.0f; r.sys.io->audioR = 8.0f;
        cv.ctls[CVPro::cA1].v = 1.0f; cv.ctls[CVPro::cA2].v = 1.0f; cv.ctls[CVPro::cAOff].v = 1.0f; r.step (4);
        float big = cv.jacks[CVPro::jAOut].value;
        CHECK (big > 8.0f && big < 11.6f, "soft limit engages, no rail clip", big);
    }

    // T17 ------------------------------------------------------------------
    {
        GROUP ("T17 triple waveshaper");
        Rig r;
        r.sys.vco->ctls[VCO::cFreq].v = vcoKnobFor (200.0);
        r.sys.patch (r.sys.vco, VCO::jSineB, r.sys.tws, TWS::jIn0);
        r.sys.tws->ctls[TWS::cAtt0].v = 0.5f;
        r.step (4000);
        Goertzel g1, g2; g1.init (200); g2.init (400);
        for (int i = 0; i < (int) SR * 2; ++i)
        {
            r.sys.engine.tick();
            double v = r.sys.tws->jacks[TWS::jB0].value;
            g1.push (v); g2.push (v);
        }
        CHECK (db (g2.amp(), g1.amp()) > 6.0, "sine in → 2F dominant (rectifier)", db (g2.amp(), g1.amp()));

        r.sys.patch (r.sys.io, IOStrip::jVel, r.sys.tws, TWS::jVC1_0);
        auto a600 = [&] (float vc)
        {
            r.sys.io->velV = vc; r.step (2000);
            Goertzel g; g.init (600);
            for (int i = 0; i < (int) SR; ++i) { r.sys.engine.tick(); g.push (r.sys.tws->jacks[TWS::jB0].value); }
            return g.amp();
        };
        double lo = a600 (0.0f), hi = a600 (5.0f);
        CHECK (std::fabs (db (hi, lo)) > 2.0, "VC1 reshapes spectrum", db (hi, lo));
        bool bounded = true;
        for (int i = 0; i < 24000; ++i)
        {
            r.sys.engine.tick();
            if (std::fabs (r.sys.tws->jacks[TWS::jB0].value) > 6.0f || r.sys.tws->jacks[TWS::jW0].value < -0.2f || r.sys.tws->jacks[TWS::jW0].value > 5.4f) bounded = false;
        }
        CHECK (bounded, "outputs bounded (white 0..5, black AC)");
    }

    // T18 ------------------------------------------------------------------
    {
        GROUP ("T18 metronomic clock");
        Rig r;
        r.sys.clock->ctls[MetroClock::cTempo].v = 0.5f;
        double f = edgeRate (r, r.sys.clock, MetroClock::jOut, (int) SR * 6);
        double expect = 0.1 * std::pow (300.0, 0.5);
        CHECK (std::fabs (f / expect - 1.0) < 0.02, "tempo law within 2%", f, expect);
        r.sys.clock->ctls[MetroClock::cFine].v = 1.0f;
        double f2 = edgeRate (r, r.sys.clock, MetroClock::jOut, (int) SR * 6);
        CHECK (f2 > f && (f2 / f - 1.0) < 0.035, "FINE span ≤ ~3%", f2 / f - 1.0);
    }

    std::printf ("\n==================== %d passed, %d failed ====================\n", passed, failed);
    return failed;
}
