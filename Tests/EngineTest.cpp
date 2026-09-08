// Headless integration test: loads a real LV2 instrument, routes MIDI through the
// engine and checks audio, program changes, mappings and learn.
#include "Engine.h"
#include "MappingSuggestions.h"
#include <juce_events/juce_events.h>
#include <cstdio>
#include <cmath>
#include <set>

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

    // --- effect chains ----------------------------------------------------------------------
    {
        // Pick a Calf effect with a controllable input level; fall back to any effect.
        PluginDescription fx; bool haveFx = false;
        for (auto& d : host.getKnownPlugins().getTypes())
            if (! d.isInstrument && d.name.containsIgnoreCase ("Stereo Tools")) { fx = d; haveFx = true; }
        if (! haveFx)
            for (auto& d : host.getKnownPlugins().getTypes())
                if (! d.isInstrument && d.name.containsIgnoreCase ("Compressor")) { fx = d; haveFx = true; break; }
        CHECK (haveFx);
        if (haveFx)
        {
            std::printf ("     using effect: %s\n", fx.name.toRawUTF8());
            engine.setPreloadAllPrograms (false);
            engine.selectProgram (0, 0);
            CHECK (engine.addSlot (0, 0, synth, err));
            auto* synthInst = engine.getSlotInstance (0, 0, 0);
            CHECK (synthInst != nullptr);

            // Baseline level, instrument only.
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            render (engine, 10);
            const float dry = render (engine, 10);
            std::printf ("     dry rms: %f\n", dry);
            CHECK (dry > 0.01f);

            // Slot insert chain: audio must still pass with the effect in place.
            CHECK (engine.addEffect (0, 0, 0, fx, err));
            if (err.isNotEmpty()) std::printf ("     addEffect error: %s\n", err.toRawUTF8());
            CHECK (engine.getSetup().inputs[0].programs[0].slots[0].effects.size() == 1);
            auto* fxInst = engine.getPluginInstance (0, 0, 0, 0);
            CHECK (fxInst != nullptr);
            render (engine, 5);
            const float wet = render (engine, 10);
            std::printf ("     rms through slot effect: %f\n", wet);
            CHECK (wet > 0.01f);

            // Find a level/gain parameter and turn it down: output must drop.
            AudioProcessorParameter* level = nullptr;
            if (fxInst != nullptr)
                for (auto* p : fxInst->getParameters())
                {
                    const auto n = p->getName (64);
                    if (n.containsIgnoreCase ("Level In") || n.containsIgnoreCase ("Input Gain") || n.containsIgnoreCase ("Input Level")) { level = p; break; }
                }
            if (level == nullptr && fxInst != nullptr)
                for (auto* p : fxInst->getParameters())
                    if (p->getName (64).containsIgnoreCase ("Level Out") || p->getName (64).containsIgnoreCase ("Output")) { level = p; break; }
            CHECK (level != nullptr);
            if (level != nullptr)
            {
                std::printf ("     effect level parameter: '%s'\n", level->getName (64).toRawUTF8());
                const String levelId = Engine::getParameterId (*level);

                // Via a mapping to the effect (slot 0, effect 0): CC 7 -> level, min 0.
                MappingDef m;
                m.source = MappingDef::Source::CC; m.number = 7; m.slot = 0; m.effect = 0;
                m.paramId = levelId; m.minValue = 0.0f; m.maxValue = 1.0f;
                engine.addMapping (0, 0, m);
                engine.injectMidi (0, MidiMessage::controllerEvent (1, 7, 0));
                render (engine, 5);
                const float muted = render (engine, 10);
                std::printf ("     rms with effect level at min: %f\n", muted);
                CHECK (muted < wet * 0.2f);

                // Bypass restores the dry signal even though the level is still down.
                engine.setEffectBypassed (0, 0, 0, 0, true);
                CHECK (engine.getSetup().inputs[0].programs[0].slots[0].effects[0].bypassed);
                render (engine, 5);
                const float bypassed = render (engine, 10);
                std::printf ("     rms with effect bypassed: %f\n", bypassed);
                CHECK (bypassed > 0.01f);
                engine.setEffectBypassed (0, 0, 0, 0, false);

                // Removing the effect drops its mapping and leaves the instrument alone.
                engine.removeEffect (0, 0, 0, 0);
                level = nullptr;   // instance destroyed with the effect
                CHECK (engine.getSetup().inputs[0].programs[0].slots[0].effects.empty());
                CHECK (engine.getSetup().inputs[0].programs[0].mappings.empty());
                render (engine, 5);
                CHECK (render (engine, 10) > 0.01f);

                // Program-level chain: same effect after the mix, mapped via slot == -1.
                CHECK (engine.addEffect (0, 0, -1, fx, err));
                CHECK (engine.getSetup().inputs[0].programs[0].effects.size() == 1);
                auto* progFx = engine.getPluginInstance (0, 0, -1, 0);
                CHECK (progFx != nullptr);
                if (progFx != nullptr)
                {
                    auto* pLevel = Engine::findParameter (*progFx, levelId);
                    CHECK (pLevel != nullptr);
                    const float unity = pLevel != nullptr ? pLevel->getValue() : 0.0f;
                    MappingDef pm = m; pm.slot = -1; pm.effect = 0; pm.number = 8;
                    engine.addMapping (0, 0, pm);
                    render (engine, 5);
                    CHECK (render (engine, 10) > 0.01f);
                    engine.injectMidi (0, MidiMessage::controllerEvent (1, 8, 0));
                    render (engine, 5);
                    const float progMuted = render (engine, 10);
                    std::printf ("     rms with program effect level at min: %f\n", progMuted);
                    CHECK (progMuted < wet * 0.2f);
                    if (pLevel != nullptr) pLevel->setValueNotifyingHost (unity);   // back to unity gain
                    render (engine, 2);
                }

                // Reordering: add a second program effect, move it first, mapping follows.
                CHECK (engine.addEffect (0, 0, -1, fx, err));
                engine.moveEffect (0, 0, -1, 1, 0);
                CHECK (engine.getSetup().inputs[0].programs[0].mappings.size() == 1);
                CHECK (engine.getSetup().inputs[0].programs[0].mappings[0].effect == 1);   // the mapped one moved to index 1
                engine.removeEffect (0, 0, -1, 0);
                CHECK (engine.getSetup().inputs[0].programs[0].mappings[0].effect == 0);
            }

            engine.injectMidi (0, MidiMessage::noteOff (1, 60));
            render (engine, 20);

            // Effects survive a save/load round trip, including on the program chain.
            TemporaryFile fxTmp (".performer.json");
            CHECK (engine.captureSetup().saveToFile (fxTmp.getFile()).wasOk());
            Setup rl;
            CHECK (Setup::loadFromFile (fxTmp.getFile(), rl).wasOk());
            CHECK (rl.inputs[0].programs[0].effects.size() == 1);
            CHECK (rl.inputs[0].programs[0].effects[0].plugin.name == fx.name);
            CHECK (rl.inputs[0].programs[0].mappings.size() == 1 && rl.inputs[0].programs[0].mappings[0].slot == -1);

            // Loading that setup rebuilds the chain live.
            engine.loadSetup (rl);
            CHECK (engine.getPluginInstance (0, 0, -1, 0) != nullptr);
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            render (engine, 10);
            const float afterLoad = render (engine, 10);
            std::printf ("     rms after reloading setup with chain: %f\n", afterLoad);
            CHECK (afterLoad > 0.01f);
            CHECK (afterLoad < dry * 1.5f);   // effect state (unity gain) survived the round trip
            engine.injectMidi (0, MidiMessage::noteOff (1, 60));
            render (engine, 20);
        }
    }

    // --- mapping suggestions ------------------------------------------------------------------
    {
        engine.setPreloadAllPrograms (false);
        engine.selectProgram (0, 30);
        CHECK (engine.addSlot (0, 30, synth, err));
        auto* mono = engine.getSlotInstance (0, 30, 0);
        CHECK (mono != nullptr);
        if (mono != nullptr)
        {
            auto byNames = suggestMappingsFromNames (*mono);
            std::printf ("     %d name-based suggestions for %s:\n", (int) byNames.size(), synth.name.toRawUTF8());
            for (auto& sg : byNames)
                std::printf ("       %-12s -> %-28s (%s)\n", sg.mapping.sourceDescription().toRawUTF8(), sg.mapping.paramName.toRawUTF8(), sg.reason.toRawUTF8());

            auto find = [&] (int cc) -> const MappingSuggestion* { for (auto& sg : byNames) if (sg.mapping.number == cc) return &sg; return nullptr; };
            CHECK (find (74) != nullptr && find (74)->mapping.paramName.containsIgnoreCase ("cutoff"));
            CHECK (find (71) != nullptr && find (71)->mapping.paramName.containsIgnoreCase ("reso"));
            CHECK (find (73) != nullptr && find (73)->mapping.paramName.containsIgnoreCase ("attack"));
            CHECK (find (72) != nullptr && find (72)->mapping.paramName.containsIgnoreCase ("release"));
            CHECK (find (5)  != nullptr && find (5)->mapping.paramName.containsIgnoreCase ("portamento"));
            CHECK (find (1) == nullptr);                          // mod wheel is left to the plugin

            // No CC and no parameter is used twice.
            std::set<int> ccs; std::set<String> ids;
            for (auto& sg : byNames) { ccs.insert (sg.mapping.number); ids.insert (sg.mapping.paramId); }
            CHECK (ccs.size() == byNames.size() && ids.size() == byNames.size());

            // Full pipeline without a template: same set, targeted at slot 0, skipping existing ones.
            auto& templates = host.getMappingTemplates();
            CHECK (! templates.has (synth));
            std::vector<MappingDef> existing;
            existing.push_back (find (74)->mapping); existing.back().slot = 0; existing.back().effect = -1;
            auto pipeline = suggestMappings (*mono, synth, templates, 0, -1, existing);
            CHECK (pipeline.size() == byNames.size() - 1);
            for (auto& sg : pipeline) { CHECK (sg.mapping.slot == 0 && sg.mapping.effect == -1); CHECK (sg.mapping.number != 74); }

            // Template: save two mappings, one of them for a parameter that doesn't exist.
            std::vector<MappingDef> tmpl;
            MappingDef a; a.number = 20; a.paramId = find (74)->mapping.paramId; a.minValue = 0.1f; a.maxValue = 0.9f;
            MappingDef b; b.number = 21; b.paramId = "sym:does_not_exist";
            tmpl.push_back (a); tmpl.push_back (b);
            templates.set (synth, tmpl);
            CHECK (templates.has (synth));
            auto fromTemplate = suggestMappings (*mono, synth, templates, 2, 1, {});
            CHECK (fromTemplate.size() == 1);
            if (fromTemplate.size() == 1)
            {
                CHECK (fromTemplate[0].mapping.number == 20 && fromTemplate[0].mapping.slot == 2 && fromTemplate[0].mapping.effect == 1);
                CHECK (std::abs (fromTemplate[0].mapping.minValue - 0.1f) < 1e-6f);
                CHECK (fromTemplate[0].mapping.paramName.containsIgnoreCase ("cutoff"));
            }
            // Persisted: a fresh instance reads the same file.
            MappingTemplates reloaded (settings.getFile().getSiblingFile ("mapping-templates.json"));
            CHECK (reloaded.has (synth) && reloaded.get (synth).size() == 2);
            templates.remove (synth);
            CHECK (! templates.has (synth));
        }
        engine.clearProgram (0, 30);
    }

    // --- optional: write a demo setup for eyeballing the UI ----------------------------------
    if (auto outPath = SystemStats::getEnvironmentVariable ("PERFORMER_TEST_WRITE_SETUP", {}); outPath.isNotEmpty())
    {
        PluginDescription fx; bool haveFx = false;
        for (auto& d : host.getKnownPlugins().getTypes())
            if (! d.isInstrument && (d.name.containsIgnoreCase ("Stereo Tools") || d.name.containsIgnoreCase ("Reverb"))) { fx = d; haveFx = true; }
        Setup demo = Setup::makeDefault();
        auto& prog = demo.inputs[0].programs[0];
        prog.name = "Demo Lead";
        SlotDef sd; sd.plugin = synth; sd.gainDb = -3.0f; sd.lowKey = 48;
        if (haveFx) { EffectDef e; e.plugin = fx; sd.effects.push_back (e); e.bypassed = true; sd.effects.push_back (e); }
        prog.slots.push_back (sd);
        SlotDef pad = sd; pad.effects.clear(); pad.transpose = -12; pad.highKey = 47; pad.lowKey = 0;
        prog.slots.push_back (pad);
        if (haveFx) { EffectDef e; e.plugin = fx; prog.effects.push_back (e); }
        MappingDef mm; mm.number = 74; mm.slot = 0; mm.effect = -1; mm.paramId = "sym:cutoff"; mm.paramName = "Cutoff";
        prog.mappings.push_back (mm);
        if (haveFx) { MappingDef fm; fm.number = 7; fm.slot = -1; fm.effect = 0; fm.paramId = "sym:level_in"; fm.paramName = "Input Gain"; prog.mappings.push_back (fm); }
        demo.inputs[0].programs[3].name = "Organ split";
        CHECK (demo.saveToFile (File (outPath)).wasOk());
        std::printf ("     demo setup written to %s\n", outPath.toRawUTF8());
    }

    // --- optional: an arbitrary VST3 (e.g. a yabridge-bridged plugin) --------------------
    // PERFORMER_TEST_VST3=/path/to/Plugin.vst3 loads it, plays a note and renders blocks;
    // this reproduces the multi-out / inactive-bus crash seen with Kontakt via yabridge.
    if (auto vst3Path = SystemStats::getEnvironmentVariable ("PERFORMER_TEST_VST3", {}); vst3Path.isNotEmpty())
    {
        AudioPluginFormat* vst3 = nullptr;
        for (auto* f : host.getFormatManager().getFormats())
            if (f->getName() == "VST3") vst3 = f;
        CHECK (vst3 != nullptr);
        if (vst3 != nullptr)
        {
            OwnedArray<PluginDescription> found;
            host.getKnownPlugins().scanAndAddFile (vst3Path, true, found, *vst3);
            CHECK (found.size() > 0);
            if (found.size() > 0)
            {
                const PluginDescription desc (*found[0]);
                std::printf ("     extra VST3: %s (%s)\n", desc.name.toRawUTF8(), desc.manufacturerName.toRawUTF8());
                engine.setPreloadAllPrograms (false);
                engine.selectProgram (0, 20);
                CHECK (engine.addSlot (0, 20, desc, err));
                if (err.isNotEmpty()) std::printf ("     addSlot error: %s\n", err.toRawUTF8());
                if (auto* x = engine.getSlotInstance (0, 20, 0))
                {
                    int busChannels = 0;
                    for (int b = 0; b < x->getBusCount (false); ++b)
                        busChannels += x->getBus (false, b)->getNumberOfChannels();
                    std::printf ("     output buses=%d  total out channels=%d (enabled %d)  main=%d\n",
                                 x->getBusCount (false), busChannels, x->getTotalNumOutputChannels(), x->getMainBusNumOutputChannels());
                    CHECK (x->getTotalNumOutputChannels() == busChannels);   // every bus active
                    engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
                    const float r = render (engine, 100);                      // ~1.2 s; would segfault before the fix
                    std::printf ("     rms with extra VST3 (may be 0 if no preset loaded): %f\n", r);
                    engine.injectMidi (0, MidiMessage::noteOff (1, 60));
                    render (engine, 20);
                    CHECK (true);   // survived processing
                }
            }
        }
    }

    engine.removeListener (&listener);
    std::printf (failures == 0 ? "EngineTest: all checks passed\n" : "EngineTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
