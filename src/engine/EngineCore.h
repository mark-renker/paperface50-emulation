// Paperface 50 — patchable engine core (no JUCE dependency; headless-testable)
// Voltage-domain modular graph. WHITE = 0..5V CV, RED = 0/5V logic,
// BLACK = bipolar audio (±2.5V nominal), rails ±12V.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

namespace pf50 {

constexpr float kCV      = 5.0f;   // full-scale DC control voltage
constexpr float kAudio   = 2.5f;   // nominal audio amplitude (±)
constexpr float kRails   = 12.0f;  // system voltage rails (clamp)
constexpr float kLogicHi = 2.5f;   // threshold: a signal above this reads "high"

enum class JackType : uint8_t { In, Out, Bi };
enum class JackColor : uint8_t { White, Red, Black, Purple, Grey, TRS };

inline float clampRails (float v) { return v >  kRails ? kRails : (v < -kRails ? -kRails : v); }
inline bool  isHigh     (float v) { return v > kLogicHi; }
inline float flushDenorm(float v) { return std::fabs(v) < 1.0e-15f ? 0.0f : v; }

struct Jack
{
    const char* name = "";
    JackType    type = JackType::In;
    JackColor   color = JackColor::White;
    float       value = 0.0f;      // last output value (Out / Bi acting as out)
    bool        biDriven = false;  // Bi jacks: true when patch resolves it as an input
};

// A knob/switch/button owned by a module. The plugin layer binds these to host
// parameters; tests poke .v directly. Range is 0..1 unless noted; modules map.
struct Ctl
{
    const char* name = "";
    float v = 0.0f;      // current (possibly smoothed by plugin layer)
    float def = 0.0f;
    int   steps = 0;     // 0 = continuous; 2 = toggle; 3 = 3-pos switch...
};

class Engine;

class Module
{
public:
    virtual ~Module() = default;

    std::string id;      // stable id, e.g. "vcoP"
    std::string title;   // panel title, e.g. "OSCILLATOR+"
    std::vector<Jack> jacks;
    std::vector<Ctl>  ctls;
    std::vector<float> leds; // 0..1 brightness, written by tick, read by UI

    virtual void prepare (double sampleRate) { sr = (float) sampleRate; }
    virtual void tick() = 0; // one sample: read inputs, write jack values

    // -- helpers wired by Engine::rebuild() --------------------------------
    float in (int jackIdx) const;        // summed, rail-clamped input
    bool  connected (int jackIdx) const; // any cable on this jack?
    float biOut (int jackIdx) const;     // for Bi jacks acting as inputs

    int addJack (const char* n, JackType t, JackColor c)
    { jacks.push_back ({ n, t, c, 0.0f, false }); return (int) jacks.size() - 1; }
    int addCtl (const char* n, float def, int steps = 0)
    { ctls.push_back ({ n, def, def, steps }); return (int) ctls.size() - 1; }

    float sr = 48000.0f;

    // per-jack source pointers (outputs feeding this jack), rebuilt on patch change
    struct Feed { std::vector<const float*> src; };
    std::vector<Feed> feeds;
};

inline float Module::in (int j) const
{
    const auto& f = feeds[(size_t) j];
    float s = 0.0f;
    for (auto* p : f.src) s += *p;
    return clampRails (s);
}
inline bool Module::connected (int j) const { return ! feeds[(size_t) j].src.empty(); }
inline float Module::biOut (int j) const { return in (j); }

struct Cable
{
    int modA = 0, jackA = 0, modB = 0, jackB = 0;
    bool operator== (const Cable& o) const
    { return (modA==o.modA && jackA==o.jackA && modB==o.modB && jackB==o.jackB)
          || (modA==o.modB && jackA==o.jackB && modB==o.modA && jackB==o.jackA); }
};

class Engine
{
public:
    std::vector<std::unique_ptr<Module>> mods;
    std::vector<Cable> cables;

    template <typename T, typename... A>
    T* add (const char* id, const char* title, A&&... a)
    {
        auto m = std::make_unique<T> (std::forward<A> (a)...);
        m->id = id; m->title = title;
        auto* raw = m.get();
        mods.push_back (std::move (m));
        return raw;
    }

    Module* byId (const std::string& id)
    {
        for (auto& m : mods) if (m->id == id) return m.get();
        return nullptr;
    }
    int indexOf (const Module* m) const
    {
        for (size_t i = 0; i < mods.size(); ++i) if (mods[i].get() == m) return (int) i;
        return -1;
    }

    void prepare (double sampleRate)
    {
        for (auto& m : mods) { m->prepare (sampleRate); }
        rebuild();
    }

    bool connect (int mA, int jA, int mB, int jB)
    {
        if (mA < 0 || mB < 0 || mA >= (int) mods.size() || mB >= (int) mods.size()) return false;
        if (jA < 0 || jB < 0 || jA >= (int) mods[mA]->jacks.size() || jB >= (int) mods[mB]->jacks.size()) return false;
        if (mA == mB && jA == jB) return false;
        Cable c { mA, jA, mB, jB };
        for (auto& e : cables) if (e == c) return false;
        cables.push_back (c);
        rebuild();
        return true;
    }
    void disconnectAt (int m, int j)
    {
        cables.erase (std::remove_if (cables.begin(), cables.end(),
            [&] (const Cable& c) { return (c.modA==m && c.jackA==j) || (c.modB==m && c.jackB==j); }),
            cables.end());
        rebuild();
    }
    void removeCable (const Cable& c)
    {
        cables.erase (std::remove (cables.begin(), cables.end(), c), cables.end());
        rebuild();
    }
    void clearCables() { cables.clear(); rebuild(); }

    // Resolve feeds. Direction: Out -> In. Bi jacks: a Bi jack is *driven*
    // (acts as input) when connected to any Out jack or to a Bi jack that is
    // itself fed by an Out elsewhere; otherwise it acts as an output.
    void rebuild()
    {
        for (auto& m : mods)
        {
            m->feeds.assign (m->jacks.size(), {});
            for (auto& j : m->jacks) j.biDriven = false;
        }
        // pass 1: mark Bi jacks connected to a pure Out as driven
        auto jackOf = [&] (int mi, int ji) -> Jack& { return mods[(size_t) mi]->jacks[(size_t) ji]; };
        for (auto& c : cables)
        {
            auto &a = jackOf (c.modA, c.jackA), &b = jackOf (c.modB, c.jackB);
            if (a.type == JackType::Bi && b.type == JackType::Out) a.biDriven = true;
            if (b.type == JackType::Bi && a.type == JackType::Out) b.biDriven = true;
        }
        // pass 2 (one relaxation): Bi fed by an undriven Bi that has an Out feed
        for (auto& c : cables)
        {
            auto &a = jackOf (c.modA, c.jackA), &b = jackOf (c.modB, c.jackB);
            if (a.type == JackType::Bi && b.type == JackType::Bi)
            {
                if (b.biDriven && ! a.biDriven) { /* b is input; a stays source */ }
            }
        }
        // wire feeds: for each cable, source = Out (or un-driven Bi), dest = In (or driven Bi)
        for (auto& c : cables)
        {
            auto &a = jackOf (c.modA, c.jackA), &b = jackOf (c.modB, c.jackB);
            auto isSource = [] (const Jack& j) { return j.type == JackType::Out || (j.type == JackType::Bi && ! j.biDriven); };
            auto isDest   = [] (const Jack& j) { return j.type == JackType::In  || (j.type == JackType::Bi &&   j.biDriven); };
            if (isSource (a) && isDest (b))
                mods[(size_t) c.modB]->feeds[(size_t) c.jackB].src.push_back (&a.value);
            if (isSource (b) && isDest (a))
                mods[(size_t) c.modA]->feeds[(size_t) c.jackA].src.push_back (&b.value);
            // Bi-to-Bi with neither driven: pass both ways (banana wire),
            // letting the router modules sort out signal direction.
            if (a.type == JackType::Bi && b.type == JackType::Bi && ! a.biDriven && ! b.biDriven)
            {
                mods[(size_t) c.modB]->feeds[(size_t) c.jackB].src.push_back (&a.value);
                mods[(size_t) c.modA]->feeds[(size_t) c.jackA].src.push_back (&b.value);
            }
        }
    }

    void tick() { for (auto& m : mods) m->tick(); }
};

} // namespace pf50
