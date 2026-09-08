// Headless integration test: loads a real LV2 instrument, routes MIDI through the
// engine and checks audio, program changes, mappings and learn.
#include "Engine.h"
#include <juce_events/juce_events.h>
#include <cstdio>
#include <cmath>

using namespace perf;
using namespace juce;

static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } else std::printf ("ok   %s\n", #cond); } while (0)

namespace
{
    constexpr int kBlock = 512;

    void pump (int ms)
    {
        MessageManager::getInstance()->runDispatchLoopUntil (ms);
    }

    /** Renders `blocks` blocks and returns the peak RMS across them. */
    float render (Engine& e, int blocks)
    {
        AudioBuffer<float> buf (2, kBlock);
        float peakRms = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            e.renderBlockForTesting (buf.getArrayOfWritePointers(), 2, kBlock);
            peakRms = jmax (peakRms, buf.getRMSLevel (0, 0, kBlock), buf.getRMSLevel (1, 0, kBlock));
        }
        return peakRms;
    }

    struct TestListener : public Engine::Listener
    {
        int learnInput = -1, learnNumber = -1; MappingDef::Source learnSource = MappingDef::Source::CC;
        int programChangedInput = -1, programChangedTo = -1;
        void learnReceived (int i, MappingDef::Source s, int n) override { learnInput = i; learnSource = s; learnNumber = n; }
        void programChanged (int i, int p) override { programChangedInput = i; programChangedTo = p; }
        void statusMessage (const String& s) override { std::printf ("     [status] %s\n", s.toRawUTF8()); }
    };
}

int main()
{
    ScopedJuceInitialiser_GUI init;

    TemporaryFile settingsTmp (".settings");
    PropertiesFile::Options opts;
    opts.applicationName = "PerformerEngineTest";
    opts.filenameSuffix = "settings";
    opts.folderName = "PerformerEngineTest";
    opts.storageFormat = PropertiesFile::storeAsXML;
    PropertiesFile settings (settingsTmp.getFile(), opts);

    PluginHost host (settings);

    // --- scan LV2 plugins and pick an instrument -------------------------------
    AudioPluginFormat* lv2 = nullptr;
    for (auto* f : host.getFormatManager().getFormats())
        if (f->getName() == "LV2") lv2 = f;
    CHECK (lv2 != nullptr);
    if (lv2 == nullptr) return 1;

    auto ids = lv2->searchPathsForPlugins (lv2->getDefaultLocationsToSearch(), true, false);
    ids.removeDuplicates (false);        // LV2 reports the bundle path once per plugin
    std::printf ("     %d LV2 bundles found\n", ids.size());
    for (auto& id : ids)
        if (id.containsIgnoreCase ("calf"))
        {
            OwnedArray<PluginDescription> found;
            host.getKnownPlugins().scanAndAddFile (id, true, found, *lv2);
        }

    PluginDescription synth;
    bool haveSynth = false;
    for (auto& d : host.getKnownPlugins().getTypes())
        if (d.isInstrument && d.name.containsIgnoreCase ("Monosynth")) { synth = d; haveSynth = true; }
    if (! haveSynth)
        for (auto& d : host.getKnownPlugins().getTypes())
            if (d.isInstrument) { synth = d; haveSynth = true; break; }
    CHECK (haveSynth);
    if (! haveSynth) { std::printf ("No LV2 instrument available (install calf-plugins)\n"); return 1; }
    std::printf ("     using instrument: %s (%s)\n", synth.name.toRawUTF8(), synth.fileOrIdentifier.toRawUTF8());

    // --- engine ------------------------------------------------------------------
    Engine engine (host, settings, /*startAudioDevice*/ false);
    TestListener listener;
    engine.addListener (&listener);
    engine.setReleaseTailSeconds (0.1);

    CHECK (engine.getSetup().inputs.size() == 2);         // Upper (ch1), Lower (ch2)
    CHECK (engine.isProgramLoaded (0, 0));

    String err;
    CHECK (engine.addSlot (0, 0, synth, err));
    if (err.isNotEmpty()) std::printf ("     addSlot error: %s\n", err.toRawUTF8());
    auto* inst = engine.getSlotInstance (0, 0, 0);
    CHECK (inst != nullptr);
    if (inst == nullptr) return 1;
    std::printf ("     instance outs=%d params=%d\n", inst->getMainBusNumOutputChannels(), inst->getParameters().size());

    // Silence before any note.
    const float silence = render (engine, 4);
    CHECK (silence < 1e-4f);

    // --- note on channel 1 -> Upper sounds -----------------------------------------
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
    const float sounding = render (engine, 20);
    std::printf ("     rms while note held: %f\n", sounding);
    CHECK (sounding > 0.01f);

    engine.injectMidi (0, MidiMessage::noteOff (1, 60));
    render (engine, 200);                                    // ~2.3 s of release
    const float afterRelease = render (engine, 4);
    std::printf ("     rms after release: %f\n", afterRelease);
    CHECK (afterRelease < sounding * 0.5f);

    // --- channel filter: ch 2 must not reach Upper (ch 1) ---------------------------
    engine.injectMidi (0, MidiMessage::noteOn (2, 60, (uint8) 100));
    CHECK (render (engine, 20) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (2, 60));

    // --- key range + transpose ------------------------------------------------------
    engine.setSlotKeyRange (0, 0, 0, 60, 72);
    engine.injectMidi (0, MidiMessage::noteOn (1, 48, (uint8) 100));   // below range
    CHECK (render (engine, 20) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 48));
    engine.setSlotKeyRange (0, 0, 0, 0, 127);

    // --- slot disabled -> silent ------------------------------------------------------
    engine.setSlotEnabled (0, 0, 0, false);
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
    CHECK (render (engine, 20) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 60));
    engine.setSlotEnabled (0, 0, 0, true);
    render (engine, 100);

    // --- CC mapping -----------------------------------------------------------------
    AudioProcessorParameter* target = nullptr;
    for (auto* p : inst->getParameters())
        if (p->isAutomatable() && ! p->isDiscrete() && p->getName (64).containsIgnoreCase ("cutoff")) { target = p; break; }
    if (target == nullptr)
        for (auto* p : inst->getParameters())
            if (p->isAutomatable() && ! p->isDiscrete()) { target = p; break; }
    CHECK (target != nullptr);
    if (target != nullptr)
    {
        std::printf ("     mapping CC 74 -> '%s' (id %s)\n", target->getName (64).toRawUTF8(), Engine::getParameterId (*target).toRawUTF8());
        MappingDef m;
        m.source = MappingDef::Source::CC; m.number = 74; m.slot = 0;
        m.paramId = Engine::getParameterId (*target);
        m.minValue = 0.25f; m.maxValue = 0.75f;
        engine.addMapping (0, 0, m);
        CHECK (engine.getSetup().inputs[0].programs[0].mappings.size() == 1);

        engine.injectMidi (0, MidiMessage::controllerEvent (1, 74, 127));
        render (engine, 2);
        std::printf ("     param after CC=127: %f\n", target->getValue());
        CHECK (std::abs (target->getValue() - 0.75f) < 0.02f);

        engine.injectMidi (0, MidiMessage::controllerEvent (1, 74, 0));
        render (engine, 2);
        std::printf ("     param after CC=0: %f\n", target->getValue());
        CHECK (std::abs (target->getValue() - 0.25f) < 0.02f);

        // A different CC is not affected by the mapping.
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 75, 127));
        render (engine, 2);
        CHECK (std::abs (target->getValue() - 0.25f) < 0.02f);
    }

    // --- learn ------------------------------------------------------------------------
    engine.setLearnArmed (true);
    engine.injectMidi (0, MidiMessage::controllerEvent (1, 11, 64));
    pump (50);
    CHECK (! engine.isLearnArmed());
    CHECK (listener.learnInput == 0 && listener.learnSource == MappingDef::Source::CC && listener.learnNumber == 11);

    // --- program change via MIDI ------------------------------------------------------
    engine.injectMidi (0, MidiMessage::programChange (1, 5));
    pump (50);
    CHECK (listener.programChangedInput == 0 && listener.programChangedTo == 5);
    CHECK (engine.getSetup().inputs[0].currentProgram == 5);
    CHECK (engine.isProgramLoaded (0, 5));
    CHECK (engine.isProgramLoaded (0, 0));                  // still releasing
    render (engine, 20);                                     // > 0.1 s tail
    pump (1600);                                             // housekeeping tick unloads it
    CHECK (! engine.isProgramLoaded (0, 0));
    // Definition of program 0 survives unloading, with captured plugin state.
    CHECK (engine.getSetup().inputs[0].programs[0].slots.size() == 1);
    CHECK (engine.getSetup().inputs[0].programs[0].slots[0].state.getSize() > 0);

    // Program 5 is empty: notes produce silence.
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
    CHECK (render (engine, 10) < 1e-4f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 60));

    // Switching back reloads program 0 from its definition and it plays again.
    engine.selectProgram (0, 0);
    CHECK (engine.isProgramLoaded (0, 0));
    CHECK (engine.getSlotInstance (0, 0, 0) != nullptr);
    CHECK (engine.getSetup().inputs[0].programs[0].mappings.size() == 1);
    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
    const float again = render (engine, 20);
    std::printf ("     rms after reload: %f\n", again);
    CHECK (again > 0.01f);
    engine.injectMidi (0, MidiMessage::noteOff (1, 60));

    // --- preload mode loads every used program ----------------------------------------
    CHECK (engine.addSlot (0, 7, synth, err));               // program 7 not loaded: def only
    CHECK (! engine.isProgramLoaded (0, 7));
    engine.setPreloadAllPrograms (true);
    pump (1600);
    CHECK (engine.isProgramLoaded (0, 7));
    CHECK (engine.getSlotInstance (0, 7, 0) != nullptr);

    // --- copy / clear / removeSlot ------------------------------------------------------
    engine.copyProgram (0, 0, 9);
    CHECK (engine.getSetup().inputs[0].programs[9].slots.size() == 1);
    CHECK (engine.getSetup().inputs[0].programs[9].mappings.size() == 1);
    engine.clearProgram (0, 9);
    CHECK (engine.getSetup().inputs[0].programs[9].isEmpty());
    engine.removeSlot (0, 0, 0);
    CHECK (engine.getSetup().inputs[0].programs[0].slots.empty());
    CHECK (engine.getSetup().inputs[0].programs[0].mappings.empty());   // mapping to removed slot gone
    CHECK (render (engine, 4) >= 0.0f);                       // still renders fine

    // --- panic + save round trip ----------------------------------------------------------
    engine.panic();
    render (engine, 2);
    TemporaryFile setupTmp (".performer.json");
    CHECK (engine.captureSetup().saveToFile (setupTmp.getFile()).wasOk());
    Setup reloaded;
    CHECK (Setup::loadFromFile (setupTmp.getFile(), reloaded).wasOk());
    CHECK (reloaded.inputs[0].programs[7].slots.size() == 1 && reloaded.inputs[0].programs[7].slots[0].plugin.name == synth.name);
    CHECK (reloaded.preloadAllPrograms);

    engine.removeListener (&listener);
    std::printf (failures == 0 ? "EngineTest: all checks passed\n" : "EngineTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
