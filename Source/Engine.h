#pragma once

#include "Model.h"
#include "PluginHost.h"
#include "RemotePlugin.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace perf
{

/**
    The audio/MIDI engine.

    Owns the Setup document plus the live plugin instances for every loaded program.
    All public methods must be called from the message thread. Audio and MIDI
    callbacks run on their own threads and touch only the runtime structures,
    guarded by a single lock that is held for the duration of each audio block.

    Signal flow per program:
        for each slot:   MIDI -> instrument -> [effect chain] -> * gain  --+
                                                                            +-> sum -> [program effect chain] -> out
    Plugins are addressed by (slot, effect): slot >= 0 selects an instrument slot,
    slot == -1 the program-level chain; effect == -1 means the instrument itself.
*/
class Engine : private juce::AudioIODeviceCallback,
               private juce::MidiInputCallback,
               private juce::AsyncUpdater,
               private juce::Timer,
               private juce::ChangeListener
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        /** Inputs added/removed or a whole setup loaded. */
        virtual void setupChanged() {}
        /** The active program of an input changed (via MIDI or UI). */
        virtual void programChanged (int inputIndex, int program) { juce::ignoreUnused (inputIndex, program); }
        /** Slots, effects or mappings of a program were edited, or its load state changed. */
        virtual void programContentChanged (int inputIndex, int program) { juce::ignoreUnused (inputIndex, program); }
        /** A MIDI controller arrived while learn mode was armed. */
        virtual void learnReceived (int inputIndex, MappingDef::Source source, int number) { juce::ignoreUnused (inputIndex, source, number); }
        /** The user touched a parameter in a plugin GUI (effect == -1 for an instrument). */
        virtual void parameterTouched (int inputIndex, int program, int slot, int effect, int paramIndex) { juce::ignoreUnused (inputIndex, program, slot, effect, paramIndex); }
        virtual void statusMessage (const juce::String&) {}
    };

    /** @param startAudioDevice  pass false for headless tests; blocks are then rendered
                                 manually with renderBlockForTesting(). */
    Engine (PluginHost&, juce::PropertiesFile& settings, bool startAudioDevice = true);
    ~Engine() override;

    juce::AudioDeviceManager& getDeviceManager()    { return deviceManager; }
    PluginHost& getPluginHost()                     { return host; }

    /** Closes and reopens the current audio device (picks up a new PIPEWIRE_LATENCY). */
    void restartAudioDevice();

    //==============================================================================
    // Setup document
    const Setup& getSetup() const                   { return setup; }
    void loadSetup (Setup newSetup);
    /** Pulls the live plugin states into the model and returns a snapshot for saving. */
    Setup captureSetup();
    void setPreloadAllPrograms (bool);
    void setReleaseTailSeconds (double);

    //==============================================================================
    // Inputs
    int  addInput (const juce::String& name);
    void removeInput (int inputIndex);
    void setInputName (int inputIndex, const juce::String&);
    void setInputMidiDevice (int inputIndex, const juce::MidiDeviceInfo&);
    void setInputChannel (int inputIndex, int channel);
    void setInputRespondToProgramChange (int inputIndex, bool);
    /** Re-scans MIDI devices and re-opens the ones the setup refers to (hot-plug). */
    void refreshMidiDevices();

    //==============================================================================
    // Programs
    void selectProgram (int inputIndex, int program);
    void setProgramName (int inputIndex, int program, const juce::String&);
    bool isProgramLoaded (int inputIndex, int program) const;
    void copyProgram (int inputIndex, int from, int to);
    void clearProgram (int inputIndex, int program);

    //==============================================================================
    // Slots (instruments)
    bool addSlot (int inputIndex, int program, const juce::PluginDescription&, juce::String& error);
    void removeSlot (int inputIndex, int program, int slot);
    void setSlotEnabled   (int inputIndex, int program, int slot, bool);
    void setSlotGainDb    (int inputIndex, int program, int slot, float);
    void setSlotTranspose (int inputIndex, int program, int slot, int);
    void setSlotKeyRange  (int inputIndex, int program, int slot, int low, int high);
    void setSlotVelocityRange (int inputIndex, int program, int slot, int low, int high);
    void setSlotVelocityCurve (int inputIndex, int program, int slot, float curve);   // -1..1
    void setSlotPan       (int inputIndex, int program, int slot, float pan);         // -1..1
    /** Velocity after a slot's curve: 0 leaves it, >0 lifts soft playing, <0 tames it. 1..127 in and out. */
    static int curveVelocity (int velocity, float curve);
    /** Left/right gains for a pan position: a balance law, unity in the centre. */
    static void panGains (float pan, float& left, float& right);
    void setSlotOutChannel(int inputIndex, int program, int slot, int);

    //==============================================================================
    // Effects. slot == -1 addresses the program-level chain.
    bool addEffect (int inputIndex, int program, int slot, const juce::PluginDescription&, juce::String& error);
    void removeEffect (int inputIndex, int program, int slot, int effect);
    /** Moves an effect within its chain. */
    void moveEffect (int inputIndex, int program, int slot, int from, int to);
    void setEffectBypassed (int inputIndex, int program, int slot, int effect, bool);

    /** Any plugin in a program: effect == -1 is the instrument of `slot`; slot == -1 the
        program chain. Returns nullptr if the program isn't loaded or the plugin failed to
        start; a plugin whose process died is returned but reports isAlive() == false. */
    RemotePlugin* getPlugin (int inputIndex, int program, int slot, int effect = -1) const;
    bool isPluginAlive (int inputIndex, int program, int slot, int effect = -1) const;
    /** True while the plugin's process is being started on the loader thread. */
    bool isPluginLoading (int inputIndex, int program, int slot, int effect = -1) const;
    /** True while any plugin is still loading. */
    bool hasPendingLoads() const                    { return pendingLoads.load() > 0; }
    /** Plugins being loaded right now (queued ones not counted). */
    int getLoadsInFlight() const                    { return loadsInFlight.load(); }
    int getParallelLoads() const                    { return loaderThreadCount; }
    juce::String getPluginLoadError (int inputIndex, int program, int slot, int effect = -1) const;
    /** Restarts a plugin that failed or crashed, from its saved definition and state. */
    void reloadPlugin (int inputIndex, int program, int slot, int effect = -1);

    //==============================================================================
    // Mappings
    void addMapping    (int inputIndex, int program, const MappingDef&);
    void updateMapping (int inputIndex, int program, int mappingIndex, const MappingDef&);
    void removeMapping (int inputIndex, int program, int mappingIndex);

    /** Arms MIDI learn: the next CC / pitch bend / aftertouch on any input fires
        Listener::learnReceived and disarms. */
    void setLearnArmed (bool);
    bool isLearnArmed() const                       { return learnArmed.load(); }

    struct TouchedParam { int inputIndex = -1, program = -1, slot = -1, effect = -1, paramIndex = -1; bool valid() const { return paramIndex >= 0; } };
    TouchedParam getLastTouchedParam() const        { return lastTouched; }

    //==============================================================================
    /** All notes off + all sound off on every loaded plugin. */
    void panic();

    /** Feeds a MIDI message into an input as if it had arrived from its device
        (channel filter, program change and learn handling all apply). Thread-safe. */
    void injectMidi (int inputIndex, const juce::MidiMessage&);

    /** Renders one block exactly as the audio callback would. Only for tests /
        offline use when constructed with startAudioDevice = false. */
    void renderBlockForTesting (float* const* out, int numOut, int numSamples);

    double getSampleRate() const                    { return sampleRate; }
    double getCpuUsage() const                      { return deviceManager.getCpuUsage(); }
    /** Blocks in which at least one plugin process missed its deadline. */
    int getLateBlockCount() const                   { return lateBlocks.load(); }
    /** Samples per block actually delivered by the device (PipeWire's JACK
        client reports its default 1024 via jack_get_buffer_size even while
        the graph runs a smaller quantum; this is the real number). 0 until
        the first callback. */
    int getLastBlockSize() const                    { return lastBlockSamples.load(); }

    void addListener (Listener* l)                  { listeners.add (l); }
    void removeListener (Listener* l)               { listeners.remove (l); }

private:
    //==============================================================================
    struct PluginNode;
    struct SlotRuntime;
    struct EffectRuntime;
    struct MappingRuntime;
    struct ProgramRuntime;
    struct InputRuntime;

    struct Event
    {
        enum Type { programChange, learn, touched, pluginDied, pluginLoaded };
        Type type; int input = 0, a = 0, b = 0, c = 0, d = 0;
    };

    /** Plugins load on a background thread (starting Kontakt takes ~10 s; a Wine stall
        can take far longer) so program changes and preloading never block the UI. */
    struct LoadJob
    {
        uint64_t nodeId;
        juce::PluginDescription desc;
        juce::MemoryBlock state;
        double sampleRate;
        int blockSize;
        bool bridged = false;    // Wine/yabridge plugin: limited concurrency (see takeJob)
    };
    struct LoadResult
    {
        uint64_t nodeId;
        std::unique_ptr<RemotePlugin> plugin;
        juce::String error;
        double sampleRate;
        int blockSize;
    };

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    // MidiInputCallback
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void routeMidi (InputRuntime&, int inputIndex, const juce::MidiMessage&);   // caller holds lock

    // AsyncUpdater / Timer / ChangeListener
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void postEvent (const Event&);

    // runtime management (message thread)
    void rebuildRuntimes();
    void teardownRuntimes();
    ProgramRuntime* ensureProgramLoaded (int inputIndex, int program);
    std::unique_ptr<ProgramRuntime> buildProgram (int inputIndex, int program);
    std::unique_ptr<SlotRuntime> buildSlot (int inputIndex, int program, int slotIndex, const SlotDef&);
    std::unique_ptr<EffectRuntime> buildEffect (int inputIndex, int program, int slotIndex, int effectIndex, const EffectDef&);
    void queueLoad (PluginNode&, const juce::PluginDescription&, const juce::MemoryBlock& state);
    void loaderThreadFunc();
    void attachLoaded (LoadResult&);
    void unloadProgram (InputRuntime&, int program);
    void destroyProgramPlugins (ProgramRuntime&);
    void destroyNode (PluginNode*);
    void captureProgramState (int inputIndex, ProgramRuntime&);
    void resolveMappings (ProgramRuntime&, const ProgramDef&);
    void openMidiDevices();
    void housekeeping();
    std::vector<std::unique_ptr<EffectRuntime>>* runtimeChainFor (ProgramRuntime&, int slot) const;
    std::vector<EffectDef>* defChainFor (ProgramDef&, int slot) const;
    void renumberChain (std::vector<std::unique_ptr<EffectRuntime>>&);

    // audio thread
    void processInput (InputRuntime&, float* const* out, int numOut, int numSamples, bool panic);
    void processProgram (ProgramRuntime&, const juce::MidiBuffer& in, float* const* out, int numOut, int numSamples, bool panic);
    void runChain (std::vector<std::unique_ptr<EffectRuntime>>&, const juce::MidiBuffer& midi, juce::AudioBuffer<float>& stereo, int numSamples);

    bool validInput (int i) const     { return i >= 0 && i < (int) setup.inputs.size(); }
    bool validProgram (int p) const   { return p >= 0 && p < InputDef::numPrograms; }
    ProgramRuntime* getLoaded (int inputIndex, int program) const;
    PluginNode* getNode (int inputIndex, int program, int slot, int effect) const;

    void noteParameterTouched (int inputIndex, int program, int slot, int effect, int paramIndex);
    void notePluginDied (int inputIndex, int program, int slot, int effect);
    void notifyContent (int inputIndex, int program);

    //==============================================================================
    PluginHost& host;
    juce::PropertiesFile& settings;
    juce::AudioDeviceManager deviceManager;

    Setup setup;
    std::vector<std::unique_ptr<InputRuntime>> runtimes;

    juce::CriticalSection lock;          // guards runtimes for audio + midi threads
    juce::CriticalSection eventLock;
    std::vector<Event> pendingEvents;

    double sampleRate = 44100.0;
    int blockSize = 512;

    std::atomic<bool> learnArmed { false };
    std::atomic<bool> panicRequested { false };
    std::atomic<int> lateBlocks { 0 };
    std::atomic<int> lastBlockSamples { 0 };
    timespec blockDeadline {};
    TouchedParam lastTouched;

    juce::ListenerList<Listener> listeners;

    // background loader: a pool of threads, each plugin is its own process anyway.
    // Wine-bridged plugins start one at a time until the first has come up (the
    // prefix's wineserver and services boot on that first start; concurrent Wine
    // start-ups stall), then at most kBridgedParallel at once.
    int bridgedParallel = 2;                      // settings "parallelBridgedLoads"
    int loaderThreadCount = 4;                    // settings "parallelLoads"
    std::vector<std::thread> loaderThreads;
    std::mutex loaderMutex;
    std::condition_variable loaderCv;
    std::deque<LoadJob> loadQueue;
    std::vector<LoadResult> loadResults;
    std::vector<RemotePlugin*> loadsInProgress;   // guarded by loaderMutex
    int bridgedInFlight = 0;                      // guarded by loaderMutex
    bool bridgedWarm = false;                     // Wine is up: a wineserver was already running, or one bridged plugin has loaded
    static bool wineserverRunning();
    std::atomic<bool> loaderQuit { false };
    std::atomic<int> pendingLoads { 0 };
    std::atomic<int> loadsInFlight { 0 };
    bool takeJob (LoadJob&);                      // called with loaderMutex held
    std::map<uint64_t, PluginNode*> nodeRegistry;   // message thread only

    friend struct PluginNode;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};

} // namespace perf
