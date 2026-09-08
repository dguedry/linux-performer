#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

namespace perf
{

//==============================================================================
/** An effect plugin in an insert chain (per slot, or per program). */
struct EffectDef
{
    juce::PluginDescription plugin;
    juce::MemoryBlock state;
    bool bypassed = false;

    juce::var toVar() const;
    static EffectDef fromVar (const juce::var&);
};

//==============================================================================
/** One instrument inside a program, with its own insert-effect chain. */
struct SlotDef
{
    juce::PluginDescription plugin;   // which plugin
    juce::MemoryBlock state;          // last saved plugin state
    bool enabled = true;
    float gainDb = 0.0f;
    int transpose = 0;                // semitones
    int lowKey = 0, highKey = 127;    // key zone
    int outChannel = 0;               // 0 = keep incoming channel, 1..16 = force
    std::vector<EffectDef> effects;   // processed in order after the instrument

    juce::var toVar() const;
    static SlotDef fromVar (const juce::var&);
};

//==============================================================================
/** Maps a MIDI controller message to a plugin parameter. */
struct MappingDef
{
    enum class Source : int { CC = 0, PitchBend = 1, ChannelPressure = 2 };

    Source source = Source::CC;
    int number = 1;                   // CC number (ignored for other sources)
    int slot = 0;                     // index into ProgramDef::slots, or -1 for the program chain
    int effect = -1;                  // -1 = the instrument itself, else index into the effect chain
    juce::String paramId;             // HostedAudioProcessorParameter ID (or index as string)
    juce::String paramName;           // for display only
    float minValue = 0.0f, maxValue = 1.0f;
    bool passThrough = false;         // also forward the raw message to the plugins

    juce::String sourceDescription() const;

    juce::var toVar() const;
    static MappingDef fromVar (const juce::var&);
};

//==============================================================================
struct ProgramDef
{
    juce::String name;
    std::vector<SlotDef> slots;
    std::vector<EffectDef> effects;   // program-level chain, applied to the summed slots
    std::vector<MappingDef> mappings;

    bool isEmpty() const { return slots.empty() && effects.empty(); }

    /** Returns the chain a mapping/effect index refers to, or nullptr. */
    const std::vector<EffectDef>* chainFor (int slot) const
    {
        if (slot < 0) return &effects;
        return slot < (int) slots.size() ? &slots[(size_t) slot].effects : nullptr;
    }

    juce::var toVar() const;
    static ProgramDef fromVar (const juce::var&);
};

//==============================================================================
/** A MIDI source such as "Upper" or "Lower" keyboard. */
struct InputDef
{
    static constexpr int numPrograms = 128;

    juce::String name { "Input" };
    juce::String midiDeviceIdentifier;
    juce::String midiDeviceName;      // used to re-find a device if identifiers change
    int channel = 0;                  // 0 = omni
    bool respondToProgramChange = true;
    int currentProgram = 0;
    std::vector<ProgramDef> programs { (size_t) numPrograms };

    juce::var toVar() const;
    static InputDef fromVar (const juce::var&);
};

//==============================================================================
struct Setup
{
    std::vector<InputDef> inputs;
    bool preloadAllPrograms = false;  // keep every used program's plugins resident
    double releaseTailSeconds = 4.0;  // how long the outgoing program keeps sounding

    juce::var toVar() const;
    static Setup fromVar (const juce::var&);

    juce::Result saveToFile (const juce::File&) const;
    static juce::Result loadFromFile (const juce::File&, Setup& out);

    static Setup makeDefault();
};

} // namespace perf
