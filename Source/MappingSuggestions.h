#pragma once

#include "Model.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

namespace perf
{

//==============================================================================
/** A proposed mapping plus a short explanation of where it came from. */
struct MappingSuggestion
{
    MappingDef mapping;
    juce::String reason;
};

//==============================================================================
/** Per-plugin default mappings, stored as JSON next to the settings file.
    Keyed by PluginDescription::createIdentifierString(). Entries carry no
    slot/effect; they're applied to whatever target the user picks. */
class MappingTemplates
{
public:
    explicit MappingTemplates (const juce::File& storage);

    bool has (const juce::PluginDescription&) const;
    std::vector<MappingDef> get (const juce::PluginDescription&) const;
    void set (const juce::PluginDescription&, const std::vector<MappingDef>&);
    void remove (const juce::PluginDescription&);
    int size() const { return (int) entries.size(); }

    juce::Result save() const;
    juce::Result load();

    static juce::String keyFor (const juce::PluginDescription& d) { return d.createIdentifierString(); }

private:
    struct Entry { juce::String pluginName; std::vector<MappingDef> mappings; };
    juce::File file;
    std::map<juce::String, Entry> entries;
};

//==============================================================================
/** Stable identifier for a hosted parameter: the plugin's own ID where available,
    else the index as a string. */
juce::String parameterIdOf (const juce::AudioProcessorParameter&);
juce::AudioProcessorParameter* findParameterById (juce::AudioPluginInstance&, const juce::String& paramId);

//==============================================================================
/** Proposes CC mappings for a plugin from its parameter names, using the
    General MIDI Level 2 sound-controller numbers (CC 74 cutoff, 71 resonance,
    73 attack, 72 release, ...). Each CC and each parameter is used at most once. */
std::vector<MappingSuggestion> suggestMappingsFromNames (juce::AudioPluginInstance&);

/** Turns a template into suggestions, dropping entries whose parameter the
    plugin no longer has. */
std::vector<MappingSuggestion> suggestMappingsFromTemplate (juce::AudioPluginInstance&, const std::vector<MappingDef>& tmpl);

/** Full pipeline: template if one exists, otherwise name heuristics. Sets
    slot/effect on every suggestion and drops anything that duplicates a CC or
    parameter already mapped to the same target in `existing`. */
std::vector<MappingSuggestion> suggestMappings (juce::AudioPluginInstance&, const juce::PluginDescription&,
                                                const MappingTemplates&, int slot, int effect,
                                                const std::vector<MappingDef>& existing);

} // namespace perf
