#pragma once

#include "Model.h"
#include "ParamInfo.h"
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
/** Index of the parameter with this ID (or numeric index as text), or -1. */
int findParamIndex (const ParamInfoList&, const juce::String& paramId);

//==============================================================================
/** Proposes CC mappings for a plugin from its parameter names, using the
    General MIDI Level 2 sound-controller numbers (CC 74 cutoff, 71 resonance,
    73 attack, 72 release, ...). Each CC and each parameter is used at most once. */
std::vector<MappingSuggestion> suggestMappingsFromNames (const ParamInfoList&);

/** Turns a template into suggestions, dropping entries whose parameter the
    plugin no longer has. */
std::vector<MappingSuggestion> suggestMappingsFromTemplate (const ParamInfoList&, const std::vector<MappingDef>& tmpl);

/** Full pipeline: template if one exists, otherwise name heuristics. Sets
    slot/effect on every suggestion and drops anything that duplicates a CC or
    parameter already mapped to the same target in `existing`. */
std::vector<MappingSuggestion> suggestMappings (const ParamInfoList&, const juce::PluginDescription&,
                                                const MappingTemplates&, int slot, int effect,
                                                const std::vector<MappingDef>& existing);

} // namespace perf
