#pragma once

#include "Model.h"
#include "PluginHost.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <map>
#include <memory>
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
        /** Called synchronously before a plugin instance is destroyed (close its editor!). */
        virtual void instanceAboutToBeDeleted (juce::AudioPluginInstance*) {}
        virtual void statusMessage (const juce::String&) {}
    };

    /** @param startAudioDevice  pass false for headless tests; blocks are then rendered
                                 manually with renderBlockForTesting(). */
    Engine (PluginHost&, juce::PropertiesFile& settings, bool startAudioDevice = true);
    ~Engine() override;

    juce::AudioDeviceManager& getDeviceManager()    { return deviceManager; }
    PluginHost& getPluginHost()                     { return host; }

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
    void setSlotOutChannel(int inputIndex, int program, int slot, int);

    //==============================================================================
    // Effects. slot == -1 addresses the program-level chain.
    bool addEffect (int inputIndex, int program, int slot, const juce::PluginDescription&, juce::String& error);
    void removeEffect (int inputIndex, int program, int slot, int effect);
    /** Moves an effect within its chain. */
    void moveEffect (int inputIndex, int program, int slot, int from, int to);
    void setEffectBypassed (int inputIndex, int program, int slot, int effect, bool);

    /** Any plugin in a program: effect == -1 is the instrument of `slot`; slot == -1 the
        program chain. Returns nullptr if the program isn't loaded or the plugin failed. */
    juce::AudioPluginInstance* getPluginInstance (int inputIndex, int program, int slot, int effect = -1) const;
    juce::String getPluginLoadError (int inputIndex, int program, int slot, int effect = -1) const;
    juce::AudioPluginInstance* getSlotInstance (int inputIndex, int program, int slot) const { return getPluginInstance (inputIndex, program, slot, -1); }
    juce::String getSlotLoadError (int inputIndex, int program, int slot) const              { return getPluginLoadError (inputIndex, program, slot, -1); }

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

    static juce::String getParameterId (const juce::AudioProcessorParameter&);
    static juce::AudioProcessorParameter* findParameter (juce::AudioPluginInstance&, const juce::String& paramId);

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
        enum Type { programChange, learn, touched };
        Type type; int input = 0, a = 0, b = 0, c = 0, d = 0;
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
    void loadPluginInto (PluginNode&, const juce::PluginDescription&, const juce::MemoryBlock& state);
    void unloadProgram (InputRuntime&, int program);
    void destroyProgramPlugins (ProgramRuntime&);
    void destroyNode (PluginNode*);
    void captureProgramState (int inputIndex, ProgramRuntime&);
    void resolveMappings (ProgramRuntime&, const ProgramDef&);
    void prepareInstance (juce::AudioPluginInstance&);
    void openMidiDevices();
    void housekeeping();
    std::vector<std::unique_ptr<EffectRuntime>>* runtimeChainFor (ProgramRuntime&, int slot) const;
    std::vector<EffectDef>* defChainFor (ProgramDef&, int slot) const;
    void renumberChain (std::vector<std::unique_ptr<EffectRuntime>>&);

    // audio thread
    void processInput (InputRuntime&, float* const* out, int numOut, int numSamples, bool panic);
    void processProgram (ProgramRuntime&, const juce::MidiBuffer& in, float* const* out, int numOut, int numSamples, bool panic);
    void renderNode (PluginNode&, juce::AudioBuffer<float>& stereo, int numSamples, bool feedStereoIn);
    void runChain (std::vector<std::unique_ptr<EffectRuntime>>&, const juce::MidiBuffer& midi, juce::AudioBuffer<float>& stereo, int numSamples);

    bool validInput (int i) const     { return i >= 0 && i < (int) setup.inputs.size(); }
    bool validProgram (int p) const   { return p >= 0 && p < InputDef::numPrograms; }
    ProgramRuntime* getLoaded (int inputIndex, int program) const;
    PluginNode* getNode (int inputIndex, int program, int slot, int effect) const;

    void noteParameterTouched (int inputIndex, int program, int slot, int effect, int paramIndex);
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
    std::atomic<bool> suppressTouch { false };
    TouchedParam lastTouched;

    juce::ListenerList<Listener> listeners;

    friend struct PluginNode;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};

} // namespace perf
