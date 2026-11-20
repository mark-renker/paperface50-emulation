// Paperface 50 — plugin-level measurement bench (JUCE). Drives the real PF50Processor
// (processBlock, MIDI handling, parameter application, factory patches) headlessly.
//
//   pf50_pluginbench <outdir>      (built twice: PF50_FX=0 instrument, PF50_FX=1 effect)
//
// Writes plugin_<variant>.json with: reported latency/tail, measured structural latency
// (FX build), block-size invariance of every factory patch, measured release tail, and
// float32 captures of each factory patch (block 512) for a null test against the
// engine-level mirror in bench/render/Bench.cpp.
#include "../src/plugin/PluginProcessor.h"
#include <cmath>
#include <cstdio>
#include <vector>

#if PF50_FX
static const char* kVariant = "fx";
#else
static const char* kVariant = "instrument";
#endif

static constexpr double SR = 48000.0;

struct Capture { std::vector<float> L, R; };

// Render `total` samples with a given block size; one note on/off at fixed sample positions.
static Capture renderPatch (int patch, int block, int total, long noteOn, long noteOff,
                            const std::function<void(juce::AudioBuffer<float>&, long)>& fillInput = {})
{
    PF50Processor proc;
    proc.setRateAndBufferSizeDetails (SR, block);
    proc.prepareToPlay (SR, block);
    if (patch > 0) proc.loadFactoryPatch (patch);
    Capture c; c.L.reserve ((size_t) total); c.R.reserve ((size_t) total);
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    for (long pos = 0; pos < total; pos += block)
    {
        const int n = (int) std::min<long> (block, total - pos);
        buf.setSize (2, n, false, false, true);
        buf.clear();
        if (fillInput) fillInput (buf, pos);
        midi.clear();
        if (noteOn  >= pos && noteOn  < pos + n) midi.addEvent (juce::MidiMessage::noteOn  (1, 48, (juce::uint8) 102), (int) (noteOn - pos));
        if (noteOff >= pos && noteOff < pos + n) midi.addEvent (juce::MidiMessage::noteOff (1, 48), (int) (noteOff - pos));
        proc.processBlock (buf, midi);
        for (int i = 0; i < n; ++i) { c.L.push_back (buf.getSample (0, i)); c.R.push_back (buf.getSample (1, i)); }
    }
    return c;
}

static double maxAbsDiff (const Capture& a, const Capture& b)
{
    double m = 0;
    for (size_t i = 0; i < std::min (a.L.size(), b.L.size()); ++i)
        m = std::max ({ m, (double) std::fabs (a.L[i] - b.L[i]), (double) std::fabs (a.R[i] - b.R[i]) });
    return m;
}

static void writeF32 (const juce::File& f, const std::vector<float>& v)
{
    f.deleteFile();
    juce::FileOutputStream os (f);
    os.write (v.data(), v.size() * sizeof (float));
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::File out (argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]) : juce::File::getCurrentWorkingDirectory());
    out.createDirectory();

    PF50Processor probe;
    probe.prepareToPlay (SR, 512);
    const int reportedLatency = probe.getLatencySamples();
    const double reportedTail = probe.getTailLengthSeconds();

    juce::String js;
    js << "{\"variant\":\"" << kVariant << "\",\"sr\":" << SR
       << ",\"reported_latency\":" << reportedLatency << ",\"reported_tail_s\":" << reportedTail << ",\"patches\":[";

    const int total = (int) (4.0 * SR);
    const long on = (long) (0.1 * SR), off = (long) (1.5 * SR);
    const int blocks[] = { 512, 1, 32, 128, 441, 2048 };
    for (int p = 1; p <= 4; ++p)
    {
        auto ref = renderPatch (p, 512, total, on, off);
        double worst = 0; juce::String perBlock;
        for (int b : blocks)
        {
            if (b == 512) continue;
            double d = maxAbsDiff (ref, renderPatch (p, b, total, on, off));
            worst = std::max (worst, d);
            perBlock << (perBlock.isEmpty() ? "" : ",") << "\"" << b << "\":" << juce::String (d, 12);
        }
        // release tail: last sample above -120 dBFS after note-off (or after 0.5 s for free-running patches)
        long lastLoud = -1;
        for (size_t i = 0; i < ref.L.size(); ++i)
            if (std::fabs (ref.L[i]) > 1.0e-6f || std::fabs (ref.R[i]) > 1.0e-6f) lastLoud = (long) i;
        writeF32 (out.getChildFile (juce::String ("plugin_") + kVariant + "_patch" + juce::String (p) + "__L.f32"), ref.L);
        writeF32 (out.getChildFile (juce::String ("plugin_") + kVariant + "_patch" + juce::String (p) + "__R.f32"), ref.R);
        js << (p > 1 ? "," : "") << "{\"patch\":" << p << ",\"n\":" << (int) ref.L.size()
           << ",\"block_invariance_max_abs\":" << juce::String (worst, 12) << ",\"per_block\":{" << perBlock << "}"
           << ",\"last_sample_above_m120dBFS\":" << (int) lastLoud << ",\"note_off\":" << (int) off << "}";
    }
    js << "]";

#if PF50_FX
    // Measured structural latency of FX routes: impulse into the DAW input, read the DAW output.
    struct Route { const char* name; std::function<void(PF50Processor&)> wire; };
    auto idx = [] (PF50Processor& pr, pf50::Module* m) { return pr.sys.engine.indexOf (m); };
    std::vector<Route> routes {
        { "in_to_out_direct", [&] (PF50Processor& pr) { auto& s = pr.sys;
              pr.addCable (idx (pr, s.io), pf50::IOStrip::jAudioL, idx (pr, s.io), pf50::IOStrip::jMainL); } },
        { "in_gates_out", [&] (PF50Processor& pr) { auto& s = pr.sys;
              if (auto* p = pr.apvts.getParameter (PF50Processor::paramID (*s.gates, pf50::DualGates::cMode0))) p->setValueNotifyingHost (0.0f);
              if (auto* p = pr.apvts.getParameter (PF50Processor::paramID (*s.gates, pf50::DualGates::cGain0))) p->setValueNotifyingHost (1.0f);
              pr.addCable (idx (pr, s.io), pf50::IOStrip::jAudioL, idx (pr, s.gates), pf50::DualGates::jIn0);
              pr.addCable (idx (pr, s.gates), pf50::DualGates::jOut0, idx (pr, s.io), pf50::IOStrip::jMainL); } },
        { "in_vcf_balout", [&] (PF50Processor& pr) { auto& s = pr.sys;
              pr.addCable (idx (pr, s.io), pf50::IOStrip::jAudioL, idx (pr, s.vcfH), pf50::VCF1973::jBalIn); } },
    };
    js << ",\"fx_routes\":[";
    for (size_t r = 0; r < routes.size(); ++r)
    {
        PF50Processor proc;
        proc.setRateAndBufferSizeDetails (SR, 256);
        proc.prepareToPlay (SR, 256);
        routes[r].wire (proc);
        juce::AudioBuffer<float> buf (2, 256); juce::MidiBuffer midi;
        const int t0 = 100; int peakAt = -1; float peak = 0;
        for (int pos = 0; pos < 1024; pos += 256)
        {
            buf.clear();
            if (t0 >= pos && t0 < pos + 256) { buf.setSample (0, t0 - pos, 0.25f); buf.setSample (1, t0 - pos, 0.25f); }
            proc.processBlock (buf, midi);
            for (int i = 0; i < 256; ++i)
                if (std::fabs (buf.getSample (0, i)) > peak) { peak = std::fabs (buf.getSample (0, i)); peakAt = pos + i; }
        }
        js << (r ? "," : "") << "{\"route\":\"" << routes[r].name << "\",\"measured_latency\":" << (peakAt - t0)
           << ",\"peak\":" << juce::String (peak, 9) << "}";
    }
    js << "]";
#endif
    js << "}";
    auto jf = out.getChildFile (juce::String ("plugin_") + kVariant + ".json");
    jf.replaceWithText (js);
    std::printf ("wrote %s\n", jf.getFullPathName().toRawUTF8());
    return 0;
}
