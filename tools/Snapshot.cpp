// Renders the full plugin editor to a PNG without showing a window.
// Usage: pf50_snapshot <out.png> [patchIndex]
#include "../src/plugin/PluginProcessor.h"
#include "../src/ui/MainView.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String outPath = argc > 1 ? argv[1] : "pf50_ui.png";
    const int patch = argc > 2 ? atoi (argv[2]) : 0;

    PF50Processor proc;
    proc.prepareToPlay (48000.0, 512);
    if (patch > 0) proc.loadFactoryPatch (patch);

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    auto* view = dynamic_cast<pf50ui::PF50Editor*> (ed.get());
    juce::ignoreUnused (view);
    ed->setSize (ed->getWidth(), ed->getHeight());

    // let async updates (parameter attachments) flush
    for (int i = 0; i < 5; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.25f);
    juce::File f (juce::File::getCurrentWorkingDirectory().getChildFile (outPath));
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat png;
    bool ok = os.openedOk() && png.writeImageToStream (img, os);
    std::printf ("%s %s (%dx%d)\n", ok ? "wrote" : "FAILED", f.getFullPathName().toRawUTF8(),
                 img.getWidth(), img.getHeight());
    return ok ? 0 : 1;
}
