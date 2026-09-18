#pragma once

#include "ParamInfo.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>
#include <vector>

namespace perf
{

/** The handful of parameters worth reaching for on a phone, per plugin.

    A plugin can publish thousands of parameters -- Kontakt reports 4145, of
    which 2096 are per-channel MIDI controller proxies -- so showing all of them
    is useless on a touchscreen. The user picks a few and those become sliders.

    Keyed per plugin rather than per program, so choosing the drawbars on a B-3X
    once makes them appear wherever that plugin is loaded. Stored beside the
    settings, next to the per-plugin mapping templates, which this deliberately
    mirrors.

    Parameters are stored by ID, never by index: an updated plugin can renumber
    its parameters, and a slider labelled "Leslie Speed" that silently moves
    reverb is worse than one that disappears. */
class Favourites
{
public:
    explicit Favourites (const juce::File& storage);

    /** Parameter IDs chosen for this plugin, in the order they should appear. */
    std::vector<juce::String> get (const juce::PluginDescription&) const;
    void set (const juce::PluginDescription&, const std::vector<juce::String>& paramIds);

    /** Adds or removes one parameter, keeping the rest in order. */
    void add (const juce::PluginDescription&, const juce::String& paramId);
    void remove (const juce::PluginDescription&, const juce::String& paramId);
    bool contains (const juce::PluginDescription&, const juce::String& paramId) const;

    bool hasAny (const juce::PluginDescription&) const;

    juce::Result save() const;
    juce::Result load();

    static juce::String keyFor (const juce::PluginDescription& d) { return d.createIdentifierString(); }

private:
    struct Entry
    {
        juce::String pluginName;          // for a readable file, not used as a key
        std::vector<juce::String> paramIds;
    };

    juce::File file;
    std::map<juce::String, Entry> entries;
};

} // namespace perf
