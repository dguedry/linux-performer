#include "Engine.h"

using namespace juce;

namespace perf
{

//==============================================================================
// Runtime structures
//==============================================================================

/** A hosted plugin: an instrument or an effect, living in its own process. */
struct Engine::PluginNode : public RemotePlugin::Listener
{
    PluginNode (Engine& e, int in, int prog, int slot, int effect)
        : engine (e), inputIndex (in), program (prog), slotIndex (slot), effectIndex (effect) {}

    ~PluginNode() override { if (plugin != nullptr) plugin->setListener (nullptr); }

    bool alive() const { return plugin != nullptr && plugin->isAlive(); }

    // RemotePlugin::Listener (reader thread)
    void remoteParameterTouched (RemotePlugin&, int index, float) override
    {
        engine.noteParameterTouched (inputIndex.load(), program, slotIndex.load(), effectIndex.load(), index);
    }
    void remoteParameterChanged (RemotePlugin&, int index, float) override
    {
        engine.noteParameterTouched (inputIndex.load(), program, slotIndex.load(), effectIndex.load(), index);
    }
    void remoteDied (RemotePlugin&) override
    {
        engine.notePluginDied (inputIndex.load(), program, slotIndex.load(), effectIndex.load());
    }

    Engine& engine;
    std::atomic<int> inputIndex;
    int program;
    std::atomic<int> slotIndex, effectIndex;
    std::unique_ptr<RemotePlugin> plugin;
    String loadError;
    MidiBuffer midi;
};

struct Engine::EffectRuntime : public PluginNode
{
    using PluginNode::PluginNode;
    std::atomic<bool> bypassed { false };
};

struct Engine::SlotRuntime : public PluginNode
{
    SlotRuntime (Engine& e, int in, int prog, int slot) : PluginNode (e, in, prog, slot, -1) {}

    void applyDef (const SlotDef& d)
    {
        enabled.store (d.enabled);
        gain.store (Decibels::decibelsToGain (d.gainDb, -60.0f));
        transpose.store (d.transpose);
        lowKey.store (d.lowKey);
        highKey.store (d.highKey);
        outChannel.store (d.outChannel);
    }

    std::atomic<bool> enabled { true };
    std::atomic<float> gain { 1.0f };
    std::atomic<int> transpose { 0 }, lowKey { 0 }, highKey { 127 }, outChannel { 0 };
    std::vector<std::unique_ptr<EffectRuntime>> effects;
    AudioBuffer<float> stereo;     // the slot's signal as it travels down the chain
};

struct Engine::MappingRuntime
{
    MappingDef def;
    RemotePlugin* plugin = nullptr;
    int paramIndex = -1;
};

struct Engine::ProgramRuntime
{
    int program = 0;
    std::vector<std::unique_ptr<SlotRuntime>> slots;
    std::vector<std::unique_ptr<EffectRuntime>> effects;   // program-level chain
    std::vector<MappingRuntime> mappings;
    AudioBuffer<float> mix;        // summed slots, before the program chain
    MidiBuffer programMidi;        // MIDI seen by the program chain
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

void Engine::notifyContent (int inputIndex, int program)
{
    listeners.call ([=] (Listener& l) { l.programContentChanged (inputIndex, program); });
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
            destroyProgramPlugins (*rt);
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
        // Nodes report their input index for "touched" events; fix the survivors.
        for (int i = inputIndex; i < (int) runtimes.size(); ++i)
            for (auto& [p, rt] : runtimes[(size_t) i]->loaded)
            {
                for (auto& s : rt->slots)
                {
                    s->inputIndex.store (i);
                    for (auto& e : s->effects) e->inputIndex.store (i);
                }
                for (auto& e : rt->effects) e->inputIndex.store (i);
            }
    }

    for (auto& [p, rt] : in->loaded)
        destroyProgramPlugins (*rt);

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
    notifyContent (inputIndex, to);
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
    notifyContent (inputIndex, program);
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
    notifyContent (inputIndex, program);
    return raw;
}

std::unique_ptr<Engine::ProgramRuntime> Engine::buildProgram (int inputIndex, int program)
{
    auto rt = std::make_unique<ProgramRuntime>();
    rt->program = program;
    rt->mix.setSize (2, blockSize);
    rt->programMidi.ensureSize (2048);

    const auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    for (int s = 0; s < (int) def.slots.size(); ++s)
        rt->slots.push_back (buildSlot (inputIndex, program, s, def.slots[(size_t) s]));
    for (int e = 0; e < (int) def.effects.size(); ++e)
        rt->effects.push_back (buildEffect (inputIndex, program, -1, e, def.effects[(size_t) e]));

    resolveMappings (*rt, def);
    return rt;
}

std::unique_ptr<Engine::SlotRuntime> Engine::buildSlot (int inputIndex, int program, int slotIndex, const SlotDef& def)
{
    auto slot = std::make_unique<SlotRuntime> (*this, inputIndex, program, slotIndex);
    slot->applyDef (def);
    slot->stereo.setSize (2, blockSize);
    loadPluginInto (*slot, def.plugin, def.state);
    for (int e = 0; e < (int) def.effects.size(); ++e)
        slot->effects.push_back (buildEffect (inputIndex, program, slotIndex, e, def.effects[(size_t) e]));
    return slot;
}

std::unique_ptr<Engine::EffectRuntime> Engine::buildEffect (int inputIndex, int program, int slotIndex, int effectIndex, const EffectDef& def)
{
    auto fx = std::make_unique<EffectRuntime> (*this, inputIndex, program, slotIndex, effectIndex);
    fx->bypassed.store (def.bypassed);
    loadPluginInto (*fx, def.plugin, def.state);
    return fx;
}

void Engine::loadPluginInto (PluginNode& node, const PluginDescription& desc, const MemoryBlock& state)
{
    auto plugin = std::make_unique<RemotePlugin>();
    String error;
    if (! plugin->load (desc, sampleRate, blockSize, error))
    {
        node.loadError = error;
        listeners.call ([&] (Listener& l) { l.statusMessage ("Failed to load " + desc.name + ": " + error); });
        return;
    }

    if (state.getSize() > 0 && ! plugin->setState (state))
        listeners.call ([&] (Listener& l) { l.statusMessage (desc.name + ": could not restore its saved state (" + plugin->getLastError() + ")"); });

    node.midi.ensureSize (2048);
    plugin->setListener (&node);
    node.plugin = std::move (plugin);
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

    destroyProgramPlugins (*rt);
}

void Engine::destroyProgramPlugins (ProgramRuntime& rt)
{
    for (auto& s : rt.slots)
    {
        for (auto& e : s->effects) destroyNode (e.get());
        destroyNode (s.get());
    }
    for (auto& e : rt.effects) destroyNode (e.get());
    rt.slots.clear();
    rt.effects.clear();
}

void Engine::destroyNode (PluginNode* node)
{
    if (node == nullptr || node->plugin == nullptr) return;
    node->plugin->setListener (nullptr);
    node->plugin->shutdown();
    node->plugin.reset();
}

void Engine::captureProgramState (int inputIndex, ProgramRuntime& rt)
{
    if (! validInput (inputIndex)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) rt.program];

    auto capture = [] (PluginNode& node, MemoryBlock& dest)
    {
        if (node.alive())
        {
            MemoryBlock mb;
            if (node.plugin->getState (mb) && mb.getSize() > 0)
                dest = std::move (mb);
        }
    };

    for (size_t s = 0; s < rt.slots.size() && s < def.slots.size(); ++s)
    {
        capture (*rt.slots[s], def.slots[s].state);
        auto& fxRt = rt.slots[s]->effects;
        auto& fxDef = def.slots[s].effects;
        for (size_t e = 0; e < fxRt.size() && e < fxDef.size(); ++e)
            capture (*fxRt[e], fxDef[e].state);
    }
    for (size_t e = 0; e < rt.effects.size() && e < def.effects.size(); ++e)
        capture (*rt.effects[e], def.effects[e].state);
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

    bool ok = true;
    if (auto* rt = getLoaded (inputIndex, program))
    {
        auto slot = buildSlot (inputIndex, program, slotIndex, sd);
        ok = slot->plugin != nullptr;
        error = slot->loadError;
        const ScopedLock sl (lock);
        rt->slots.push_back (std::move (slot));
    }
    notifyContent (inputIndex, program);
    return ok;
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
                {
                    rt->slots[(size_t) s]->slotIndex.store (s);
                    for (auto& e : rt->slots[(size_t) s]->effects) e->slotIndex.store (s);
                }
            }
            resolveMappings (*rt, def);
        }
        if (removed != nullptr)
        {
            for (auto& e : removed->effects) destroyNode (e.get());
            destroyNode (removed.get());
        }
    }
    notifyContent (inputIndex, program);
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

//==============================================================================
// Effects
//==============================================================================
std::vector<std::unique_ptr<Engine::EffectRuntime>>* Engine::runtimeChainFor (ProgramRuntime& rt, int slot) const
{
    if (slot < 0) return &rt.effects;
    return slot < (int) rt.slots.size() ? &rt.slots[(size_t) slot]->effects : nullptr;
}

std::vector<EffectDef>* Engine::defChainFor (ProgramDef& def, int slot) const
{
    if (slot < 0) return &def.effects;
    return slot < (int) def.slots.size() ? &def.slots[(size_t) slot].effects : nullptr;
}

void Engine::renumberChain (std::vector<std::unique_ptr<EffectRuntime>>& chain)
{
    for (int i = 0; i < (int) chain.size(); ++i)
        chain[(size_t) i]->effectIndex.store (i);
}

bool Engine::addEffect (int inputIndex, int program, int slot, const PluginDescription& desc, String& error)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return false;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    auto* chainDef = defChainFor (def, slot);
    if (chainDef == nullptr) return false;

    EffectDef ed;
    ed.plugin = desc;
    chainDef->push_back (ed);
    const int effectIndex = (int) chainDef->size() - 1;

    bool ok = true;
    if (auto* rt = getLoaded (inputIndex, program))
        if (auto* chain = runtimeChainFor (*rt, slot))
        {
            auto fx = buildEffect (inputIndex, program, slot, effectIndex, ed);
            ok = fx->plugin != nullptr;
            error = fx->loadError;
            const ScopedLock sl (lock);
            chain->push_back (std::move (fx));
        }
    notifyContent (inputIndex, program);
    return ok;
}

void Engine::removeEffect (int inputIndex, int program, int slot, int effect)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    auto* chainDef = defChainFor (def, slot);
    if (chainDef == nullptr || effect < 0 || effect >= (int) chainDef->size()) return;

    for (auto it = def.mappings.begin(); it != def.mappings.end();)
    {
        if (it->slot == slot && it->effect == effect)       it = def.mappings.erase (it);
        else { if (it->slot == slot && it->effect > effect) --it->effect; ++it; }
    }
    chainDef->erase (chainDef->begin() + effect);

    if (auto* rt = getLoaded (inputIndex, program))
    {
        std::unique_ptr<EffectRuntime> removed;
        {
            const ScopedLock sl (lock);
            if (auto* chain = runtimeChainFor (*rt, slot))
                if (effect < (int) chain->size())
                {
                    removed = std::move ((*chain)[(size_t) effect]);
                    chain->erase (chain->begin() + effect);
                    renumberChain (*chain);
                }
            resolveMappings (*rt, def);
        }
        destroyNode (removed.get());
    }
    notifyContent (inputIndex, program);
}

void Engine::moveEffect (int inputIndex, int program, int slot, int from, int to)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    auto* chainDef = defChainFor (def, slot);
    if (chainDef == nullptr) return;
    const int n = (int) chainDef->size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;

    auto moveItem = [] (auto& vec, int a, int b)
    {
        auto item = std::move (vec[(size_t) a]);
        vec.erase (vec.begin() + a);
        vec.insert (vec.begin() + b, std::move (item));
    };
    moveItem (*chainDef, from, to);

    for (auto& m : def.mappings)
        if (m.slot == slot && m.effect >= 0)
        {
            if (m.effect == from)                       m.effect = to;
            else if (from < to && m.effect > from && m.effect <= to) --m.effect;
            else if (to < from && m.effect >= to && m.effect < from) ++m.effect;
        }

    if (auto* rt = getLoaded (inputIndex, program))
    {
        const ScopedLock sl (lock);
        if (auto* chain = runtimeChainFor (*rt, slot))
            if (from < (int) chain->size() && to < (int) chain->size())
            {
                moveItem (*chain, from, to);
                renumberChain (*chain);
            }
        resolveMappings (*rt, def);
    }
    notifyContent (inputIndex, program);
}

void Engine::setEffectBypassed (int inputIndex, int program, int slot, int effect, bool b)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    auto* chainDef = defChainFor (def, slot);
    if (chainDef == nullptr || effect < 0 || effect >= (int) chainDef->size()) return;
    (*chainDef)[(size_t) effect].bypassed = b;

    if (auto* rt = getLoaded (inputIndex, program))
        if (auto* chain = runtimeChainFor (*rt, slot))
            if (effect < (int) chain->size())
                (*chain)[(size_t) effect]->bypassed.store (b);
}

Engine::PluginNode* Engine::getNode (int inputIndex, int program, int slot, int effect) const
{
    auto* rt = getLoaded (inputIndex, program);
    if (rt == nullptr) return nullptr;
    if (effect < 0)
    {
        if (slot < 0 || slot >= (int) rt->slots.size()) return nullptr;
        return rt->slots[(size_t) slot].get();
    }
    auto* chain = runtimeChainFor (*rt, slot);
    if (chain == nullptr || effect >= (int) chain->size()) return nullptr;
    return (*chain)[(size_t) effect].get();
}

RemotePlugin* Engine::getPlugin (int inputIndex, int program, int slot, int effect) const
{
    auto* node = getNode (inputIndex, program, slot, effect);
    return node != nullptr ? node->plugin.get() : nullptr;
}

bool Engine::isPluginAlive (int inputIndex, int program, int slot, int effect) const
{
    auto* node = getNode (inputIndex, program, slot, effect);
    return node != nullptr && node->alive();
}

String Engine::getPluginLoadError (int inputIndex, int program, int slot, int effect) const
{
    auto* node = getNode (inputIndex, program, slot, effect);
    if (node == nullptr) return {};
    if (node->plugin != nullptr && ! node->plugin->isAlive())
        return "Plugin process stopped: " + node->plugin->getLastError();
    return node->loadError;
}

void Engine::reloadPlugin (int inputIndex, int program, int slot, int effect)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto* node = getNode (inputIndex, program, slot, effect);
    if (node == nullptr) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];

    const PluginDescription* desc = nullptr;
    const MemoryBlock* state = nullptr;
    if (effect < 0)
    {
        if (slot < 0 || slot >= (int) def.slots.size()) return;
        desc = &def.slots[(size_t) slot].plugin;
        state = &def.slots[(size_t) slot].state;
    }
    else if (auto* chain = defChainFor (def, slot))
    {
        if (effect >= (int) chain->size()) return;
        desc = &(*chain)[(size_t) effect].plugin;
        state = &(*chain)[(size_t) effect].state;
    }
    if (desc == nullptr) return;

    // Detach the old process from the audio thread, then start a fresh one.
    std::unique_ptr<RemotePlugin> old;
    {
        const ScopedLock sl (lock);
        old = std::move (node->plugin);
    }
    if (old != nullptr) { old->setListener (nullptr); old->shutdown(); }

    node->loadError.clear();
    loadPluginInto (*node, *desc, *state);
    {
        const ScopedLock sl (lock);
        if (auto* rt = getLoaded (inputIndex, program))
            resolveMappings (*rt, def);
    }
    notifyContent (inputIndex, program);
}

//==============================================================================
// Mappings
//==============================================================================
void Engine::resolveMappings (ProgramRuntime& rt, const ProgramDef& def)
{
    std::vector<MappingRuntime> resolved;
    for (auto& m : def.mappings)
    {
        MappingRuntime mr;
        mr.def = m;

        PluginNode* node = nullptr;
        if (m.effect < 0)
        {
            if (m.slot >= 0 && m.slot < (int) rt.slots.size())
                node = rt.slots[(size_t) m.slot].get();
        }
        else if (auto* chain = runtimeChainFor (rt, m.slot))
        {
            if (m.effect < (int) chain->size())
                node = (*chain)[(size_t) m.effect].get();
        }

        if (node != nullptr && node->plugin != nullptr)
        {
            mr.plugin = node->plugin.get();
            mr.paramIndex = node->plugin->findParameterIndex (m.paramId);
        }
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
    notifyContent (inputIndex, program);
}

void Engine::updateMapping (int inputIndex, int program, int mappingIndex, const MappingDef& m)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (mappingIndex < 0 || mappingIndex >= (int) def.mappings.size()) return;
    def.mappings[(size_t) mappingIndex] = m;
    if (auto* rt = getLoaded (inputIndex, program)) { const ScopedLock sl (lock); resolveMappings (*rt, def); }
    notifyContent (inputIndex, program);
}

void Engine::removeMapping (int inputIndex, int program, int mappingIndex)
{
    if (! validInput (inputIndex) || ! validProgram (program)) return;
    auto& def = setup.inputs[(size_t) inputIndex].programs[(size_t) program];
    if (mappingIndex < 0 || mappingIndex >= (int) def.mappings.size()) return;
    def.mappings.erase (def.mappings.begin() + mappingIndex);
    if (auto* rt = getLoaded (inputIndex, program)) { const ScopedLock sl (lock); resolveMappings (*rt, def); }
    notifyContent (inputIndex, program);
}

void Engine::setLearnArmed (bool b) { learnArmed.store (b); }

void Engine::noteParameterTouched (int inputIndex, int program, int slot, int effect, int paramIndex)
{
    // Can arrive on any thread; dedupe and hand off to the message thread.
    postEvent ({ Event::touched, inputIndex, program, slot, effect, paramIndex });
}

void Engine::notePluginDied (int inputIndex, int program, int slot, int effect)
{
    postEvent ({ Event::pluginDied, inputIndex, program, slot, effect, 0 });
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
                if (p.type == Event::touched && p.input == e.input && p.a == e.a && p.b == e.b && p.c == e.c && p.d == e.d)
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
                lastTouched = { e.input, e.a, e.b, e.c, e.d };
                listeners.call ([&] (Listener& l) { l.parameterTouched (e.input, e.a, e.b, e.c, e.d); });
                break;

            case Event::pluginDied:
                if (auto* node = getNode (e.input, e.a, e.b, e.c))
                    if (node->plugin != nullptr && ! node->plugin->isAlive())
                    {
                        const auto name = node->plugin->getName();
                        listeners.call ([&] (Listener& l) { l.statusMessage (name + " stopped (" + node->plugin->getLastError() + "). Use Reload on that plugin to restart it."); });
                        notifyContent (e.input, e.a);
                    }
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
                notifyContent (i, p);
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
            postEvent ({ Event::programChange, i, m.getProgramChangeNumber(), 0, 0, 0 });
        return;
    }

    if (learnArmed.load())
    {
        if (m.isController())            { learnArmed = false; postEvent ({ Event::learn, i, (int) MappingDef::Source::CC, m.getControllerNumber(), 0, 0 }); }
        else if (m.isPitchWheel())       { learnArmed = false; postEvent ({ Event::learn, i, (int) MappingDef::Source::PitchBend, 0, 0, 0 }); }
        else if (m.isChannelPressure())  { learnArmed = false; postEvent ({ Event::learn, i, (int) MappingDef::Source::ChannelPressure, 0, 0, 0 }); }
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
        {
            rt->mix.setSize (2, blockSize, false, false, true);
            auto prep = [this] (PluginNode& node) { if (node.alive()) node.plugin->prepare (sampleRate, blockSize); };
            for (auto& s : rt->slots)
            {
                s->stereo.setSize (2, blockSize, false, false, true);
                prep (*s);
                for (auto& e : s->effects) prep (*e);
            }
            for (auto& e : rt->effects) prep (*e);
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

    // Every plugin process must answer within this block; late ones are silenced for it.
    blockDeadline = ipc::monotonicDeadline (jmax (0.001, 0.85 * numSamples / sampleRate));

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

static void addNotesOff (MidiBuffer& midi, bool allSoundOff)
{
    for (int ch = 1; ch <= 16; ++ch)
    {
        midi.addEvent (MidiMessage::controllerEvent (ch, 64, 0), 0);   // sustain off
        midi.addEvent (MidiMessage::allNotesOff (ch), 0);
        if (allSoundOff)
            midi.addEvent (MidiMessage::allSoundOff (ch), 0);
    }
}

void Engine::runChain (std::vector<std::unique_ptr<EffectRuntime>>& chain, const MidiBuffer& midi, AudioBuffer<float>& stereo, int numSamples)
{
    for (auto& fx : chain)
    {
        if (! fx->alive() || fx->bypassed.load()) continue;
        fx->plugin->beginProcess (stereo.getReadPointer (0), stereo.getReadPointer (1), midi, numSamples);
        if (! fx->plugin->finishProcess (stereo.getWritePointer (0), stereo.getWritePointer (1), blockDeadline))
        {
            stereo.clear();
            lateBlocks.fetch_add (1);
        }
    }
}

void Engine::processProgram (ProgramRuntime& prog, const MidiBuffer& in, float* const* out, int numOut, int numSamples, bool panicNow)
{
    for (auto& slot : prog.slots)
        slot->midi.clear();
    prog.programMidi.clear();

    if (prog.needsNotesOff || panicNow)
    {
        prog.needsNotesOff = false;
        for (auto& slot : prog.slots)
            addNotesOff (slot->midi, panicNow);
        addNotesOff (prog.programMidi, panicNow);
    }

    // Route incoming MIDI: mappings first, then per-slot filtering.
    for (const auto meta : in)
    {
        const auto m = meta.getMessage();
        const int pos = meta.samplePosition;

        bool consumed = false;
        for (auto& map : prog.mappings)
        {
            if (map.plugin == nullptr || map.paramIndex < 0 || ! map.plugin->isAlive() || ! mappingMatches (map.def, m)) continue;
            const float norm = normalisedValueOf (m, map.def.source);
            const float value = map.def.minValue + (map.def.maxValue - map.def.minValue) * norm;
            map.plugin->queueParameterChange (map.paramIndex, jlimit (0.0f, 1.0f, value));
            if (! map.def.passThrough) consumed = true;
        }
        if (consumed) continue;

        prog.programMidi.addEvent (m, pos);

        for (auto& slot : prog.slots)
        {
            if (! slot->alive() || ! slot->enabled.load()) continue;

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

    // Render each slot through its chain and sum into the program mix. Instruments run
    // in parallel (one process each); each slot's effect chain then runs in order.
    if (prog.mix.getNumSamples() < numSamples) prog.mix.setSize (2, numSamples, false, false, true);
    AudioBuffer<float> mix (prog.mix.getArrayOfWritePointers(), 2, numSamples);
    mix.clear();

    for (auto& slot : prog.slots)
    {
        if (slot->stereo.getNumSamples() < numSamples) slot->stereo.setSize (2, numSamples, false, false, true);
        if (slot->alive())
            slot->plugin->beginProcess (nullptr, nullptr, slot->midi, numSamples);
    }

    for (auto& slot : prog.slots)
    {
        if (! slot->alive()) continue;
        AudioBuffer<float> stereo (slot->stereo.getArrayOfWritePointers(), 2, numSamples);

        if (! slot->plugin->finishProcess (stereo.getWritePointer (0), stereo.getWritePointer (1), blockDeadline))
        {
            stereo.clear();
            lateBlocks.fetch_add (1);
            continue;
        }
        runChain (slot->effects, slot->midi, stereo, numSamples);

        const float g = slot->enabled.load() ? slot->gain.load() : 0.0f;
        if (g <= 0.0f) continue;
        for (int c = 0; c < 2; ++c)
            mix.addFrom (c, 0, stereo, c, 0, numSamples, g);
    }

    runChain (prog.effects, prog.programMidi, mix, numSamples);

    for (int c = 0; c < jmin (numOut, 2); ++c)
        if (out[c] != nullptr)
            FloatVectorOperations::add (out[c], mix.getReadPointer (c), numSamples);
}

} // namespace perf
