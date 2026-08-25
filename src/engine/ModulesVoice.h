// Paperface 50 — voice-panel module DSP (VCO, VCO+, SSG, CV-Pro, TWS, 1973 VCF)
// Behavior per the official Paperface 50 manual v0.1; see docs/PANEL_SPEC.md.
#pragma once
#include "EngineCore.h"

namespace pf50 {

// ---------------------------------------------------------------- helpers ---
inline float expmap (float v01, float lo, float hi) { return lo * std::pow (hi / lo, std::clamp (v01, 0.0f, 1.0f)); }
inline float av (float ctl01) { return ctl01 * 2.0f - 1.0f; } // attenuverter -1..+1

struct EdgeDetector
{
    bool prev = false;
    bool rising (float v) { bool h = isHigh (v); bool e = h && ! prev; prev = h; return e; }
};

struct PulseGen // short trigger pulse, min 2 samples
{
    int n = 0;
    void fire (float sr, float ms = 1.0f) { n = std::max (2, (int) (sr * ms * 0.001f)); }
    float tick() { if (n > 0) { --n; return kCV; } return 0.0f; }
    bool active() const { return n > 0; }
};

struct DCBlock
{
    float x1 = 0, y1 = 0, R = 0.999f;
    void prepare (float sr) { R = 1.0f - 2.0f * 3.14159265f * 10.0f / sr; x1 = y1 = 0; }
    float tick (float x) { float y = x - x1 + R * y1; x1 = x; y1 = flushDenorm (y); return y1; }
};

// ------------------------------------------------------------------- VCO ----
// OSCILLATOR / OSCILLATOR+ (manual pp. 8–9). Saw + variable "sine-like" wave,
// each white (0..5) and black (±2.5). 1V/Oct, 2 CV ins with attenuverters
// (CV1 processor doubles as fine tune, ± a major third, when unpatched).
// SYNC = hard sync (harmonic-series locking). VCO+ adds PWM pulse + noise.
class VCO : public Module
{
public:
    enum J { jSineB, jSineW, jShapeCV, jSawW, jSawB, jCV1, jCV2, jV1O, jSync,
             jPulse, jVCPWM, jWhite, jPink, jSHSrc }; // last 5 only for plus version
    enum C { cFreq, cShape, cProc1, cProc2, cPW, cPWMAtt };

    explicit VCO (bool plusVersion) : plus (plusVersion)
    {
        addJack ("SINE",  JackType::Out, JackColor::Black);
        addJack ("SINE",  JackType::Out, JackColor::White);
        addJack ("WAVE SHAPE", JackType::In, JackColor::White);
        addJack ("SAW",   JackType::Out, JackColor::White);
        addJack ("SAW",   JackType::Out, JackColor::Black);
        addJack ("CV 1",  JackType::In,  JackColor::White);
        addJack ("CV 2",  JackType::In,  JackColor::White);
        addJack ("1V/O",  JackType::In,  JackColor::White);
        addJack ("SYNC",  JackType::In,  JackColor::Purple);
        addCtl ("FREQ", 0.35f); addCtl ("WAVE SHAPE", 0.0f);
        addCtl ("PROC 1", 0.5f); addCtl ("PROC 2", 0.5f);
        if (plus)
        {
            addJack ("PULSE", JackType::Out, JackColor::Red);
            addJack ("VC PWM", JackType::In, JackColor::White);
            addJack ("WHITE", JackType::Out, JackColor::Black);
            addJack ("PINK",  JackType::Out, JackColor::Black);
            addJack ("S/H SRC", JackType::Out, JackColor::White);
            addCtl ("PULSEWIDTH", 0.5f); addCtl ("VC PWM", 0.5f);
        }
        leds.resize (0);
    }

    void prepare (double sampleRate) override
    {
        Module::prepare (sampleRate);
        ph = 0; rngState = 0x9E3779B9u; // fixed seed: deterministic renders for tests/state-compare
        pink[0]=pink[1]=pink[2]=0; shLP = 2.5f; sync = {};
    }

    void tick() override
    {
        // pitch
        float oct = in (jV1O);
        oct += av (ctls[cProc1].v) * (connected (jCV1) ? in (jCV1) : (4.0f / 12.0f)); // fine ±maj 3rd
        oct += av (ctls[cProc2].v) * in (jCV2);
        float f = expmap (ctls[cFreq].v, 13.0f, 4200.0f) * std::exp2 (oct);
        f = std::clamp (f, 0.01f, sr * 0.45f);
        const float dp = f / sr;

        if (sync.rising (in (jSync))) ph = 0.0f; // hard sync

        ph += dp; if (ph >= 1.0f) ph -= 1.0f;

        // saw (polyBLEP)
        float saw = 2.0f * ph - 1.0f;
        saw -= polyblep (ph, dp);
        jacks[jSawB].value = saw * kAudio;
        jacks[jSawW].value = (saw * 0.5f + 0.5f) * kCV;

        // variable "sine-like" wave: phase-fed sine with shape-controlled
        // self-phase-modulation — near-sine at 0, brighter/asymmetric with shape.
        float shape = std::clamp (ctls[cShape].v + in (jShapeCV) / kCV, 0.0f, 1.3f);
        float s = std::sin (6.2831853f * ph + shape * 2.2f * std::sin (6.2831853f * ph));
        jacks[jSineB].value = s * kAudio;
        jacks[jSineW].value = (s * 0.5f + 0.5f) * kCV;

        if (plus)
        {
            float pw = std::clamp (ctls[cPW].v + ctls[cPWMAtt].v * in (jVCPWM) / kCV * 0.9f, 0.05f, 0.95f);
            float pulse = (ph < pw ? 1.0f : -1.0f);
            pulse += polyblep (ph, dp);
            float phw = ph - pw; if (phw < 0) phw += 1.0f;
            pulse -= polyblep (phw, dp);
            jacks[jPulse].value = (pulse * 0.5f + 0.5f) * kCV; // red logic 0..5

            float w = white();
            jacks[jWhite].value = w * kAudio;
            // Paul Kellet economy pink
            pink[0] = 0.99765f * pink[0] + w * 0.0990460f;
            pink[1] = 0.96300f * pink[1] + w * 0.2965164f;
            pink[2] = 0.57000f * pink[2] + w * 1.0526913f;
            jacks[jPink].value = std::clamp ((pink[0] + pink[1] + pink[2] + w * 0.1848f) * 0.32f, -kRails, kRails) * kAudio;
            // S/H source: bounded random walk over the full 0..5 V range —
            // wide, slow wander for the SSG random-voltage patch.
            shLP += white() * (620.0f / sr);
            if (shLP < 0.0f) shLP = -shLP;
            if (shLP > kCV)  shLP = 2.0f * kCV - shLP;
            jacks[jSHSrc].value = shLP;
        }
    }

    const bool plus;
private:
    static float polyblep (float t, float dt)
    {
        if (dt <= 0) return 0;
        if (t < dt)          { float x = t / dt;          return x + x - x * x - 1.0f; }
        if (t > 1.0f - dt)   { float x = (t - 1.0f) / dt; return x * x + x + x + 1.0f; }
        return 0.0f;
    }
    float white()
    {
        rngState = rngState * 1664525u + 1013904223u;
        return ((float) (rngState >> 8) / 8388608.0f) - 1.0f; // ±1
    }
    float ph = 0; uint32_t rngState = 1; float pink[3] {}; float shLP = 2.5f;
    EdgeDetector sync;
};

// ------------------------------------------------------------------- SSG ----
// Smooth & Stepped Generator (manual pp. 10–11).
class SSG : public Module
{
public:
    enum J { jSmIn, jSmVC, jSmCycle, jSmHold, jSmOut,
             jStIn, jStVC, jStSample, jStCycle, jStOut,
             jCoupRed, jCoupHot };
    enum C { cSmRate, cSmVC, cStRate, cStVC };

    SSG()
    {
        addJack ("IN",     JackType::In,  JackColor::White);
        addJack ("VC",     JackType::In,  JackColor::Black);  // bipolar-capable, diamond on panel
        addJack ("CYCLE",  JackType::Out, JackColor::Red);
        addJack ("HOLD",   JackType::In,  JackColor::Red);
        addJack ("SMOOTH", JackType::Out, JackColor::White);
        addJack ("IN",     JackType::In,  JackColor::White);
        addJack ("VC",     JackType::In,  JackColor::Black);
        addJack ("SAMPLE", JackType::In,  JackColor::Red);
        addJack ("CYCLE",  JackType::Out, JackColor::Red);
        addJack ("STEPPED",JackType::Out, JackColor::White);
        addJack ("COUPLER",JackType::Out, JackColor::Red);
        addJack ("COUPLER",JackType::Out, JackColor::Black);
        addCtl ("RATE", 0.5f); addCtl ("VC", 0.5f);
        addCtl ("RATE", 0.5f); addCtl ("VC", 0.5f);
        leds.resize (2);
    }
    void prepare (double s) override { Module::prepare (s); ySm = ySt = 0; cycSm = cycSt = true; }

    void tick() override
    {
        const float dt = 1.0f / sr;
        // ---- smooth ----
        {
            float rc = std::clamp (ctls[cSmRate].v + av (ctls[cSmVC].v) * in (jSmVC) / kCV, 0.0f, 1.2f);
            float rate = expmap (rc, 3.0f, 42000.0f); // V/s
            float target = in (jSmIn);
            if (! isHigh (in (jSmHold)))
            {
                float d = target - ySm;
                float step = rate * dt;
                ySm += std::clamp (d, -step, step);
            }
            if (ySm > 4.6f)  cycSm = false;
            if (ySm < 0.35f) cycSm = true;
            jacks[jSmCycle].value = cycSm ? kCV : 0.0f;
            jacks[jSmOut].value = flushDenorm (ySm);
            leds[0] = std::clamp (ySm / kCV, 0.0f, 1.0f);
        }
        // ---- stepped ----
        {
            float rc = std::clamp (ctls[cStRate].v + av (ctls[cStVC].v) * in (jStVC) / kCV, 0.0f, 1.2f);
            float rate = expmap (rc, 3.0f, 42000.0f);
            if (isHigh (in (jStSample)))
            {
                float d = in (jStIn) - ySt;
                float step = rate * dt;
                ySt += std::clamp (d, -step, step);
            }
            if (ySt > 4.6f)  cycSt = false;
            if (ySt < 0.35f) cycSt = true;
            jacks[jStCycle].value = cycSt ? kCV : 0.0f;
            jacks[jStOut].value = flushDenorm (ySt);
            leds[1] = std::clamp (ySt / kCV, 0.0f, 1.0f);
        }
        // ---- coupler ----
        bool gt = ySm > ySt;
        jacks[jCoupRed].value = gt ? kCV : 0.0f;
        jacks[jCoupHot].value = std::clamp ((ySm - ySt) * 2000.0f, -11.5f, 11.5f);
    }
    float ySm = 0, ySt = 0; bool cycSm = true, cycSt = true;
};

// ----------------------------------------------------------- Dual CV-Pro ----
// Two 3-input attenuverting mixers with DC offset and soft limiting (p. 12).
class CVPro : public Module
{
public:
    enum J { jA1, jA2, jA3, jAOut, jB1, jB2, jB3, jBOut };
    enum C { cA1, cA2, cA3, cAOff, cB1, cB2, cB3, cBOff };
    CVPro()
    {
        for (int s = 0; s < 2; ++s)
        {
            addJack ("IN 1", JackType::In, JackColor::White);
            addJack ("IN 2", JackType::In, JackColor::White);
            addJack ("IN 3", JackType::In, JackColor::White);
            addJack ("OUT",  JackType::Out, JackColor::White);
        }
        addCtl ("PROC 1", 0.5f); addCtl ("PROC 2", 0.5f); addCtl ("PROC 3", 0.5f); addCtl ("OFFSET", 0.0f);
        addCtl ("PROC 1", 0.5f); addCtl ("PROC 2", 0.5f); addCtl ("PROC 3", 0.5f); addCtl ("OFFSET", 0.0f);
        leds.resize (2);
    }
    static float softlim (float x)
    {
        float a = std::fabs (x);
        if (a <= 8.0f) return x;
        float y = 8.0f + 3.4f * std::tanh ((a - 8.0f) / 3.4f);
        return x > 0 ? y : -y;
    }
    void tick() override
    {
        for (int s = 0; s < 2; ++s)
        {
            int jb = s * 4, cb = s * 4;
            float o = av (ctls[cb+0].v) * in (jb+0) + av (ctls[cb+1].v) * in (jb+1)
                    + av (ctls[cb+2].v) * in (jb+2) + ctls[cb+3].v * kCV;
            o = softlim (o);
            jacks[jb+3].value = o;
            leds[(size_t) s] = std::clamp (std::fabs (o) / 8.0f, 0.0f, 1.0f);
        }
    }
};

// ---------------------------------------------------- Triple Waveshaper ----
// Three cascadable full-wave-rectifying folder sections (p. 13). Triangle in
// → sine-like at 2F; saw in → ~sine at 1F. VC1 drives fold depth, VC2 asymmetry.
class TWS : public Module
{
public:
    enum J { jIn0, jVC1_0, jVC2_0, jW0, jB0,
             jIn1, jVC1_1, jVC2_1, jW1, jB1,
             jIn2, jVC1_2, jVC2_2, jW2, jB2 };
    enum C { cAtt0, cAtt1, cAtt2 };
    TWS()
    {
        for (int s = 0; s < 3; ++s)
        {
            addJack ("IN",  JackType::In,  JackColor::White);
            addJack ("VC 1",JackType::In,  JackColor::White);
            addJack ("VC 2",JackType::In,  JackColor::Black);
            addJack ("OUT", JackType::Out, JackColor::White);
            addJack ("OUT", JackType::Out, JackColor::Black);
        }
        addCtl ("IN", 0.75f); addCtl ("IN", 0.75f); addCtl ("IN", 0.75f);
    }
    void prepare (double s) override { Module::prepare (s); for (auto& d : dc) d.prepare (sr); }
    void tick() override
    {
        for (int s = 0; s < 3; ++s)
        {
            const int jb = s * 5;
            float att = ctls[(size_t) s].v;
            float t = in (jb) / kAudio;                       // normalize ±1
            float d = att * (1.0f + std::clamp (in (jb+1) / kCV, 0.0f, 1.5f) * 1.6f); // VC1: drive/fold
            float a = std::clamp (in (jb+2) / kCV, -1.0f, 1.5f) * 0.8f * att;         // VC2: asymmetry
            float r = std::fabs (t + a) * d * 2.0f;
            float y01 = 0.5f - 0.5f * std::cos (3.14159265f * std::min (r, 6.0f));    // cosine folder
            jacks[jb+3].value = y01 * kCV;                    // white 0..5
            jacks[jb+4].value = dc[(size_t) s].tick ((y01 - 0.5f) * 2.0f) * kAudio;   // black, AC-coupled
        }
    }
    DCBlock dc[3];
};

// ------------------------------------------------- 1973 VCF + Balanced out ---
// TPT state-variable filter, simultaneous LP/HP/BP (12 dB/oct), common cutoff,
// manual-only Q, LOW range toggle; BAL OUT = tap to the master output bus.
class VCF1973 : public Module
{
public:
    enum J { jBalIn, jHigh, jBand, jLow, jIn, jVCFreq };
    enum C { cGain, cVCProc, cFreq, cQ, cLowRange };
    VCF1973()
    {
        addJack ("IN",      JackType::In,  JackColor::Black); // -> BAL OUT tap
        addJack ("HIGH",    JackType::Out, JackColor::Black);
        addJack ("BAND",    JackType::Out, JackColor::Black);
        addJack ("LOW",     JackType::Out, JackColor::Black);
        addJack ("IN",      JackType::In,  JackColor::Black);
        addJack ("VC FREQ", JackType::In,  JackColor::White);
        addCtl ("GAIN", 0.75f); addCtl ("PROC", 0.5f); addCtl ("FREQ", 0.5f);
        addCtl ("Q", 0.0f); addCtl ("LOW", 0.0f, 2);
    }
    void prepare (double s) override { Module::prepare (s); ic1 = ic2 = 0; }
    void tick() override
    {
        balTap = in (jBalIn);

        float f = expmap (ctls[cFreq].v, 16.0f, 8000.0f);
        if (ctls[cLowRange].v > 0.5f) f *= (1.0f / 32.0f);
        f *= std::exp2 (av (ctls[cVCProc].v) * in (jVCFreq));
        f = std::clamp (f, 0.05f, sr * 0.47f);

        float res = expmap (ctls[cQ].v, 0.5f, 22.0f);
        float g = std::tan (3.14159265f * f / sr);
        float k = 1.0f / res;
        float x = std::tanh (in (jIn) * ctls[cGain].v * 1.5f / kAudio) * kAudio;

        float a1 = 1.0f / (1.0f + g * (g + k));
        float v1 = a1 * (ic1 + g * (x - ic2));
        float v2 = ic2 + g * v1;
        ic1 = flushDenorm (2.0f * v1 - ic1);
        ic2 = flushDenorm (2.0f * v2 - ic2);
        float bp = v1, lp = v2, hp = x - k * v1 - v2;

        jacks[jLow].value  = std::clamp (lp, -kRails, kRails);
        jacks[jBand].value = std::clamp (bp, -kRails, kRails);
        jacks[jHigh].value = std::clamp (hp, -kRails, kRails);
    }
    float balTap = 0; // read by the plugin layer, mixed to master out
    float ic1 = 0, ic2 = 0;
};

} // namespace pf50
