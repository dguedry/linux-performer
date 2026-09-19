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
    int lowVelocity = 1, highVelocity = 127;   // note-ons outside are dropped (velocity layers)
    float velocityCurve = 0.0f;       // -1..1: <0 softer, 0 linear, >0 louder for the same touch
    float pan = 0.0f;                 // -1 (left) .. 1 (right); a balance on the stereo output, unity at centre
    int outChannel = 0;               // 0 = keep incoming channel, 1..16 = force
    std::vector<EffectDef> effects;   // processed in order after the instrument

    /** Parameter IDs shown as controls on the phone, for THIS instance.

        Per instance rather than per plugin: two Kontakts in one setup are two
        different instruments, and choosing a filter cutoff on the strings
        should not put the same control on the drums. The per-plugin list is
        still used, but only as the starting point when a slot has chosen
        nothing yet. */
    std::vector<juce::String> phoneControls;

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

    /** Free-text category: "Organs", "Strings", "Brass". Purely a label for
        finding things -- a program's number is fixed by MIDI Program Change, so
        a group can never move or renumber it. Empty means ungrouped.

        Per input, like everything else about a program: 007 on Upper has nothing
        to do with 007 on Lower, and neither do their groups. */
    juce::String group;
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
    /** Which channel carries Program Change for this input. Keyboards differ: most
        send it on the channel they play on, but a workstation may use a global
        channel (a Roland Jupiter-50 sends registrations on 16 while playing on
        1/3/4), and some send it on a channel you cannot change. -1 = the same
        channel as the notes (the default, and what every keyboard that does the
        usual thing wants), 0 = any channel on this input's MIDI port, 1..16 = that
        channel only. */
    static constexpr int pcChannelSameAsNotes = -1;
    static constexpr int pcChannelAny = 0;
    int programChangeChannel = pcChannelSameAsNotes;
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

    /** Tempo handed to every plugin, for tempo-synced delays and arpeggiators.
        One tempo for the whole rig rather than one per program: a band plays a
        song at a tempo, not an instrument. */
    double tempoBpm = 120.0;

    /** Controller number that taps the tempo, or 0 for none. Global for the same
        reason the tempo is: a footswitch should work whatever is loaded. */
    int tapTempoCC = 0;

    juce::var toVar() const;
    static Setup fromVar (const juce::var&);

    juce::Result saveToFile (const juce::File&) const;
    static juce::Result loadFromFile (const juce::File&, Setup& out);

    static Setup makeDefault();
};

} // namespace perf
