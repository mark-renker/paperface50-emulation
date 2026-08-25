#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "../engine/System.h"

class PF50Processor : public juce::AudioProcessor
{
public:
    PF50Processor();
    ~PF50Processor() override = default;

    void prepareToPlay (double sr, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& l) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override;
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- patch (cable) operations, callable from the UI thread ----
    bool addCable (int mA, int jA, int mB, int jB);
    void removeCablesAt (int m, int j);
    void removeCable (const pf50::Cable& c);
    void clearAllCables();
    std::vector<pf50::Cable> cableSnapshot();
    int cableVersion() const noexcept { return cableVer.load(); }
    void loadFactoryPatch (int index); // 0..n
    static juce::StringArray factoryPatchNames();

    // momentary panel buttons (UI thread → audio thread)
    void setMomentary (pf50::Module* m, int ctlIdx, bool down);

    pf50::System sys;
    juce::AudioProcessorValueTreeState apvts;
    juce::SpinLock engineLock;

    static juce::String paramID (const pf50::Module& m, int ctlIdx);

private:
    juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    void applyParams();
    juce::ValueTree cablesToTree();
    void cablesFromTree (const juce::ValueTree&);

    std::vector<std::atomic<float>*> paramPtrs; // aligned with module ctl order
    struct MomentaryFlag { pf50::Module* mod; int ctl; std::atomic<bool> down { false }; };
    std::vector<std::unique_ptr<MomentaryFlag>> momentaries;

    std::atomic<int> cableVer { 0 };
    int heldNote = -1; float heldVel = 0;
    juce::SortedSet<int> notesDown;
    double clockPhase = 0; bool wasPlaying = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PF50Processor)
};
