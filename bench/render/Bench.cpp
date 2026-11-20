// Paperface 50 — measurement bench renderer (no JUCE).
//
// Renders deterministic test signals through the headless engine and writes raw
// float32 captures plus a manifest; bench/run_bench.py does the analysis.
//
//   pf50_bench render <outdir> [--quick]   offline renders for spectral/IR/null analysis
//   pf50_bench cpu    <outdir> [--quick]   per-block CPU timing, FTZ on/off, subnormal scan
//
// Built against whichever engine tree is on the include path (-I <tree>/src/engine),
// so the same harness can render the current tree and a git reference tree for null tests.
// Levels: engine jacks are volts. "Nominal" audio = ±2.5 V peak (kAudio). The master
// output (I/O strip OUT + BAL taps, exactly as PluginProcessor::processBlock mixes it)
// is in full-scale units (±1.0).
#include "System.h"

#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
 #include <xmmintrin.h>
 #define PF_BENCH_X86 1
#endif

using namespace pf50;

static const double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------ FTZ/DAZ --
struct FlushMode
{
    static bool supported()
    {
#if defined(PF_BENCH_X86) || defined(__aarch64__)
        return true;
#else
        return false;
#endif
    }
    static void set (bool on)
    {
#if defined(PF_BENCH_X86)
        unsigned csr = _mm_getcsr();
        csr = on ? (csr | 0x8040u) : (csr & ~0x8040u); // FTZ (bit 15) | DAZ (bit 6)
        _mm_setcsr (csr);
#elif defined(__aarch64__)
        uint64_t fpcr; __asm__ __volatile__ ("mrs %0, fpcr" : "=r"(fpcr));
        fpcr = on ? (fpcr | (1ull << 24)) : (fpcr & ~(1ull << 24));
        __asm__ __volatile__ ("msr fpcr, %0" :: "r"(fpcr));
#else
        (void) on;
#endif
    }
};

// ----------------------------------------------------------------- manifest --
struct Meta
{
    std::vector<std::pair<std::string, std::string>> kv; // value already JSON-encoded
    Meta& num (const std::string& k, double v) { char b[64]; std::snprintf (b, sizeof b, "%.17g", v); kv.push_back ({ k, b }); return *this; }
    Meta& str (const std::string& k, const std::string& v) { kv.push_back ({ k, "\"" + v + "\"" }); return *this; }
};

struct Manifest
{
    std::string dir;
    std::vector<std::string> entries;
    void add (const std::string& json) { entries.push_back (json); }
    bool write (const std::string& file) const
    {
        FILE* f = std::fopen ((dir + "/" + file).c_str(), "w");
        if (! f) return false;
        std::fprintf (f, "[\n");
        for (size_t i = 0; i < entries.size(); ++i)
            std::fprintf (f, "  %s%s\n", entries[i].c_str(), i + 1 < entries.size() ? "," : "");
        std::fprintf (f, "]\n");
        std::fclose (f);
        return true;
    }
};

static bool writeF32 (const std::string& path, const std::vector<float>& v)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) return false;
    std::fwrite (v.data(), sizeof (float), v.size(), f);
    std::fclose (f);
    return true;
}

// ----------------------------------------------------------------- rendering --
struct Probe { std::string label; std::function<float()> get; };

struct Rig
{
    std::unique_ptr<System> sys;
    double sr;
    explicit Rig (double rate) : sys (std::make_unique<System>()), sr (rate) { sys->prepare (rate); }
    System& s() { return *sys; }
    bool patch (Module* a, int ja, Module* b, int jb) { return sys->patch (a, ja, b, jb); }
    // master output exactly as PluginProcessor::processBlock mixes it
    float masterL() const { return sys->io->outL + (sys->vcfH->balTap + sys->vcfP->balTap) / kAudio * 0.5f; }
    float masterR() const { return sys->io->outR + (sys->vcfH->balTap + sys->vcfP->balTap) / kAudio * 0.5f; }
};

static Manifest gMan;
static bool gQuick = false;

// pre(i) runs before tick i (i counts from the first settle sample); probes are read after tick.
static void render (const std::string& name, const std::string& group, Rig& r,
                    int settle, int n, const std::function<void(long)>& pre,
                    const std::vector<Probe>& probes, Meta meta)
{
    std::vector<std::vector<float>> out (probes.size(), std::vector<float> ((size_t) n));
    long i = 0;
    for (int k = 0; k < settle; ++k, ++i) { if (pre) pre (i); r.s().engine.tick(); }
    for (int k = 0; k < n; ++k, ++i)
    {
        if (pre) pre (i);
        r.s().engine.tick();
        for (size_t p = 0; p < probes.size(); ++p) out[p][(size_t) k] = probes[p].get();
    }
    std::ostringstream js;
    js << "{\"name\":\"" << name << "\",\"group\":\"" << group << "\",\"sr\":" << r.sr
       << ",\"n\":" << n << ",\"settle\":" << settle << ",\"probes\":[";
    for (size_t p = 0; p < probes.size(); ++p)
    {
        const std::string file = name + "__" + probes[p].label + ".f32";
        writeF32 (gMan.dir + "/" + file, out[p]);
        js << (p ? "," : "") << "{\"label\":\"" << probes[p].label << "\",\"file\":\"" << file << "\"}";
    }
    js << "],\"meta\":{";
    for (size_t k = 0; k < meta.kv.size(); ++k)
        js << (k ? "," : "") << "\"" << meta.kv[k].first << "\":" << meta.kv[k].second;
    js << "}}";
    gMan.add (js.str());
}

static std::string fmtHz (double f) { char b[32]; std::snprintf (b, sizeof b, "%g", f); std::string s (b); for (auto& c : s) if (c == '.') c = 'p'; return s; }
static std::string fmtK (double sr) { char b[32]; std::snprintf (b, sizeof b, "%dk", (int) std::lround (sr / 1000.0)); return b; }
static std::string fmtDb (double d) { char b[32]; std::snprintf (b, sizeof b, "%s%g", d < 0 ? "m" : "p", std::fabs (d)); return b; }

static inline float sineAt (double amp, double f, double sr, long i) { return (float) (amp * std::sin (2.0 * kPi * f * (double) i / sr)); }

// ---------------------------------------------------------- factory patches --
// Mirror of PF50Processor::loadFactoryPatch (src/plugin/PluginProcessor.cpp). The
// plugin-level bench (tools/PluginBench.cpp) renders the real plugin and cross-checks
// this copy, so drift between the two is caught there.
static void factoryPatch (System& s, int index)
{
    auto set = [] (Module* m, int c, float v) { m->ctls[(size_t) c].v = v; };
    auto C = [&] (Module* a, int ja, Module* b, int jb) { s.patch (a, ja, b, jb); };
    s.engine.clearCables();
    set (s.io, IOStrip::cVolume, 0.8f);
    switch (index)
    {
        default: break;
        case 1:
            C (s.io, IOStrip::jPitch, s.vco, VCO::jV1O);
            C (s.io, IOStrip::jGate,  s.env, EnvGen::jStart);
            C (s.vco, VCO::jSawB,     s.vcfH, VCF1973::jIn);
            C (s.vcfH, VCF1973::jLow, s.gates, DualGates::jIn0);
            C (s.env, EnvGen::jOut,   s.gates, DualGates::jLog0);
            C (s.env, EnvGen::jOut,   s.vcfH, VCF1973::jVCFreq);
            C (s.gates, DualGates::jOut0, s.io, IOStrip::jMainL);
            set (s.gates, DualGates::cMode0, 1.0f);
            set (s.gates, DualGates::cGain0, 0.0f);
            set (s.vcfH, VCF1973::cFreq, 0.42f);
            set (s.vcfH, VCF1973::cQ, 0.35f);
            set (s.vcfH, VCF1973::cVCProc, 0.78f);
            set (s.env, EnvGen::cRise, 0.12f);
            set (s.env, EnvGen::cFall, 0.55f);
            break;
        case 2:
            C (s.clock, MetroClock::jOut, s.env, EnvGen::jStart);
            C (s.env, EnvGen::jOut, s.gates, DualGates::jLin0);
            C (s.env, EnvGen::jOut, s.vcfP, VCF1973::jVCFreq);
            C (s.vcfP, VCF1973::jBand, s.vcfP, VCF1973::jIn);
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
        case 3:
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
        case 4:
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
            for (int st = 0; st < 4; ++st) set (s.seq4a, st, 0.25f + 0.11f * st);
            break;
    }
}
static const char* kPatchNames[] = { "init", "first_voice", "serge_bongos", "random_dance", "sequenced_line" };

// MIDI emulation identical to processBlock: one note held between on/off sample indices.
struct NoteScript
{
    long on = -1, off = -1; int note = 48; float vel = 0.8f;
    void apply (System& s, long i) const
    {
        bool down = on >= 0 && i >= on && (off < 0 || i < off);
        s.io->gateV  = down ? kCV : 0.0f;
        s.io->pitchV = (on >= 0 && i >= on) ? (note - 36) / 12.0f : 0.0f; // heldNote persists after release
        s.io->velV   = (on >= 0 && i >= on) ? vel * kCV : 0.0f;
    }
};

// Pathological "everything at audio rate" patch for CPU worst case.
static void stressPatch (System& s)
{
    auto C = [&] (Module* a, int ja, Module* b, int jb) { s.patch (a, ja, b, jb); };
    C (s.vcoP, VCO::jSawB, s.vco, VCO::jCV2);
    C (s.vco, VCO::jSineB, s.tws, TWS::jIn0);
    C (s.tws, TWS::jB0, s.tws, TWS::jIn1);
    C (s.tws, TWS::jB1, s.tws, TWS::jIn2);
    C (s.vcoP, VCO::jSineW, s.tws, TWS::jVC1_0);
    C (s.tws, TWS::jB2, s.vcfH, VCF1973::jIn);
    C (s.vcoP, VCO::jSawW, s.vcfH, VCF1973::jVCFreq);
    C (s.vcfH, VCF1973::jBand, s.vcfP, VCF1973::jIn);
    C (s.vcfP, VCF1973::jBand, s.vcfP, VCF1973::jIn);
    C (s.vcoP, VCO::jSineW, s.vcfP, VCF1973::jVCFreq);
    C (s.vcfP, VCF1973::jLow, s.gates, DualGates::jIn0);
    C (s.vcoP, VCO::jPink, s.gates, DualGates::jMod0);
    C (s.neg, NegSlew::jP0, s.neg, NegSlew::jIn0);
    C (s.neg, NegSlew::jOutW0, s.gates, DualGates::jLog0);
    C (s.vcoP, VCO::jPulse, s.env, EnvGen::jStart);
    C (s.env, EnvGen::jCycle, s.env, EnvGen::jStart);
    C (s.env, EnvGen::jOut, s.pos, PosSlew::jIn0);
    C (s.vcoP, VCO::jSHSrc, s.ssgH, SSG::jSmIn);
    C (s.ssgH, SSG::jSmOut, s.cvproH, CVPro::jA1);
    C (s.cvproH, CVPro::jAOut, s.vcoP, VCO::jCV1);
    C (s.clock, MetroClock::jOut, s.seq4a, Seq4::jClock);
    C (s.clock, MetroClock::jOut, s.gseq, GateSeq::jClock);
    C (s.gates, DualGates::jOut0, s.io, IOStrip::jMainL);
    C (s.vcfH, VCF1973::jHigh, s.io, IOStrip::jMainR);
    s.gates->ctls[DualGates::cMode0].v = 2.0f; s.gates->ctls[DualGates::cGain0].v = 1.0f;
    s.vcfP->ctls[VCF1973::cQ].v = 1.0f;
    s.neg->ctls[NegSlew::cRate0].v = 1.0f;
    s.clock->ctls[MetroClock::cTempo].v = 1.0f;
}

// ===================================================================== RENDER ==
static void renderAll()
{
    const std::vector<double> rates = gQuick ? std::vector<double> { 48000.0 } : std::vector<double> { 48000.0, 96000.0, 192000.0 };
    const double A = kAudio; // nominal ±2.5 V

    // ---- VCO aliasing (saw, sine@shape 0, sine@shape 1, pulse) -----------------
    {
        const std::vector<double> freqs = gQuick ? std::vector<double> { 2489.0 } : std::vector<double> { 523.0, 1249.0, 2489.0, 4973.0, 9967.0 };
        for (double sr : rates)
            for (double f : freqs)
            {
                Rig r (sr); auto& s = r.s();
                s.vco->ctls[VCO::cFreq].v = 1.0f;     // 4200 Hz at 0 V
                s.vcoP->ctls[VCO::cFreq].v = 1.0f;
                s.vcoP->ctls[VCO::cShape].v = 1.0f;
                r.patch (s.io, IOStrip::jPitch, s.vco, VCO::jV1O);
                r.patch (s.io, IOStrip::jPitch, s.vcoP, VCO::jV1O);
                s.io->pitchV = (float) std::log2 (f / 4200.0);
                render ("vco_" + fmtHz (f) + "_" + fmtK (sr), "vco_alias", r, (int) (0.05 * sr), (int) sr, {},
                        { { "saw",      [&] { return s.vco->jacks[VCO::jSawB].value; } },
                          { "sine",     [&] { return s.vco->jacks[VCO::jSineB].value; } },
                          { "shape1",   [&] { return s.vcoP->jacks[VCO::jSineB].value; } },
                          { "pulse",    [&] { return s.vcoP->jacks[VCO::jPulse].value - kAudio; } } },
                        Meta().num ("f0", f).num ("ref_amp", A));
            }
    }

    // ---- Triple waveshaper: aliasing vs drive & frequency ------------------------
    {
        const std::vector<double> freqs = gQuick ? std::vector<double> { 3001.0 } : std::vector<double> { 221.0, 1009.0, 3001.0, 7019.0 };
        const std::vector<float> drives = gQuick ? std::vector<float> { 0.75f } : std::vector<float> { 0.25f, 0.5f, 0.75f, 1.0f };
        for (double sr : rates)
            for (double f : freqs)
                for (float att : drives)
                {
                    Rig r (sr); auto& s = r.s();
                    r.patch (s.io, IOStrip::jAudioL, s.tws, TWS::jIn0);
                    s.tws->ctls[TWS::cAtt0].v = att;
                    char nm[96]; std::snprintf (nm, sizeof nm, "tws_%s_d%03d_%s", fmtHz (f).c_str(), (int) std::lround (att * 100), fmtK (sr).c_str());
                    render (nm, "tws_alias", r, (int) (0.1 * sr), (int) sr,
                            [&] (long i) { s.io->audioL = sineAt (A, f, sr, i); },
                            { { "out", [&] { return s.tws->jacks[TWS::jB0].value; } } },
                            Meta().num ("f0", f).num ("drive", att).num ("in_amp", A).num ("ref_amp", A));
                }
    }

    // ---- Master output stage (I/O strip tanh): THD vs level, saw aliasing --------
    {
        const std::vector<double> levels = gQuick ? std::vector<double> { 0.0 } : std::vector<double> { -20, -12, -6, 0, 6 };
        for (double dB : levels)
        {
            const double sr = 48000.0, f = 997.0, amp = A * std::pow (10.0, dB / 20.0);
            Rig r (sr); auto& s = r.s();
            r.patch (s.io, IOStrip::jAudioL, s.io, IOStrip::jMainL);
            render ("master_thd_" + fmtDb (dB), "master_thd", r, 4800, 48000,
                    [&] (long i) { s.io->audioL = sineAt (amp, f, sr, i); },
                    { { "out", [&] { return r.masterL(); } } },
                    Meta().num ("f0", f).num ("level_db", dB).num ("in_amp", amp).num ("ref_amp", 1.0));
        }
        for (double sr : rates)
        {
            const double f = 4973.0;
            Rig r (sr); auto& s = r.s();
            s.vco->ctls[VCO::cFreq].v = 1.0f;
            r.patch (s.io, IOStrip::jPitch, s.vco, VCO::jV1O);
            r.patch (s.vco, VCO::jSawB, s.io, IOStrip::jMainL);
            s.io->pitchV = (float) std::log2 (f / 4200.0);
            render ("master_saw_" + fmtHz (f) + "_" + fmtK (sr), "master_alias", r, (int) (0.05 * sr), (int) sr, {},
                    { { "raw",    [&] { return s.vco->jacks[VCO::jSawB].value / kAudio; } },
                      { "master", [&] { return r.masterL(); } } },
                    Meta().num ("f0", f).num ("ref_amp", 1.0));
        }
    }

    // ---- 1973 VCF: impulse responses (LP/BP/HP) --------------------------------
    {
        const std::vector<float> fknobs = gQuick ? std::vector<float> { 0.5f } : std::vector<float> { 0.25f, 0.5f, 0.75f };
        const std::vector<float> qknobs = gQuick ? std::vector<float> { 0.5f } : std::vector<float> { 0.0f, 0.5f, 1.0f };
        for (float fk : fknobs)
            for (float qk : qknobs)
            {
                const double sr = 48000.0, imp = 1.0e-3; // 1 mV: tanh input stage is linear to < 1e-6
                Rig r (sr); auto& s = r.s();
                s.vcfH->ctls[VCF1973::cFreq].v = fk;
                s.vcfH->ctls[VCF1973::cQ].v = qk;
                r.patch (s.io, IOStrip::jAudioL, s.vcfH, VCF1973::jIn);
                // io ticks after vcfH, so the filter sees io's input one sample later: put the
                // impulse at i=0 and let the analyzer locate it (structural delay is measured separately).
                const double fNom = 16.0 * std::pow (8000.0 / 16.0, (double) fk);
                const double res  = 0.5 * std::pow (22.0 / 0.5, (double) qk);
                const double gainSS = s.vcfH->ctls[VCF1973::cGain].v * 1.5; // small-signal gain of tanh(x*g*1.5/A)*A
                char nm[64]; std::snprintf (nm, sizeof nm, "vcf_f%03d_q%03d", (int) std::lround (fk * 100), (int) std::lround (qk * 100));
                render (nm, "vcf_ir", r, 0, 65536,
                        [&] (long i) { s.io->audioL = (i == 0) ? (float) imp : 0.0f; },
                        { { "lp", [&] { return s.vcfH->jacks[VCF1973::jLow].value; } },
                          { "bp", [&] { return s.vcfH->jacks[VCF1973::jBand].value; } },
                          { "hp", [&] { return s.vcfH->jacks[VCF1973::jHigh].value; } } },
                        Meta().num ("f_nominal", fNom).num ("res", res).num ("imp", imp).num ("gain_ss", gainSS));
            }
    }

    // ---- 1973 VCF: THD vs level (input tanh stage), cutoff wide open -------------
    {
        const std::vector<double> levels = gQuick ? std::vector<double> { 0.0 } : std::vector<double> { -20, -12, -6, 0, 6 };
        const std::vector<double> freqs = gQuick ? std::vector<double> { 997.0 } : std::vector<double> { 101.0, 997.0 };
        for (double f : freqs)
            for (double dB : levels)
            {
                const double sr = 48000.0, amp = A * std::pow (10.0, dB / 20.0);
                Rig r (sr); auto& s = r.s();
                s.vcfH->ctls[VCF1973::cFreq].v = 1.0f;  // 8 kHz
                s.vcfH->ctls[VCF1973::cQ].v = 0.0f;
                r.patch (s.io, IOStrip::jAudioL, s.vcfH, VCF1973::jIn);
                const float k = 1.0f / (0.5f);
                render ("vcf_thd_" + fmtHz (f) + "_" + fmtDb (dB), "vcf_thd", r, 4800, 48000,
                        [&] (long i) { s.io->audioL = sineAt (amp, f, sr, i); },
                        { { "lp", [&] { return s.vcfH->jacks[VCF1973::jLow].value; } },
                          // HP + k·BP + LP reconstructs the saturated input x exactly (TPT SVF identity)
                          { "x",  [&] { return s.vcfH->jacks[VCF1973::jHigh].value + k * s.vcfH->jacks[VCF1973::jBand].value + s.vcfH->jacks[VCF1973::jLow].value; } } },
                        Meta().num ("f0", f).num ("level_db", dB).num ("in_amp", amp).num ("ref_amp", A));
            }
    }

    // ---- Dual gates: linearity (DC mode) and ring-mod carrier suppression ---------
    {
        const double sr = 48000.0;
        for (double dB : { 0.0, 6.0, 12.0 })
        {
            const double f = 997.0, amp = A * std::pow (10.0, dB / 20.0);
            Rig r (sr); auto& s = r.s();
            s.gates->ctls[DualGates::cMode0].v = 0.0f; s.gates->ctls[DualGates::cGain0].v = 1.0f;
            r.patch (s.io, IOStrip::jAudioL, s.gates, DualGates::jIn0);
            render ("gates_dc_thd_" + fmtDb (dB), "gates_thd", r, 4800, 48000,
                    [&] (long i) { s.io->audioL = sineAt (amp, f, sr, i); },
                    { { "out", [&] { return s.gates->jacks[DualGates::jOut0].value; } } },
                    Meta().num ("f0", f).num ("level_db", dB).num ("in_amp", amp).num ("ref_amp", A));
        }
        {
            const double fc = 997.0, fm = 151.0;
            Rig r (sr); auto& s = r.s();
            s.gates->ctls[DualGates::cMode0].v = 2.0f; s.gates->ctls[DualGates::cGain0].v = 1.0f;
            r.patch (s.io, IOStrip::jAudioL, s.gates, DualGates::jIn0);
            r.patch (s.io, IOStrip::jAudioR, s.gates, DualGates::jMod0);
            render ("gates_ringmod", "gates_ringmod", r, 4800, 48000,
                    [&] (long i) { s.io->audioL = sineAt (A, fc, sr, i); s.io->audioR = sineAt (A, fm, sr, i); },
                    { { "out", [&] { return s.gates->jacks[DualGates::jOut0].value; } } },
                    Meta().num ("fc", fc).num ("fm", fm).num ("ref_amp", A));
        }
    }

    // ---- DC offset on bipolar (black) outputs ----------------------------------
    {
        const double sr = 48000.0;
        for (float shape : { 0.0f, 0.5f, 1.0f, 1.3f })
        {
            Rig r (sr); auto& s = r.s();
            s.vco->ctls[VCO::cFreq].v = 1.0f;
            s.vco->ctls[VCO::cShape].v = std::min (shape, 1.0f);
            r.patch (s.io, IOStrip::jPitch, s.vco, VCO::jV1O);
            if (shape > 1.0f) { r.patch (s.io, IOStrip::jVel, s.vco, VCO::jShapeCV); s.io->velV = (shape - 1.0f) * kCV; }
            const double f = 220.0;
            s.io->pitchV = (float) std::log2 (f / 4200.0);
            char nm[64]; std::snprintf (nm, sizeof nm, "dc_vco_shape%03d", (int) std::lround (shape * 100));
            render (nm, "dc", r, 4800, 48000, {},
                    { { "sine", [&] { return s.vco->jacks[VCO::jSineB].value; } },
                      { "saw",  [&] { return s.vco->jacks[VCO::jSawB].value; } } },
                    Meta().num ("f0", f).num ("shape", shape).num ("ref_amp", A));
        }
        {   // TWS black out with asymmetry CV (DC-blocked by design) and the filter bank fed a saw
            Rig r (sr); auto& s = r.s();
            s.vco->ctls[VCO::cFreq].v = 1.0f;
            r.patch (s.io, IOStrip::jPitch, s.vco, VCO::jV1O);
            s.io->pitchV = (float) std::log2 (220.0 / 4200.0);
            r.patch (s.vco, VCO::jSawB, s.tws, TWS::jIn0);
            r.patch (s.io, IOStrip::jVel, s.tws, TWS::jVC2_0); s.io->velV = 2.5f;
            r.patch (s.vco, VCO::jSawB, s.vcfH, VCF1973::jIn);
            render ("dc_tws_vcf", "dc", r, 48000, 48000, {},
                    { { "tws",   [&] { return s.tws->jacks[TWS::jB0].value; } },
                      { "vcf_lp", [&] { return s.vcfH->jacks[VCF1973::jLow].value; } },
                      { "vcf_bp", [&] { return s.vcfH->jacks[VCF1973::jBand].value; } },
                      { "vcf_hp", [&] { return s.vcfH->jacks[VCF1973::jHigh].value; } } },
                    Meta().num ("f0", 220.0).num ("ref_amp", A));
        }
    }

    // ---- Idle noise floor: silence into each audio path --------------------------
    {
        const double sr = 48000.0;
        Rig r (sr); auto& s = r.s();
        r.patch (s.io, IOStrip::jAudioL, s.vcfH, VCF1973::jIn);
        r.patch (s.io, IOStrip::jAudioL, s.tws, TWS::jIn0);
        r.patch (s.io, IOStrip::jAudioL, s.gates, DualGates::jIn0);
        r.patch (s.vcfH, VCF1973::jLow, s.io, IOStrip::jMainL);
        r.patch (s.tws, TWS::jB0, s.io, IOStrip::jMainR);
        render ("noise_silence", "noise", r, 96000, 48000, [&] (long) { s.io->audioL = 0.0f; },
                { { "vcf_lp", [&] { return s.vcfH->jacks[VCF1973::jLow].value; } },
                  { "tws",    [&] { return s.tws->jacks[TWS::jB0].value; } },
                  { "gates",  [&] { return s.gates->jacks[DualGates::jOut0].value; } },
                  { "masterL", [&] { return r.masterL(); } },
                  { "masterR", [&] { return r.masterR(); } } },
                Meta().num ("ref_amp", A));
    }

    // ---- Structural latency: impulse through FX-path routes ----------------------
    {
        const double sr = 48000.0; const long t0 = 100;
        struct Route { const char* name; std::function<void(Rig&)> wire; };
        std::vector<Route> routes {
            { "lat_io_direct", [] (Rig& r) { auto& s = r.s(); r.patch (s.io, IOStrip::jAudioL, s.io, IOStrip::jMainL); } },
            { "lat_io_gates",  [] (Rig& r) { auto& s = r.s(); s.gates->ctls[DualGates::cMode0].v = 0.0f; s.gates->ctls[DualGates::cGain0].v = 1.0f;
                                             r.patch (s.io, IOStrip::jAudioL, s.gates, DualGates::jIn0); r.patch (s.gates, DualGates::jOut0, s.io, IOStrip::jMainL); } },
            { "lat_io_cvpro_gates", [] (Rig& r) { auto& s = r.s(); s.gates->ctls[DualGates::cMode0].v = 0.0f; s.gates->ctls[DualGates::cGain0].v = 1.0f;
                                             s.cvproP->ctls[CVPro::cA1].v = 1.0f;
                                             r.patch (s.io, IOStrip::jAudioL, s.gates, DualGates::jIn0); r.patch (s.gates, DualGates::jOut0, s.cvproP, CVPro::jA1);
                                             r.patch (s.cvproP, CVPro::jAOut, s.io, IOStrip::jMainL); } },
            { "lat_io_balout", [] (Rig& r) { auto& s = r.s(); r.patch (s.io, IOStrip::jAudioL, s.vcfH, VCF1973::jBalIn); } },
        };
        for (auto& rt : routes)
        {
            Rig r (sr); auto& s = r.s(); rt.wire (r);
            s.io->ctls[IOStrip::cVolume].v = 0.8f;
            render (rt.name, "latency", r, 0, 1024,
                    [&] (long i) { s.io->audioL = (i == t0) ? 0.01f : 0.0f; },
                    { { "out", [&] { return r.masterL(); } } },
                    Meta().num ("t0", (double) t0).num ("reported_latency", 0.0));
        }
    }

    // ---- Parameter stepping (zipper): per-block knob updates vs per-sample ramp ---
    {
        const double sr = 48000.0; const int n = 48000;
        for (int block : { 1, 64, 512 })
        {
            Rig r (sr); auto& s = r.s();
            s.gates->ctls[DualGates::cMode0].v = 0.0f;
            r.patch (s.io, IOStrip::jAudioL, s.gates, DualGates::jIn0);
            render ("zip_gates_b" + std::to_string (block), "zipper", r, 0, n,
                    [&] (long i) {
                        long i0 = (i / block) * block; // processBlock applies params once per block
                        s.gates->ctls[DualGates::cGain0].v = (float) (0.25 + 0.75 * (double) i0 / n);
                        s.io->audioL = sineAt (A, 997.0, sr, i); },
                    { { "out", [&] { return s.gates->jacks[DualGates::jOut0].value; } } },
                    Meta().num ("block", block).num ("ref_amp", A));
        }
        for (int block : { 1, 64, 512 })
        {
            Rig r (sr); auto& s = r.s();
            s.vcfH->ctls[VCF1973::cQ].v = 0.5f;
            r.patch (s.io, IOStrip::jAudioL, s.vcfH, VCF1973::jIn);
            render ("zip_vcf_b" + std::to_string (block), "zipper", r, 0, n,
                    [&] (long i) {
                        long i0 = (i / block) * block;
                        s.vcfH->ctls[VCF1973::cFreq].v = (float) (0.3 + 0.5 * (double) i0 / n);
                        s.io->audioL = sineAt (A, 997.0, sr, i); },
                    { { "out", [&] { return s.vcfH->jacks[VCF1973::jLow].value; } } },
                    Meta().num ("block", block).num ("ref_amp", A));
        }
    }

    // ---- Factory patches: stereo master, 4 s (null-test anchors) -------------------
    for (int p = 1; p <= 4; ++p)
    {
        const double sr = 48000.0;
        Rig r (sr); auto& s = r.s();
        factoryPatch (s, p);
        NoteScript ns; ns.on = (long) (0.1 * sr); ns.off = (long) (1.5 * sr);
        render (std::string ("patch_") + kPatchNames[p], "factory", r, 0, gQuick ? 48000 : 192000,
                [&] (long i) { ns.apply (s, i); },
                { { "L", [&] { return r.masterL(); } }, { "R", [&] { return r.masterR(); } } },
                Meta().num ("ref_amp", 1.0).num ("patch", p));
    }
}

// ======================================================================== CPU ==
static bool isSub (float v) { return std::fpclassify (v) == FP_SUBNORMAL; }

static long countSubnormals (System& s)
{
    long c = 0;
    for (auto& m : s.engine.mods) { for (auto& j : m->jacks) c += isSub (j.value); for (float l : m->leds) c += isSub (l); }
    c += isSub (s.vcfH->ic1) + isSub (s.vcfH->ic2) + isSub (s.vcfP->ic1) + isSub (s.vcfP->ic2);
    for (auto& d : s.tws->dc) c += isSub (d.x1) + isSub (d.y1);
    c += isSub (s.pos->y[0]) + isSub (s.pos->y[1]) + isSub (s.neg->y[0]) + isSub (s.neg->y[1]);
    c += isSub (s.env->y) + isSub (s.ssgH->ySm) + isSub (s.ssgH->ySt) + isSub (s.ssgP->ySm) + isSub (s.ssgP->ySt);
    c += isSub (s.io->outL) + isSub (s.io->outR);
    return c;
}

struct CpuResult { std::vector<double> blockNs; double sink = 0; };

static void setupScenario (Rig& r, int scenario, NoteScript& ns, double sr)
{
    auto& s = r.s();
    if (scenario >= 1 && scenario <= 4) { factoryPatch (s, scenario); ns.on = (long) (0.1 * sr); ns.off = (long) (0.6 * sr); }
    else if (scenario == 5) stressPatch (s);
}

static CpuResult runCpu (int scenario, bool ftz, double sr, int block, double seconds)
{
    FlushMode::set (ftz);
    Rig r (sr); auto& s = r.s();
    NoteScript ns; setupScenario (r, scenario, ns, sr);
    const long total = (long) (seconds * sr);
    const int nb = (int) (total / block);
    CpuResult res; res.blockNs.reserve ((size_t) nb);
    long i = 0; double acc = 0;
    for (int b = 0; b < nb; ++b)
    {
        auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < block; ++k, ++i)
        {
            ns.apply (s, i);
            s.engine.tick();
            acc += r.masterL();
        }
        auto t1 = std::chrono::steady_clock::now();
        res.blockNs.push_back ((double) std::chrono::duration_cast<std::chrono::nanoseconds> (t1 - t0).count());
    }
    res.sink = acc;
    FlushMode::set (false);
    return res;
}


// Untimed denormal trace with FTZ/DAZ off: per module, how many blocks raised the IEEE
// underflow flag inside its tick() (= produced a subnormal or flushed-to-zero tiny result),
// plus how many blocks ended with subnormal values left in jacks / filter state.
struct ScanResult { long subnormalStateBlocks = 0, maxSub = 0, underflowBlocks = 0; std::map<std::string, long> perModule; };

static ScanResult scanScenario (int scenario, double sr, int block, double seconds)
{
    FlushMode::set (false);
    Rig r (sr); auto& s = r.s();
    NoteScript ns; setupScenario (r, scenario, ns, sr);
    const int nb = (int) ((long) (seconds * sr) / block);
    ScanResult res; long i = 0;
    for (int b = 0; b < nb; ++b)
    {
        std::map<std::string, bool> hit; bool any = false;
        for (int k = 0; k < block; ++k, ++i)
        {
            ns.apply (s, i);
            for (auto& m : s.engine.mods)
            {
                std::feclearexcept (FE_UNDERFLOW);
                m->tick();
                if (std::fetestexcept (FE_UNDERFLOW)) { hit[m->id] = true; any = true; }
            }
        }
        for (auto& h : hit) ++res.perModule[h.first];
        res.underflowBlocks += any;
        long c = countSubnormals (s);
        if (c) { ++res.subnormalStateBlocks; res.maxSub = std::max (res.maxSub, c); }
    }
    return res;
}

static void cpuAll (const std::string& dir)
{
    const double sr = 48000.0; const int block = 128;
    const double seconds = gQuick ? 2.0 : 6.0; const int reps = gQuick ? 1 : 5;
    const char* scen[] = { "idle_unpatched", "first_voice", "serge_bongos", "random_dance", "sequenced_line", "stress_all_modules" };
    FILE* f = std::fopen ((dir + "/cpu.json").c_str(), "w");
    if (! f) { std::perror ("cpu.json"); return; }
    std::fprintf (f, "{\"sr\":%g,\"block\":%d,\"seconds\":%g,\"ftz_supported\":%s,\"runs\":[\n", sr, block, seconds, FlushMode::supported() ? "true" : "false");
    bool first = true; double sinkAll = 0;
    // reps interleave FTZ on/off and scenarios so slow drift on a shared machine hits all equally
    for (int rep = 0; rep < reps; ++rep)
        for (int sc = 0; sc < 6; ++sc)
            for (int ftz = 1; ftz >= 0; --ftz)
            {
                auto res = runCpu (sc, ftz == 1, sr, block, seconds);
                sinkAll += res.sink;
                std::fprintf (f, "%s{\"scenario\":\"%s\",\"ftz\":%s,\"rep\":%d,\"block_ns\":[",
                              first ? "" : ",\n", scen[sc], ftz ? "true" : "false", rep);
                for (size_t b = 0; b < res.blockNs.size(); ++b) std::fprintf (f, "%s%.0f", b ? "," : "", res.blockNs[b]);
                std::fprintf (f, "]}");
                first = false;
            }
    std::fprintf (f, "\n],\"denormal_scan\":[");
    for (int sc = 0; sc < 6; ++sc)
    {
        auto d = scanScenario (sc, sr, block, seconds);
        std::fprintf (f, "%s{\"scenario\":\"%s\",\"subnormal_state_blocks\":%ld,\"max_subnormals\":%ld,\"underflow_blocks\":%ld,\"per_module\":{",
                      sc ? ",\n" : "\n", scen[sc], d.subnormalStateBlocks, d.maxSub, d.underflowBlocks);
        bool fm = true;
        for (auto& pm : d.perModule) { std::fprintf (f, "%s\"%s\":%ld", fm ? "" : ",", pm.first.c_str(), pm.second); fm = false; }
        std::fprintf (f, "}}");
    }
    std::fprintf (f, "\n],\"sink\":%g}\n", sinkAll == 12345.678 ? 1.0 : 0.0);
    std::fclose (f);
}

// ======================================================================= main ==
int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "usage: %s render|cpu <outdir> [--quick]\n", argv[0]); return 2; }
    const std::string mode = argv[1]; gMan.dir = argv[2];
    for (int a = 3; a < argc; ++a) if (std::strcmp (argv[a], "--quick") == 0) gQuick = true;
    if (mode == "render")
    {
        renderAll();
        if (! gMan.write ("manifest.json")) { std::perror ("manifest"); return 1; }
        std::printf ("rendered %zu cases into %s\n", gMan.entries.size(), gMan.dir.c_str());
        return 0;
    }
    if (mode == "cpu") { cpuAll (gMan.dir); std::printf ("cpu timings written to %s/cpu.json\n", gMan.dir.c_str()); return 0; }
    std::fprintf (stderr, "unknown mode %s\n", mode.c_str());
    return 2;
}
