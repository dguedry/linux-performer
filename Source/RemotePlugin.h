#pragma once

#include "ParamInfo.h"
#include "Ipc/Protocol.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace perf
{

/**
    A plugin running in its own performer-plugin-host process.

    Non-realtime methods (load, state, parameters, editor) talk over a socket and
    block the calling thread for at most their timeout. Real-time methods
    (queueParameterChange / beginProcess / finishProcess) use shared memory and
    never block past the deadline the caller supplies; a plugin that misses its
    deadline simply contributes silence for that block.
*/
class RemotePlugin
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        // All of these arrive on the plugin's reader thread.
        virtual void remoteParameterTouched (RemotePlugin&, int index, float value) { juce::ignoreUnused (index, value); }
        virtual void remoteParameterChanged (RemotePlugin&, int index, float value) { juce::ignoreUnused (index, value); }
        virtual void remoteEditorClosed (RemotePlugin&) {}
        virtual void remoteDied (RemotePlugin&) {}
    };

    RemotePlugin();
    ~RemotePlugin();

    /** Finds the helper executable: $PERFORMER_PLUGIN_HOST, next to this executable,
        or in the sibling JUCE artefacts folder of a build tree. */
    static juce::File findHostExecutable();

    /** Environment for helper processes. yabridge starts `wine` from PATH; tools like
        nilinux install a ~/.local/bin/wine shim (and WINELOADER via environment.d) so
        that plugins run with the wine that owns their prefix. Desktop launchers may not
        have either, so the helper's environment gets them applied explicitly. The
        returned strings are "NAME=value"; `pointers` receives a null-terminated envp. */
    static std::vector<std::string> buildHelperEnvironment (std::vector<char*>& pointers);

    /** Spawns the host process and loads the plugin. Blocks until done or timed out. */
    bool load (const juce::PluginDescription&, double sampleRate, int blockSize, juce::String& error);
    bool prepare (double sampleRate, int blockSize);

    const juce::PluginDescription& getDescription() const   { return description; }
    juce::String getName() const                            { return name; }
    bool isInstrument() const                               { return instrument; }
    bool hasEditor() const                                  { return editorAvailable; }
    bool isAlive() const                                    { return alive.load(); }
    juce::String getLastError() const;

    // Parameters -------------------------------------------------------------------
    const ParamInfoList& getParameters() const              { return params; }
    int findParameterIndex (const juce::String& id) const;
    float getCachedParameterValue (int index) const;
    /** Round trip to the plugin for the current value. */
    bool fetchParameterValue (int index, float& value);

    /** The same, but gives up almost immediately. For polling on the message
        thread, where a plugin that has stopped answering must cost a frame
        rather than freeze the window for the usual ten-second timeout. */
    bool pollParameterValue (int index, float& value);
    /** Non-realtime set (UI / tests). */
    bool setParameterValue (int index, float value);

    // State / editor --------------------------------------------------------------------
    bool getState (juce::MemoryBlock&);
    bool setState (const juce::MemoryBlock&);
    bool showEditor (const juce::String& title);
    bool hideEditor();
    bool isEditorOpen() const                               { return editorOpen.load(); }

    void setListener (Listener* l)                          { listener = l; }

    // Real-time ------------------------------------------------------------------------------
    /** Queue a parameter change to be applied with the next block. Audio thread only. */
    void queueParameterChange (int index, float value);
    /** Hands a block to the plugin. Input may be nullptr for instruments. Audio thread only. */
    void beginProcess (const float* inL, const float* inR, const juce::MidiBuffer&, int numSamples);

    /** The tempo reported to the plugin. Set from the message thread; the audio
        thread copies it into each block, so a change costs nothing per block. */
    void setTempo (double bpm, int timeSigNumerator, int timeSigDenominator);
    /** Waits for the block until `deadline` (CLOCK_MONOTONIC). Returns false if the
        plugin is late or dead; the caller must then treat the output as silence. */
    bool finishProcess (float* outL, float* outR, const timespec& deadline);
    int getMissedBlocks() const                             { return missedBlocks.load(); }

    /** Asks the process to quit and reaps it (or kills it after a grace period). */
    void shutdown();

private:
    bool request (ipc::Msg, const juce::MemoryBlock& payload, int timeoutMs, juce::MemoryBlock* response = nullptr);
    void readerLoop();
    void handleNotification (const ipc::FrameHeader&, const juce::MemoryBlock&);
    void markDead (const juce::String& why);
    bool spawn (juce::String& error);
    void drainDoneSemaphore();

    struct Pending
    {
        juce::WaitableEvent event;
        juce::MemoryBlock response;
        bool ok = false;
    };

    juce::PluginDescription description;
    juce::String name, shmName;
    bool instrument = false, editorAvailable = false;
    ParamInfoList params;
    std::unique_ptr<std::atomic<float>[]> paramValues;   // sized after load

    pid_t pid = -1;
    int socketFd = -1;
    ipc::SharedBlock* shm = nullptr;

    std::thread reader;
    std::mutex sendMutex, pendingMutex;
    mutable std::mutex errorMutex;
    std::map<uint32_t, Pending*> pending;
    std::atomic<uint32_t> nextRequestId { 1 };
    std::atomic<bool> alive { false }, editorOpen { false };
    juce::String lastError;
    Listener* listener = nullptr;

    // audio-thread state
    bool blockInFlight = false;
    int inFlightSamples = 0;
    uint32_t pendingParamCount = 0;
    ipc::ParamChange pendingParams[ipc::kMaxParamChanges];
    std::atomic<int> missedBlocks { 0 };

    // Tempo: written by the message thread, read by the audio thread each block.
    std::atomic<double> tempoBpm { 120.0 };
    std::atomic<int> tempoNumerator { 4 }, tempoDenominator { 4 };
    double ppqPosition = 0.0;              // audio thread only
    double currentSampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RemotePlugin)
};

} // namespace perf
