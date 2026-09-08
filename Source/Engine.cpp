#include "Engine.h"

using namespace juce;

namespace perf
{

//==============================================================================
// Runtime structures
//==============================================================================
struct Engine::SlotRuntime : public AudioProcessorListener
{
    SlotRuntime (Engine& e, int in, int prog, int slot) : engine (e), inputIndex (in), program (prog), slotIndex (slot) {}
    ~SlotRuntime() override { if (instance != nullptr) instance->removeListener (this); }

    void applyDef (const SlotDef& d)
    {
        enabled.store (d.enabled);
        gain.store (Decibels::decibelsToGain (d.gainDb, -60.0f));
        transpose.store (d.transpose);
        lowKey.store (d.lowKey);
        highKey.store (d.highKey);
        outChannel.store (d.outChannel);
    }

    void ensureScratch (int numSamples)
    {
        if (instance == nullptr) return;
        const int ch = jmax (1, instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels());
        if (scratch.getNumChannels() != ch || scratch.getNumSamples() < numSamples)
            scratch.setSize (ch, numSamples, false, false, true);
    }

    // AudioProcessorListener -- used for "last touched parameter"
    void audioProcessorParameterChanged (AudioProcessor*, int paramIndex, float) override
    {
        if (! engine.suppressTouch.load())
            engine.noteParameterTouched (inputIndex, program, slotIndex, paramIndex);
    }
    void audioProcessorChanged (AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin (AudioProcessor*, int paramIndex) override
    {
        engine.noteParameterTouched (inputIndex, program, slotIndex, paramIndex);
    }

    Engine& engine;
    int inputIndex, program;
    std::atomic<int> slotIndex;
    std::unique_ptr<AudioPluginInstance> instance;
    String loadError;
    std::atomic<bool> enabled { true };
    std::atomic<float> gain { 1.0f };
    std::atomic<int> transpose { 0 }, lowKey { 0 }, highKey { 127 }, outChannel { 0 };
    AudioBuffer<float> scratch;
    MidiBuffer midi;
};

struct Engine::MappingRuntime
{
    MappingDef def;
    AudioProcessorParameter* param = nullptr;
};

struct Engine::ProgramRuntime
{
    int program = 0;
    std::vector<std::unique_ptr<SlotRuntime>> slots;
    std::vector<MappingRuntime> mappings;
    int releaseSamplesLeft = 0;
    bool needsNotesOff = false;
};

struct Engine::InputRuntime
{
    String deviceIdentifier;                  // guarded by Engine::lock
    std::atomic<int> channel { 0 };
    std::atomic<bool> respondToProgramChange { true };
    MidiMessageCollector collector;
    MidiBuffer block;
    std::map<int, std::unique_ptr<ProgramRuntime>> loaded;
    ProgramRuntime* active = nullptr;
    std::vector<ProgramRuntime*> releasing;

    bool isReleasing (const ProgramRuntime* p) const
    {
        return std::find (releasing.begin(), releasing.end(), p) != releasing.end();
    }
    void stopReleasing (const ProgramRuntime* p)
    {
        releasing.erase (std::remove (releasing.begin(), releasing.end(), p), releasing.end());
    }
};

//==============================================================================
Engine::Engine (PluginHost& h, PropertiesFile& s, bool startAudioDevice) : host (h), settings (s)
{
    if (startAudioDevice)
    {
        std::unique_ptr<XmlElement> savedAudio (settings.getXmlValue ("audioDeviceState"));
        deviceManager.initialise (0, 2, savedAudio.get(), true);
        deviceManager.addChangeListener (this);
        deviceManager.addAudioCallback (this);
    }
    deviceManager.addMidiInputDeviceCallback ({}, this);

    loadSetup (Setup::makeDefault());
    startTimer (750);
}

Engine::~Engine()
{
    stopTimer();
    cancelPendingUpdate();
    deviceManager.removeMidiInputDeviceCallback ({}, this);
    deviceManager.removeAudioCallback (this);
    deviceManager.removeChangeListener (this);
    teardownRuntimes();
}

void Engine::changeListenerCallback (ChangeBroadcaster*)
{
    if (auto xml = deviceManager.createStateXml())
        settings.setValue ("audioDeviceState", xml.get());
    else
        settings.removeValue ("audioDeviceState");
    settings.saveIfNeeded();
}

//==============================================================================
// Setup
//==============================================================================
void Engine::loadSetup (Setup newSetup)
{
    teardownRuntimes();
    setup = std::move (newSetup);
    rebuildRuntimes();
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

Setup Engine::captureSetup()
{
    for (int i = 0; i < (int) runtimes.size(); ++i)
        for (auto& [prog, rt] : runtimes[(size_t) i]->loaded)
            captureProgramState (i, *rt);
    return setup;
}

void Engine::setPreloadAllPrograms (bool b)  { setup.preloadAllPrograms = b; }
void Engine::setReleaseTailSeconds (double s) { setup.releaseTailSeconds = jlimit (0.0, 30.0, s); }

void Engine::rebuildRuntimes()
{
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
    {
        auto rt = std::make_unique<InputRuntime>();
        rt->collector.reset (sampleRate);
        rt->channel.store (setup.inputs[(size_t) i].channel);
        rt->respondToProgramChange.store (setup.inputs[(size_t) i].respondToProgramChange);
        runtimes.push_back (std::move (rt));
    }

    openMidiDevices();

    for (int i = 0; i < (int) setup.inputs.size(); ++i)
    {
        auto* prog = ensureProgramLoaded (i, setup.inputs[(size_t) i].currentProgram);
        const ScopedLock sl (lock);
        runtimes[(size_t) i]->active = prog;
    }
}

void Engine::teardownRuntimes()
{
    // Take everything away from the audio thread first.
    std::vector<std::unique_ptr<InputRuntime>> old;
    {
        const ScopedLock sl (lock);
        old = std::move (runtimes);
        runtimes.clear();
    }

    for (int i = 0; i < (int) old.size(); ++i)
    {
        auto& in = *old[(size_t) i];
        in.active = nullptr;
        in.releasing.clear();
        for (auto& [prog, rt] : in.loaded)
        {
            if (i < (int) setup.inputs.size())
                captureProgramState (i, *rt);
            for (auto& s : rt->slots)
                destroySlot (std::move (s));
            rt->slots.clear();
        }
    }
}

//==============================================================================
// Inputs
//==============================================================================
int Engine::addInput (const String& name)
{
    InputDef def;
    def.name = name;
    setup.inputs.push_back (def);

    auto rt = std::make_unique<InputRuntime>();
    rt->collector.reset (sampleRate);
    {
        const ScopedLock sl (lock);
        runtimes.push_back (std::move (rt));
    }
    const int idx = (int) setup.inputs.size() - 1;
    auto* prog = ensureProgramLoaded (idx, 0);
    {
        const ScopedLock sl (lock);
        runtimes[(size_t) idx]->active = prog;
    }
    listeners.call ([] (Listener& l) { l.setupChanged(); });
    return idx;
}

void Engine::removeInput (int inputIndex)
{
    if (! validInput (inputIndex)) return;

    std::unique_ptr<InputRuntime> in;
    {
        const ScopedLock sl (lock);
        in = std::move (runtimes[(size_t) inputIndex]);
        runtimes.erase (runtimes.begin() + inputIndex);
        // Slot runtimes report their input index for "touched" events; fix the survivors.
        for (int i = inputIndex; i < (int) runtimes.size(); ++i)
            for (auto& [p, rt] : runtimes[(size_t) i]->loaded)
                for (auto& s : rt->slots)
                    s->inputIndex = i;
    }

    for (auto& [p, rt] : in->loaded)
        for (auto& s : rt->slots)
            destroySlot (std::move (s));

    setup.inputs.erase (setup.inputs.begin() + inputIndex);
    openMidiDevices();
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::setInputName (int inputIndex, const String& name)
{
    if (validInput (inputIndex))
        setup.inputs[(size_t) inputIndex].name = name;
}

void Engine::setInputMidiDevice (int inputIndex, const MidiDeviceInfo& info)
{
    if (! validInput (inputIndex)) return;
    auto& def = setup.inputs[(size_t) inputIndex];
    def.midiDeviceIdentifier = info.identifier;
    def.midiDeviceName = info.name;
    openMidiDevices();
}

void Engine::setInputChannel (int inputIndex, int channel)
{
    if (! validInput (inputIndex)) return;
    setup.inputs[(size_t) inputIndex].channel = jlimit (0, 16, channel);
    runtimes[(size_t) inputIndex]->channel.store (channel);
}

void Engine::setInputRespondToProgramChange (int inputIndex, bool b)
{
    if (! validInput (inputIndex)) return;
    setup.inputs[(size_t) inputIndex].respondToProgramChange = b;
    runtimes[(size_t) inputIndex]->respondToProgramChange.store (b);
}

void Engine::refreshMidiDevices()
{
    openMidiDevices();
    listeners.call ([] (Listener& l) { l.setupChanged(); });
}

void Engine::openMidiDevices()
{
    auto available = MidiInput::getAvailableDevices();

    StringArray wanted;
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
    {
        auto& def = setup.inputs[(size_t) i];

        // Re-find the device by identifier, then by name (identifiers can change on re-plug).
        bool found = false;
        for (auto& d : available)
            if (d.identifier == def.midiDeviceIdentifier && def.midiDeviceIdentifier.isNotEmpty())
                found = true;
        if (! found && def.midiDeviceName.isNotEmpty())
            for (auto& d : available)
                if (d.name == def.midiDeviceName) { def.midiDeviceIdentifier = d.identifier; found = true; break; }

        if (found)
            wanted.addIfNotAlreadyThere (def.midiDeviceIdentifier);

        const ScopedLock sl (lock);
        runtimes[(size_t) i]->deviceIdentifier = found ? def.midiDeviceIdentifier : String();
    }

    for (auto& d : available)
        deviceManager.setMidiInputDeviceEnabled (d.identifier, wanted.contains (d.identifier));
}

//==============================================================================
// Programs
//==============================================================================
Engine::ProgramRuntime* Engine::getLoaded (int inputIndex, int program) const
{
    if (! validInput (inputIndex)) return nullptr;
    auto& m = runtimes[(size_t) inputIndex]->loaded;
    auto it = m.find (program);
    return it == m.end() ? nullptr : it->second.get();
}

bool Engine::isProgramLoaded (int inputIndex, int program) const
{
    return getLoaded (inputIndex, program) != nullptr;
}

void Engine::selectProgram (int inputIndex, int program)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;

    auto& def = setup.inputs[(size_t) inputIndex];
    auto& in = *runtimes[(size_t) inputIndex];
    def.currentProgram = program;

    auto* next = ensureProgramLoaded (inputIndex, program);

    {
        const ScopedLock sl (lock);
        if (in.active != next)
        {
            if (in.active != nullptr)
            {
                in.active->needsNotesOff = true;
                in.active->releaseSamplesLeft = (int) (setup.releaseTailSeconds * sampleRate);
                if (! in.isReleasing (in.active))
                    in.releasing.push_back (in.active);
            }
            in.stopReleasing (next);
            next->releaseSamplesLeft = 0;
            in.active = next;
        }
    }

    listeners.call ([=] (Listener& l) { l.programChanged (inputIndex, program); });
}

void Engine::setProgramName (int inputIndex, int program, const String& name)
{
    if (validInput (inputIndex) && validProgram (program))
        setup.inputs[(size_t) inputIndex].programs[(size_t) program].name = name;
}

void Engine::copyProgram (int inputIndex, int from, int to)
{
    if (! validInput (inputIndex) || ! validProgram (from) || ! validProgram (to) || from == to) return;
    if (auto* rt = getLoaded (inputIndex, from))
        captureProgramState (inputIndex, *rt);

    clearProgram (inputIndex, to);
    auto& in = setup.inputs[(size_t) inputIndex];
    in.programs[(size_t) to] = in.programs[(size_t) from];

    // If the destination is loaded (e.g. it's the active program), rebuild it live.
    if (getLoaded (inputIndex, to) != nullptr)
    {
        unloadProgram (*runtimes[(size_t) inputIndex], to);
        auto* prog = ensureProgramLoaded (inputIndex, to);
        const ScopedLock sl (lock);
        if (in.currentProgram == to)
            runtimes[(size_t) inputIndex]->active = prog;
    }
    listeners.call ([=] (Listener& l) { l.programContentChanged (inputIndex, to); });
}

void Engine::clearProgram (int inputIndex, int program)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& in = *runtimes[(size_t) inputIndex];
    const bool wasActive = (in.active != nullptr && in.active->program == program);

    unloadProgram (in, program);
    setup.inputs[(size_t) inputIndex].programs[(size_t) program] = ProgramDef();

    if (wasActive)
    {
        auto* prog = ensureProgramLoaded (inputIndex, program);
        const ScopedLock sl (lock);
        in.active = prog;
    }
    listeners.call ([=] (Listener& l) { l.programContentChanged (inputIndex, program); });
}

Engine::ProgramRuntime* Engine::ensureProgramLoaded (int inputIndex, int program)
{
    if (auto* rt = getLoaded (inputIndex, program))
        return rt;

    auto built = buildProgram (inputIndex, program);
    auto* raw = built.get();
    {
        const ScopedLock sl (lock);
        runtimes[(size_t) inputIndex]->loaded[program] = std::move (built);
    }
    listeners.call ([=] (Listener& l) { l.programContentChanged (inputIndex, program); });
    return raw;
}

std::unique_ptr<Engine::ProgramRuntime> Engine::buildProgram (int inputIndex, int program)
{
    auto rt = std::make_unique<ProgramRuntime>();
    rt->program = program;

    const auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    for (int s = 0; s < (int) def.slots.size(); ++s)
        rt->slots.push_back (buildSlot (inputIndex, program, s, def.slots[(size_t) s]));

    resolveMappings (*rt, def);
    return rt;
}

std::unique_ptr<Engine::SlotRuntime> Engine::buildSlot (int inputIndex, int program, int slotIndex, const SlotDef& def)
{
    auto slot = std::make_unique<SlotRuntime> (*this, inputIndex, program, slotIndex);
    slot->applyDef (def);

    String error;
    auto instance = host.createInstance (def.plugin, sampleRate, blockSize, error);

    if (instance == nullptr)
    {
        slot->loadError = error;
        listeners.call ([&] (Listener& l) { l.statusMessage ("Failed to load " + def.plugin.name + ": " + error); });
        return slot;
    }

    // Activate every bus and give the plugin real buffers for all of them. JUCE hands
    // inactive buses null channel pointers, and some plugins (Kontakt via yabridge, for
    // one) write into those anyway and crash the host. Only the main bus gets mixed.
    instance->enableAllBuses();

    if (def.state.getSize() > 0)
    {
        // Restoring state fires parameter callbacks; don't let them count as "touched".
        suppressTouch.store (true);
        instance->setStateInformation (def.state.getData(), (int) def.state.getSize());
        suppressTouch.store (false);
    }

    prepareInstance (*instance);
    instance->addListener (slot.get());
    slot->instance = std::move (instance);
    slot->ensureScratch (blockSize);
    return slot;
}

void Engine::prepareInstance (AudioPluginInstance& inst)
{
    inst.setNonRealtime (false);
    inst.setPlayHead (nullptr);
    inst.prepareToPlay (sampleRate, blockSize);
}

void Engine::unloadProgram (InputRuntime& in, int program)
{
    std::unique_ptr<ProgramRuntime> rt;
    {
        const ScopedLock sl (lock);
        auto it = in.loaded.find (program);
        if (it == in.loaded.end()) return;
        rt = std::move (it->second);
        in.loaded.erase (it);
        if (in.active == rt.get()) in.active = nullptr;
        in.stopReleasing (rt.get());
    }

    // Find our input index to capture state into the right definition.
    for (int i = 0; i < (int) runtimes.size(); ++i)
        if (runtimes[(size_t) i].get() == &in)
            captureProgramState (i, *rt);

    for (auto& s : rt->slots)
        destroySlot (std::move (s));
}

void Engine::destroySlot (std::unique_ptr<SlotRuntime> slot)
{
    if (slot == nullptr || slot->instance == nullptr) return;
    auto* inst = slot->instance.get();
    listeners.call ([inst] (Listener& l) { l.instanceAboutToBeDeleted (inst); });
    inst->removeListener (slot.get());
    inst->releaseResources();
    slot->instance.reset();
}

void Engine::captureProgramState (int inputIndex, ProgramRuntime& rt)
{
    if (! validInput (inputIndex)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) rt.program];
    for (size_t s = 0; s < rt.slots.size() && s < def.slots.size(); ++s)
        if (auto* inst = rt.slots[s]->instance.get())
        {
            MemoryBlock mb;
            inst->getStateInformation (mb);
            if (mb.getSize() > 0)
                def.slots[s].state = std::move (mb);
        }
}

//==============================================================================
// Slots
//==============================================================================
bool Engine::addSlot (int inputIndex, int program, const PluginDescription& desc, String& error)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return false;

    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    SlotDef sd;
    sd.plugin = desc;
    def.slots.push_back (sd);
    const int slotIndex = (int) def.slots.size() - 1;

    if (auto* rt = getLoaded (inputIndex, program))
    {
        auto slot = buildSlot (inputIndex, program, slotIndex, sd);
        const bool ok = slot->instance != nullptr;
        error = slot->loadError;
        {
            const ScopedLock sl (lock);
            rt->slots.push_back (std::move (slot));
        }
        listeners.call ([=] (Listener& l) { l.programContentChanged (inputIndex, program); });
        return ok;
    }

    listeners.call ([=] (Listener& l) { l.programContentChanged (inputIndex, program); });
    return true;
}

void Engine::removeSlot (int inputIndex, int program, int slotIndex)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return;

    // Fix up mappings that point at this or later slots.
    for (auto it = def.mappings.begin(); it != def.mappings.end();)
    {
        if (it->slot == slotIndex)       it = def.mappings.erase (it);
        else { if (it->slot > slotIndex) --it->slot; ++it; }
    }
    def.slots.erase (def.slots.begin() + slotIndex);

    if (auto* rt = getLoaded (inputIndex, program))
    {
        std::unique_ptr<SlotRuntime> removed;
        {
            const ScopedLock sl (lock);
            if (slotIndex < (int) rt->slots.size())
            {
                removed = std::move (rt->slots[(size_t) slotIndex]);
                rt->slots.erase (rt->slots.begin() + slotIndex);
                for (int s = slotIndex; s < (int) rt->slots.size(); ++s)
                    rt->slots[(size_t) s]->slotIndex.store (s);
            }
            resolveMappings (*rt, def);
        }
        destroySlot (std::move (removed));
    }
    listeners.call ([=] (Listener& l) { l.programContentChanged (inputIndex, program); });
}

#define PERF_SLOT_SETTER(name, field, expr)                                                    \
    void Engine::name (int inputIndex, int program, int slotIndex, decltype (SlotDef::field) v) \
    {                                                                                          \
        if (! validInput (inputIndex) || ! validProgram (program)) return;                     \
        auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];              \
        if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return;                      \
        def.slots[(size_t) slotIndex].field = v;                                               \
        if (auto* rt = getLoaded (inputIndex, program))                                        \
            if (slotIndex < (int) rt->slots.size())                                            \
                rt->slots[(size_t) slotIndex]->expr;                                           \
    }

PERF_SLOT_SETTER (setSlotEnabled,    enabled,    enabled.store (v))
PERF_SLOT_SETTER (setSlotGainDb,     gainDb,     gain.store (Decibels::decibelsToGain (v, -60.0f)))
PERF_SLOT_SETTER (setSlotTranspose,  transpose,  transpose.store (v))
PERF_SLOT_SETTER (setSlotOutChannel, outChannel, outChannel.store (v))
#undef PERF_SLOT_SETTER

void Engine::setSlotKeyRange (int inputIndex, int program, int slotIndex, int low, int high)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (slotIndex < 0 || slotIndex >= (int) def.slots.size()) return;
    low = jlimit (0, 127, low); high = jlimit (low, 127, high);
    def.slots[(size_t) slotIndex].lowKey = low;
    def.slots[(size_t) slotIndex].highKey = high;
    if (auto* rt = getLoaded (inputIndex, program))
        if (slotIndex < (int) rt->slots.size())
        {
            rt->slots[(size_t) slotIndex]->lowKey.store (low);
            rt->slots[(size_t) slotIndex]->highKey.store (high);
        }
}

AudioPluginInstance* Engine::getSlotInstance (int inputIndex, int program, int slot) const
{
    if (auto* rt = getLoaded (inputIndex, program))
        if (slot >= 0 && slot < (int) rt->slots.size())
            return rt->slots[(size_t) slot]->instance.get();
    return nullptr;
}

String Engine::getSlotLoadError (int inputIndex, int program, int slot) const
{
    if (auto* rt = getLoaded (inputIndex, program))
        if (slot >= 0 && slot < (int) rt->slots.size())
            return rt->slots[(size_t) slot]->loadError;
    return {};
}

//==============================================================================
// Mappings
//==============================================================================
String Engine::getParameterId (const AudioProcessorParameter& p)
{
    if (auto* hosted = dynamic_cast<const HostedAudioProcessorParameter*> (&p))
        return hosted->getParameterID();
    return String (p.getParameterIndex());
}

AudioProcessorParameter* Engine::findParameter (AudioPluginInstance& inst, const String& paramId)
{
    for (auto* p : inst.getParameters())
        if (getParameterId (*p) == paramId)
            return p;
    // Fall back to a numeric index.
    if (paramId.containsOnly ("0123456789"))
    {
        const int idx = paramId.getIntValue();
        auto& params = inst.getParameters();
        if (idx >= 0 && idx < params.size())
            return params[idx];
    }
    return nullptr;
}

void Engine::resolveMappings (ProgramRuntime& rt, const ProgramDef& def)
{
    std::vector<MappingRuntime> resolved;
    for (auto& m : def.mappings)
    {
        MappingRuntime mr;
        mr.def = m;
        if (m.slot >= 0 && m.slot < (int) rt.slots.size())
            if (auto* inst = rt.slots[(size_t) m.slot]->instance.get())
                mr.param = findParameter (*inst, m.paramId);
        resolved.push_back (mr);
    }
    rt.mappings = std::move (resolved);
}

void Engine::addMapping (int inputIndex, int program, const MappingDef& m)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    def.mappings.push_back (m);
    if (auto* rt = getLoaded (inputIndex, program)) { const ScopedLock sl (lock); resolveMappings (*rt, def); }
    listeners.call ([=] (Listener& l) { l.programContentChanged (inputIndex, program); });
}

void Engine::updateMapping (int inputIndex, int program, int mappingIndex, const MappingDef& m)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (mappingIndex < 0 || mappingIndex >= (int) def.mappings.size()) return;
    def.mappings[(size_t) mappingIndex] = m;
    if (auto* rt = getLoaded (inputIndex, program)) { const ScopedLock sl (lock); resolveMappings (*rt, def); }
    listeners.call ([=] (Listener& l) { l.programContentChanged (inputIndex, program); });
}

void Engine::removeMapping (int inputIndex, int program, int mappingIndex)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (mappingIndex < 0 || mappingIndex >= (int) def.mappings.size()) return;
    def.mappings.erase (def.mappings.begin() + mappingIndex);
    if (auto* rt = getLoaded (inputIndex, program)) { const ScopedLock sl (lock); resolveMappings (*rt, def); }
    listeners.call ([=] (Listener& l) { l.programContentChanged (inputIndex, program); });
}

void Engine::setLearnArmed (bool b) { learnArmed.store (b); }

void Engine::noteParameterTouched (int inputIndex, int program, int slot, int paramIndex)
{
    // Can arrive on any thread; dedupe and hand off to the message thread.
    Event e { Event::touched, inputIndex, program, slot, paramIndex };
    postEvent (e);
}

void Engine::panic() { panicRequested.store (true); }

//==============================================================================
// Events (MIDI/audio thread -> message thread)
//==============================================================================
void Engine::postEvent (const Event& e)
{
    {
        const ScopedLock sl (eventLock);
        if (e.type == Event::touched)
        {
            // Coalesce bursts of the same parameter moving.
            for (auto& p : pendingEvents)
                if (p.type == Event::touched && p.input == e.input && p.a == e.a && p.b == e.b && p.c == e.c)
                    return;
        }
        pendingEvents.push_back (e);
    }
    triggerAsyncUpdate();
}

void Engine::handleAsyncUpdate()
{
    std::vector<Event> events;
    {
        const ScopedLock sl (eventLock);
        events.swap (pendingEvents);
    }

    for (auto& e : events)
    {
        switch (e.type)
        {
            case Event::programChange:
                selectProgram (e.input, e.a);
                break;

            case Event::learn:
                listeners.call ([&] (Listener& l) { l.learnReceived (e.input, (MappingDef::Source) e.a, e.b); });
                break;

            case Event::touched:
                lastTouched = { e.input, e.a, e.b, e.c };
                listeners.call ([&] (Listener& l) { l.parameterTouched (e.input, e.a, e.b, e.c); });
                break;
        }
    }
}

//==============================================================================
// Housekeeping: unload idle programs, or preload everything.
//==============================================================================
void Engine::timerCallback() { housekeeping(); }

void Engine::housekeeping()
{
    for (int i = 0; i < (int) runtimes.size(); ++i)
    {
        auto& in = *runtimes[(size_t) i];
        auto& def = setup.inputs[(size_t) i];

        if (setup.preloadAllPrograms)
        {
            // Load one missing program per tick to keep the UI breathing.
            for (int p = 0; p < InputDef::numPrograms; ++p)
                if (! def.programs[(size_t) p].isEmpty() && getLoaded (i, p) == nullptr)
                {
                    ensureProgramLoaded (i, p);
                    break;
                }
        }
        else
        {
            std::vector<int> idle;
            {
                const ScopedLock sl (lock);
                for (auto& [p, rt] : in.loaded)
                    if (rt.get() != in.active && ! in.isReleasing (rt.get()))
                        idle.push_back (p);
            }
            for (int p : idle)
            {
                unloadProgram (in, p);
                listeners.call ([=] (Listener& l) { l.programContentChanged (i, p); });
            }
        }
    }
}

//==============================================================================
// MIDI thread
//==============================================================================
void Engine::handleIncomingMidiMessage (MidiInput* source, const MidiMessage& m)
{
    const String id = source->getIdentifier();
    if (m.getChannel() == 0) return;    // sysex / realtime: ignored

    const ScopedLock sl (lock);
    for (int i = 0; i < (int) runtimes.size(); ++i)
        if (runtimes[(size_t) i]->deviceIdentifier == id)
            routeMidi (*runtimes[(size_t) i], i, m);
}

void Engine::injectMidi (int inputIndex, const MidiMessage& m)
{
    if (m.getChannel() == 0) return;
    MidiMessage stamped (m);
    if (stamped.getTimeStamp() <= 0.0)
        stamped.setTimeStamp (Time::getMillisecondCounterHiRes() * 0.001);

    const ScopedLock sl (lock);
    if (inputIndex >= 0 && inputIndex < (int) runtimes.size())
        routeMidi (*runtimes[(size_t) inputIndex], inputIndex, stamped);
}

void Engine::routeMidi (InputRuntime& in, int i, const MidiMessage& m)
{
    const int ch = m.getChannel();
    const int want = in.channel.load();
    if (want != 0 && ch != want) return;

    if (m.isProgramChange())
    {
        if (in.respondToProgramChange.load())
            postEvent ({ Event::programChange, i, m.getProgramChangeNumber(), 0, 0 });
        return;
    }

    if (learnArmed.load())
    {
        if (m.isController())            { learnArmed = false; postEvent ({ Event::learn, i, (int) MappingDef::Source::CC, m.getControllerNumber(), 0 }); }
        else if (m.isPitchWheel())       { learnArmed = false; postEvent ({ Event::learn, i, (int) MappingDef::Source::PitchBend, 0, 0 }); }
        else if (m.isChannelPressure())  { learnArmed = false; postEvent ({ Event::learn, i, (int) MappingDef::Source::ChannelPressure, 0, 0 }); }
    }

    in.collector.addMessageToQueue (m);
}

void Engine::renderBlockForTesting (float* const* out, int numOut, int numSamples)
{
    audioDeviceIOCallbackWithContext (nullptr, 0, out, numOut, numSamples, {});
}

//==============================================================================
// Audio thread
//==============================================================================
void Engine::audioDeviceAboutToStart (AudioIODevice* device)
{
    sampleRate = device->getCurrentSampleRate();
    blockSize  = device->getCurrentBufferSizeSamples();

    const ScopedLock sl (lock);
    for (auto& in : runtimes)
    {
        in->collector.reset (sampleRate);
        for (auto& [p, rt] : in->loaded)
            for (auto& s : rt->slots)
                if (s->instance != nullptr)
                {
                    s->instance->releaseResources();
                    prepareInstance (*s->instance);
                    s->ensureScratch (blockSize);
                }
    }
}

void Engine::audioDeviceStopped() {}

void Engine::audioDeviceIOCallbackWithContext (const float* const*, int,
                                               float* const* out, int numOut, int numSamples,
                                               const AudioIODeviceCallbackContext&)
{
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr)
            FloatVectorOperations::clear (out[c], numSamples);

    const ScopedLock sl (lock);
    const bool panicNow = panicRequested.exchange (false);

    for (auto& in : runtimes)
        processInput (*in, out, numOut, numSamples, panicNow);
}

void Engine::processInput (InputRuntime& in, float* const* out, int numOut, int numSamples, bool panicNow)
{
    in.block.clear();
    in.collector.removeNextBlockOfMessages (in.block, numSamples);

    if (in.active != nullptr)
        processProgram (*in.active, in.block, out, numOut, numSamples, panicNow);

    static const MidiBuffer emptyMidi;
    for (auto it = in.releasing.begin(); it != in.releasing.end();)
    {
        auto* p = *it;
        processProgram (*p, emptyMidi, out, numOut, numSamples, panicNow);
        p->releaseSamplesLeft -= numSamples;
        if (p->releaseSamplesLeft <= 0 || panicNow)
            it = in.releasing.erase (it);
        else
            ++it;
    }
}

static inline float normalisedValueOf (const MidiMessage& m, MappingDef::Source s)
{
    switch (s)
    {
        case MappingDef::Source::CC:              return m.getControllerValue() / 127.0f;
        case MappingDef::Source::PitchBend:       return m.getPitchWheelValue() / 16383.0f;
        case MappingDef::Source::ChannelPressure: return m.getChannelPressureValue() / 127.0f;
    }
    return 0.0f;
}

static inline bool mappingMatches (const MappingDef& d, const MidiMessage& m)
{
    switch (d.source)
    {
        case MappingDef::Source::CC:              return m.isController() && m.getControllerNumber() == d.number;
        case MappingDef::Source::PitchBend:       return m.isPitchWheel();
        case MappingDef::Source::ChannelPressure: return m.isChannelPressure();
    }
    return false;
}

void Engine::processProgram (ProgramRuntime& prog, const MidiBuffer& in, float* const* out, int numOut, int numSamples, bool panicNow)
{
    for (auto& slot : prog.slots)
        slot->midi.clear();

    if (prog.needsNotesOff || panicNow)
    {
        prog.needsNotesOff = false;
        for (auto& slot : prog.slots)
            for (int ch = 1; ch <= 16; ++ch)
            {
                slot->midi.addEvent (MidiMessage::controllerEvent (ch, 64, 0), 0);   // sustain off
                slot->midi.addEvent (MidiMessage::allNotesOff (ch), 0);
                if (panicNow)
                    slot->midi.addEvent (MidiMessage::allSoundOff (ch), 0);
            }
    }

    // Route incoming MIDI: mappings first, then per-slot filtering.
    for (const auto meta : in)
    {
        const auto m = meta.getMessage();
        const int pos = meta.samplePosition;

        bool consumed = false;
        for (auto& map : prog.mappings)
        {
            if (map.param == nullptr || ! mappingMatches (map.def, m)) continue;
            const float norm = normalisedValueOf (m, map.def.source);
            const float value = map.def.minValue + (map.def.maxValue - map.def.minValue) * norm;
            suppressTouch.store (true);
            map.param->setValueNotifyingHost (jlimit (0.0f, 1.0f, value));
            suppressTouch.store (false);
            if (! map.def.passThrough) consumed = true;
        }
        if (consumed) continue;

        for (auto& slot : prog.slots)
        {
            if (slot->instance == nullptr || ! slot->enabled.load()) continue;

            MidiMessage mm (m);
            if (mm.isNoteOnOrOff())
            {
                int note = mm.getNoteNumber();
                if (note < slot->lowKey.load() || note > slot->highKey.load()) continue;
                note += slot->transpose.load();
                if (note < 0 || note > 127) continue;
                mm.setNoteNumber (note);
            }
            const int oc = slot->outChannel.load();
            if (oc > 0) mm.setChannel (oc);
            slot->midi.addEvent (mm, pos);
        }
    }

    // Render each slot and mix to the main outputs.
    for (auto& slot : prog.slots)
    {
        auto* inst = slot->instance.get();
        if (inst == nullptr || inst->isSuspended()) continue;

        slot->ensureScratch (numSamples);
        AudioBuffer<float> view (slot->scratch.getArrayOfWritePointers(), slot->scratch.getNumChannels(), numSamples);
        view.clear();
        inst->processBlock (view, slot->midi);

        const int n = inst->getMainBusNumOutputChannels();
        if (n <= 0) continue;
        const float g = slot->enabled.load() ? slot->gain.load() : 0.0f;
        if (g <= 0.0f) continue;

        for (int c = 0; c < jmin (numOut, 2); ++c)
        {
            if (out[c] == nullptr) continue;
            const int src = (n == 1) ? 0 : jmin (c, n - 1);
            FloatVectorOperations::addWithMultiply (out[c], view.getReadPointer (src), g, numSamples);
        }
    }
}

} // namespace perf
