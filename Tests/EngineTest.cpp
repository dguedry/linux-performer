// Headless integration test: loads a real LV2 instrument, routes MIDI through the
// engine and checks audio, program changes, mappings and learn.
#include "Engine.h"
#include "PluginIcons.h"
#include "MappingSuggestions.h"
#include <juce_events/juce_events.h>
#include <cstdio>
#include <cmath>
#include <set>
#include <signal.h>
#include <unistd.h>

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

    // Own scratch folder: PluginHost keeps its crash list ("dead man's pedal") next to
    // the settings file, and a plain temp file would share /tmp with every other run,
    // so an aborted run could blacklist a plugin for the next one.
    const File scratchDir = File::getSpecialLocation (File::tempDirectory)
                                .getChildFile ("PerformerEngineTest-" + String (Time::currentTimeMillis()));
    scratchDir.createDirectory();
    struct ScratchCleanup { File d; ~ScratchCleanup() { d.deleteRecursively(); } } scratchCleanup { scratchDir };
    TemporaryFile settingsTmp (scratchDir.getChildFile ("test.settings"));
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

    // Plugins load on a background thread; wait for them (pumping the message loop so
    // the loaded processes get attached) before checking anything that needs them.
    auto settle = [&engine]
    {
        const auto t0 = Time::getMillisecondCounterHiRes();
        pump (30);
        while (engine.hasPendingLoads() && Time::getMillisecondCounterHiRes() - t0 < 120000.0) pump (30);
    };

    CHECK (engine.getSetup().inputs.size() == 2);         // Upper (ch1), Lower (ch2)
    CHECK (engine.isProgramLoaded (0, 0));

    CHECK (RemotePlugin::findHostExecutable().existsAsFile());
    std::printf ("     plugin host: %s\n", RemotePlugin::findHostExecutable().getFullPathName().toRawUTF8());

    String err;
    CHECK (engine.addSlot (0, 0, synth, err));
    settle();
    if (err.isNotEmpty()) std::printf ("     addSlot error: %s\n", err.toRawUTF8());
    auto* inst = engine.getPlugin (0, 0, 0);
    CHECK (inst != nullptr && inst->isAlive());
    if (inst == nullptr) return 1;
    std::printf ("     plugin '%s' pid-hosted, params=%d\n", inst->getName().toRawUTF8(), (int) inst->getParameters().size());

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
    const ParamInfo* target = nullptr;
    for (auto& p : inst->getParameters())
        if (p.automatable && ! p.discrete && p.name.containsIgnoreCase ("cutoff")) { target = &p; break; }
    if (target == nullptr)
        for (auto& p : inst->getParameters())
            if (p.automatable && ! p.discrete) { target = &p; break; }
    CHECK (target != nullptr);
    if (target != nullptr)
    {
        std::printf ("     mapping CC 74 -> '%s' (id %s)\n", target->name.toRawUTF8(), target->id.toRawUTF8());
        MappingDef m;
        m.source = MappingDef::Source::CC; m.number = 74; m.slot = 0;
        m.paramId = target->id;
        m.minValue = 0.25f; m.maxValue = 0.75f;
        engine.addMapping (0, 0, m);
        CHECK (engine.getSetup().inputs[0].programs[0].mappings.size() == 1);

        // Values are read back from the plugin process, not from the host's cache.
        float v = -1.0f;
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 74, 127));
        render (engine, 2);
        CHECK (inst->fetchParameterValue (target->index, v));
        std::printf ("     param after CC=127: %f\n", v);
        CHECK (std::abs (v - 0.75f) < 0.02f);

        engine.injectMidi (0, MidiMessage::controllerEvent (1, 74, 0));
        render (engine, 2);
        CHECK (inst->fetchParameterValue (target->index, v));
        std::printf ("     param after CC=0: %f\n", v);
        CHECK (std::abs (v - 0.25f) < 0.02f);

        // A different CC is not affected by the mapping.
        engine.injectMidi (0, MidiMessage::controllerEvent (1, 75, 127));
        render (engine, 2);
        CHECK (inst->fetchParameterValue (target->index, v));
        CHECK (std::abs (v - 0.25f) < 0.02f);
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
    settle();
    CHECK (engine.isProgramLoaded (0, 0));
    CHECK (engine.isPluginAlive (0, 0, 0));
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
    settle();
    CHECK (engine.isProgramLoaded (0, 7));
    CHECK (engine.isPluginAlive (0, 7, 0));

    // --- copy / clear / removeSlot ------------------------------------------------------
    engine.copyProgram (0, 0, 9);
    settle();
    CHECK (engine.getSetup().inputs[0].programs[9].slots.size() == 1);
    CHECK (engine.getSetup().inputs[0].programs[9].mappings.size() == 1);
    engine.clearProgram (0, 9);
    settle();
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
            settle();
            CHECK (engine.addSlot (0, 0, synth, err));
            settle();
            CHECK (engine.isPluginAlive (0, 0, 0));

            // Baseline level, instrument only.
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            render (engine, 10);
            const float dry = render (engine, 10);
            std::printf ("     dry rms: %f\n", dry);
            CHECK (dry > 0.01f);

            // Slot insert chain: audio must still pass with the effect in place.
            CHECK (engine.addEffect (0, 0, 0, fx, err));
            settle();
            if (err.isNotEmpty()) std::printf ("     addEffect error: %s\n", err.toRawUTF8());
            CHECK (engine.getSetup().inputs[0].programs[0].slots[0].effects.size() == 1);
            auto* fxInst = engine.getPlugin (0, 0, 0, 0);
            CHECK (fxInst != nullptr && fxInst->isAlive());
            render (engine, 5);
            const float wet = render (engine, 10);
            std::printf ("     rms through slot effect: %f\n", wet);
            CHECK (wet > 0.01f);

            // Find a level/gain parameter and turn it down: output must drop.
            const ParamInfo* level = nullptr;
            if (fxInst != nullptr)
                for (auto& p : fxInst->getParameters())
                {
                    const auto n = p.name;
                    if (n.containsIgnoreCase ("Level In") || n.containsIgnoreCase ("Input Gain") || n.containsIgnoreCase ("Input Level")) { level = &p; break; }
                }
            if (level == nullptr && fxInst != nullptr)
                for (auto& p : fxInst->getParameters())
                    if (p.name.containsIgnoreCase ("Level Out") || p.name.containsIgnoreCase ("Output")) { level = &p; break; }
            CHECK (level != nullptr);
            if (level != nullptr)
            {
                std::printf ("     effect level parameter: '%s'\n", level->name.toRawUTF8());
                const String levelId = level->id;

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
                level = nullptr;   // its process is gone with the effect
                CHECK (engine.getSetup().inputs[0].programs[0].slots[0].effects.empty());
                CHECK (engine.getSetup().inputs[0].programs[0].mappings.empty());
                render (engine, 5);
                CHECK (render (engine, 10) > 0.01f);

                // Program-level chain: same effect after the mix, mapped via slot == -1.
                CHECK (engine.addEffect (0, 0, -1, fx, err));
                settle();
                CHECK (engine.getSetup().inputs[0].programs[0].effects.size() == 1);
                auto* progFx = engine.getPlugin (0, 0, -1, 0);
                CHECK (progFx != nullptr && progFx->isAlive());
                if (progFx != nullptr)
                {
                    const int pLevel = progFx->findParameterIndex (levelId);
                    CHECK (pLevel >= 0);
                    float unity = 0.0f;
                    if (pLevel >= 0) progFx->fetchParameterValue (pLevel, unity);
                    MappingDef pm = m; pm.slot = -1; pm.effect = 0; pm.number = 8;
                    engine.addMapping (0, 0, pm);
                    render (engine, 5);
                    CHECK (render (engine, 10) > 0.01f);
                    engine.injectMidi (0, MidiMessage::controllerEvent (1, 8, 0));
                    render (engine, 5);
                    const float progMuted = render (engine, 10);
                    std::printf ("     rms with program effect level at min: %f\n", progMuted);
                    CHECK (progMuted < wet * 0.2f);
                    if (pLevel >= 0) progFx->setParameterValue (pLevel, unity);   // back to unity gain
                    render (engine, 2);
                }

                // Reordering: add a second program effect, move it first, mapping follows.
                CHECK (engine.addEffect (0, 0, -1, fx, err));
                settle();
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
            settle();
            CHECK (engine.isPluginAlive (0, 0, -1, 0));
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
        settle();
        CHECK (engine.addSlot (0, 30, synth, err));
        settle();
        auto* mono = engine.getPlugin (0, 30, 0);
        CHECK (mono != nullptr && mono->isAlive());
        if (mono != nullptr)
        {
            auto byNames = suggestMappingsFromNames (mono->getParameters());
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
            auto pipeline = suggestMappings (mono->getParameters(), synth, templates, 0, -1, existing);
            CHECK (pipeline.size() == byNames.size() - 1);
            for (auto& sg : pipeline) { CHECK (sg.mapping.slot == 0 && sg.mapping.effect == -1); CHECK (sg.mapping.number != 74); }

            // Template: save two mappings, one of them for a parameter that doesn't exist.
            std::vector<MappingDef> tmpl;
            MappingDef a; a.number = 20; a.paramId = find (74)->mapping.paramId; a.minValue = 0.1f; a.maxValue = 0.9f;
            MappingDef b; b.number = 21; b.paramId = "sym:does_not_exist";
            tmpl.push_back (a); tmpl.push_back (b);
            templates.set (synth, tmpl);
            CHECK (templates.has (synth));
            auto fromTemplate = suggestMappings (mono->getParameters(), synth, templates, 2, 1, {});
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
        settle();
    }

    // --- helper environment -----------------------------------------------------------------
    {
        std::vector<char*> envp;
        auto env = RemotePlugin::buildHelperEnvironment (envp);
        CHECK (envp.size() == env.size() + 1 && envp.back() == nullptr);
        String path;
        int pathEntries = 0;
        for (auto& e : env) if (String (e).startsWith ("PATH=")) { path = String (e).substring (5); ++pathEntries; }
        CHECK (pathEntries == 1);
        const auto localBin = File::getSpecialLocation (File::userHomeDirectory).getChildFile (".local/bin");
        if (localBin.getChildFile ("wine").existsAsFile())
        {
            std::printf ("     ~/.local/bin/wine shim present: PATH starts with %s\n", StringArray::fromTokens (path, ":", {})[0].toRawUTF8());
            CHECK (StringArray::fromTokens (path, ":", {})[0] == localBin.getFullPathName());
        }
        for (auto& e : env) if (String (e).startsWith ("WINELOADER=")) std::printf ("     %s\n", e.c_str());
    }

    // --- scanner robustness -----------------------------------------------------------------
    {
        using HR = PluginHost::HelperResult;
        // A helper that never finishes must be killed at the timeout, not waited for.
        String out;
        auto t0 = Time::getMillisecondCounterHiRes();
        const auto r1 = PluginHost::runHelperWithTimeout (File ("/bin/sleep"), StringArray { "30" }, 600, 100, [] { return false; }, out);
        const auto ms1 = Time::getMillisecondCounterHiRes() - t0;
        std::printf ("     hung helper: result %d after %.0f ms\n", (int) r1, ms1);
        CHECK (r1 == HR::timedOut && ms1 < 2000.0);

        // A stop request (dialog closing) gives the child a grace period, then kills it.
        t0 = Time::getMillisecondCounterHiRes();
        const auto r2 = PluginHost::runHelperWithTimeout (File ("/bin/sleep"), StringArray { "30" }, 60000, 300,
                                                          [t0] { return Time::getMillisecondCounterHiRes() - t0 > 250.0; }, out);
        const auto ms2 = Time::getMillisecondCounterHiRes() - t0;
        std::printf ("     stopped helper: result %d after %.0f ms\n", (int) r2, ms2);
        CHECK (r2 == HR::stopped && ms2 > 500.0 && ms2 < 2000.0);

        // A stop request while the child is about to finish still yields its output.
        t0 = Time::getMillisecondCounterHiRes();
        const auto r4 = PluginHost::runHelperWithTimeout (File ("/bin/sh"), StringArray { "-c", "sleep 0.5; echo late-result" }, 60000, 5000,
                                                          [] { return true; }, out);
        CHECK (r4 == HR::finished && out.trim() == "late-result");

        // Output is collected completely from a process that finishes.
        const auto r3 = PluginHost::runHelperWithTimeout (File ("/bin/echo"), StringArray { "hello", "scanner" }, 5000, 100, [] { return false; }, out);
        CHECK (r3 == HR::finished && out.trim() == "hello scanner");

        // A hard stop (application exiting) ignores the grace period.
        t0 = Time::getMillisecondCounterHiRes();
        const auto r5 = PluginHost::runHelperWithTimeout (File ("/bin/sleep"), StringArray { "30" }, 60000, 45000, [] { return false; }, out,
                                                          [t0] { return Time::getMillisecondCounterHiRes() - t0 > 200.0; });
        const auto ms5 = Time::getMillisecondCounterHiRes() - t0;
        std::printf ("     hard-stopped helper: result %d after %.0f ms\n", (int) r5, ms5);
        CHECK (r5 == HR::stopped && ms5 < 1500.0);

        // Nothing left behind.
        ChildProcess pgrep;
        pgrep.start (StringArray { "pgrep", "-P", String ((int) ::getpid()), "sleep" });
        CHECK (pgrep.readAllProcessOutput().trim().isEmpty());

        // Real bridged VST3s, if this machine has them (yabridge + Wine): replay exactly
        // what PluginListComponent does -- four pool threads, and as soon as one runs
        // out of files the pool is torn down with removeAllJobs (true, ...), which asks
        // the still-running probes to exit. Nothing may end up blacklisted.
        File yabridgeDir ("/home/dguedry/.vst3/yabridge");
        if (yabridgeDir.isDirectory())
        {
            AudioPluginFormat* vst3 = nullptr;
            for (auto* fmt : host.getFormatManager().getFormats()) if (fmt->getName() == "VST3") vst3 = fmt;

            KnownPluginList list;
            list.setCustomScanner (std::make_unique<PluginHost::OutOfProcessScanner>());
            TemporaryFile dmp (".txt");
            PluginDirectoryScanner ds (list, *vst3, FileSearchPath (yabridgeDir.getFullPathName()), true, dmp.getFile(), false);
            // Bundles whose Windows plugin is currently missing (being reinstalled) can't load.
            int expected = 0;
            for (const auto& bundle : yabridgeDir.findChildFiles (File::findFilesAndDirectories, false, "*.vst3"))
                if (PluginHost::brokenBridgeTarget (bundle.getFullPathName()).isEmpty()) ++expected;
                else std::printf ("     (skipping %s: bridge target missing)\n", bundle.getFileName().toRawUTF8());

            struct Job : public ThreadPoolJob
            {
                Job (PluginDirectoryScanner& s, std::atomic<bool>& f) : ThreadPoolJob ("scan"), ds (s), finished (f) {}
                JobStatus runJob() override
                {
                    String name;
                    while (! finished.load() && ds.scanNextFile (true, name) && ! shouldExit()) {}
                    finished.store (true);
                    return jobHasFinished;
                }
                PluginDirectoryScanner& ds; std::atomic<bool>& finished;
            };
            std::atomic<bool> finished { false };
            t0 = Time::getMillisecondCounterHiRes();
            {
                ThreadPool pool (ThreadPoolOptions{}.withNumberOfThreads (4));
                for (int i = 0; i < 4; ++i) pool.addJob (new Job (ds, finished), true);
                while (! finished.load()) Thread::sleep (20);
                pool.removeAllJobs (true, 60000);        // what ~Scanner does
            }
            std::printf ("     dialog-style scan of %s: %d plugin(s) expected, %d found, %d blacklisted, %d failed, %.1f s\n",
                         yabridgeDir.getFullPathName().toRawUTF8(), expected, list.getNumTypes(),
                         list.getBlacklistedFiles().size(), ds.getFailedFiles().size(), (Time::getMillisecondCounterHiRes() - t0) / 1000.0);
            for (auto& b : list.getBlacklistedFiles()) std::printf ("       BLACKLISTED %s\n", b.toRawUTF8());
            // A probe stopped by the teardown may legitimately yield nothing (it will be
            // scanned next time); what must never happen is a blacklist entry.
            CHECK (list.getBlacklistedFiles().isEmpty());
            CHECK (list.getNumTypes() <= expected && list.getNumTypes() + ds.getFailedFiles().size() >= expected);
            CHECK (list.getNumTypes() >= 1);

            // The app-owned background scanner (what the Plugins window uses).
            auto& sc = host.getScanner();
            CHECK (! sc.isScanning());
            CHECK (sc.startScan (*vst3, FileSearchPath (yabridgeDir.getFullPathName())));
            CHECK (sc.isScanning());
            CHECK (! sc.startScan (*vst3, FileSearchPath (yabridgeDir.getFullPathName())));   // busy
            t0 = Time::getMillisecondCounterHiRes();
            double lastReport = 0.0;
            while (sc.isScanning() && Time::getMillisecondCounterHiRes() - t0 < 120000.0)
            {
                pump (50);
                const auto el = Time::getMillisecondCounterHiRes() - t0;
                if (el - lastReport > 5000.0)
                {
                    lastReport = el;
                    std::printf ("       ... %.0f s: progress %.2f, current '%s'\n", el / 1000.0, sc.getProgress(), sc.getCurrentFile().toRawUTF8());
                }
            }
            int fromYabridge = 0;
            for (auto& d : host.getKnownPlugins().getTypes()) if (d.fileOrIdentifier.startsWith (yabridgeDir.getFullPathName())) ++fromYabridge;
            std::printf ("     background scanner: %d/%d yabridge plugins, %d newly blacklisted, %d failed, %.1f s\n",
                         fromYabridge, expected, sc.getNewlyBlacklistedFiles().size(), sc.getFailedFiles().size(), (Time::getMillisecondCounterHiRes() - t0) / 1000.0);
            CHECK (! sc.isScanning());
            CHECK (sc.getNewlyBlacklistedFiles().isEmpty());
            CHECK (fromYabridge == expected);
        }
    }

    // --- crash isolation --------------------------------------------------------------------
    {
        engine.setPreloadAllPrograms (false);
        engine.selectProgram (0, 40);
        settle();
        CHECK (engine.addSlot (0, 40, synth, err));
        settle();
        auto* victim = engine.getPlugin (0, 40, 0);
        CHECK (victim != nullptr && victim->isAlive());
        if (victim != nullptr && victim->isAlive())
        {
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            CHECK (render (engine, 10) > 0.01f);

            // Find and kill our plugin host children the hard way, like a plugin crash would.
            ChildProcess pgrep;
            pgrep.start (StringArray { "pgrep", "-P", String ((int) ::getpid()), "-f", "plugin-host" });
            auto pids = StringArray::fromLines (pgrep.readAllProcessOutput().trim());
            std::printf ("     %d plugin host processes running; killing them all\n", pids.size());
            CHECK (pids.size() >= 1);
            for (auto& pidText : pids)
                if (pidText.trim().isNotEmpty()) ::kill ((pid_t) pidText.getIntValue(), SIGKILL);

            pump (300);
            CHECK (! victim->isAlive());
            CHECK (! engine.isPluginAlive (0, 40, 0));
            CHECK (engine.getPluginLoadError (0, 40, 0).isNotEmpty());
            std::printf ("     after crash: %s\n", engine.getPluginLoadError (0, 40, 0).toRawUTF8());

            // The engine keeps rendering (silence from the dead slot) instead of hanging.
            CHECK (render (engine, 5) < 1e-4f);

            // Reload brings a fresh process and sound comes back.
            engine.reloadPlugin (0, 40, 0);
            settle();
            CHECK (engine.isPluginAlive (0, 40, 0));
            engine.injectMidi (0, MidiMessage::noteOn (1, 60, (uint8) 100));
            const float back = render (engine, 20);
            std::printf ("     rms after reload of crashed plugin: %f\n", back);
            CHECK (back > 0.01f);
            engine.injectMidi (0, MidiMessage::noteOff (1, 60));
            render (engine, 10);
        }
        engine.clearProgram (0, 40);
        settle();
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
                settle();
                CHECK (engine.addSlot (0, 20, desc, err));
                settle();
                if (err.isNotEmpty()) std::printf ("     addSlot error: %s\n", err.toRawUTF8());
                if (auto* x = engine.getPlugin (0, 20, 0); x != nullptr && x->isAlive())
                {
                    std::printf ("     loaded in its own process: %s, %d parameters\n", x->getName().toRawUTF8(), (int) x->getParameters().size());
                    {
                        // VST3 MIDI controller proxies: the helper asks IMidiMapping which copy is which channel.
                        int proxies = 0, vol1 = -1, vol2 = -1;
                        for (auto& p : x->getParameters())
                        {
                            if (p.midiChannel > 0) ++proxies;
                            if (p.midiController == 7 && p.midiChannel == 1) vol1 = p.index;
                            if (p.midiController == 7 && p.midiChannel == 2) vol2 = p.index;
                        }
                        std::printf ("     %d MIDI controller proxies; CC7 ch1 -> #%d, CC7 ch2 -> #%d\n", proxies, vol1, vol2);
                        if (vol1 >= 0 && vol2 >= 0)
                        {
                            // Send CC 7 on channel 1 (the Upper input's channel): the ch1 copy must move, the ch2 copy must not.
                            float a1 = 0, a2 = 0, b1 = 0, b2 = 0;
                            CHECK (x->fetchParameterValue (vol1, a1) && x->fetchParameterValue (vol2, a2));
                            const float target = a1 > 0.5f ? 0.1f : 0.9f;
                            engine.injectMidi (0, MidiMessage::controllerEvent (1, 7, (int) std::lround (target * 127.0f)));
                            render (engine, 5);
                            CHECK (x->fetchParameterValue (vol1, b1) && x->fetchParameterValue (vol2, b2));
                            std::printf ("     CC7 ch1 -> %d: ch1 copy %.3f -> %.3f, ch2 copy %.3f -> %.3f\n", (int) std::lround (target * 127.0f), a1, b1, a2, b2);
                            CHECK (std::abs (b1 - target) < 0.02f);
                            CHECK (std::abs (b2 - a2) < 1e-3f);
                        }
                    }
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

    // --- parallel loading: several plugins load at once ---------------------------------
    {
        std::printf ("     loader pool: %d threads\n", engine.getParallelLoads());
        CHECK (engine.getParallelLoads() >= 2);
        engine.selectProgram (0, 60);
        settle();
        int maxInFlight = 0;
        for (int k = 0; k < 6; ++k) { String e2; engine.addSlot (0, 60, synth, e2); }
        const auto t0p = Time::getMillisecondCounterHiRes();
        while (engine.hasPendingLoads() && Time::getMillisecondCounterHiRes() - t0p < 60000.0)
        {
            maxInFlight = jmax (maxInFlight, engine.getLoadsInFlight());
            pump (2);
        }
        std::printf ("     6 loads: max %d in flight, %.0f ms total\n", maxInFlight, Time::getMillisecondCounterHiRes() - t0p);
        CHECK (maxInFlight >= 2);
        int alive = 0;
        for (int k = 0; k < 6; ++k) if (engine.isPluginAlive (0, 60, k)) ++alive;
        CHECK (alive == 6);
        CHECK (render (engine, 3) < 1e-4f);     // silent until played
    }

    // --- plugin icons: generated badges always, embedded Windows icons when present ------
    {
        PluginDescription d; d.name = "Kontakt 8"; d.manufacturerName = "Native Instruments"; d.pluginFormatName = "VST3";
        CHECK (PluginIcons::initials ("Kontakt 8") == "K8");
        CHECK (PluginIcons::initials ("Calf Monosynth") == "CM");
        CHECK (PluginIcons::initials ("Dirt") == "D");
        CHECK (PluginIcons::badgeColour ("Native Instruments") == PluginIcons::badgeColour ("native instruments "));
        auto b = PluginIcons::badge (d, 48);
        CHECK (b.isValid() && b.getWidth() == 48 && b.getPixelAt (24, 24).getAlpha() > 200);
        TemporaryFile iconCache;
        PluginIcons icons (iconCache.getFile());
        CHECK (icons.get (d, 32).getWidth() == 32);
        d.fileOrIdentifier = "/home/dguedry/.vst3/yabridge/Kontakt 8.vst3";
        if (File (d.fileOrIdentifier).isDirectory())
        {
            const auto pe = PluginIcons::windowsBinaryFor (d);
            std::printf ("     Kontakt windows binary: %s\n", pe.getFullPathName().toRawUTF8());
            CHECK (pe.existsAsFile());
            const auto t0i = Time::getMillisecondCounterHiRes();
            const auto img = PluginIcons::fromWindowsBinary (pe);
            std::printf ("     embedded icon: %dx%d in %.1f ms\n", img.getWidth(), img.getHeight(), Time::getMillisecondCounterHiRes() - t0i);
            CHECK (img.isValid() && img.getWidth() >= 128);
            CHECK (icons.native (d).isValid());
            CHECK (iconCache.getFile().getNumberOfChildFiles (File::findFiles, "*.png") == 1);   // cached
        }
        PluginDescription none; none.name = "No Icon"; none.manufacturerName = "X"; none.fileOrIdentifier = "/nonexistent";
        CHECK (! icons.native (none).isValid() && icons.get (none, 24).isValid());
    }

    engine.removeListener (&listener);
    std::printf (failures == 0 ? "EngineTest: all checks passed\n" : "EngineTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
