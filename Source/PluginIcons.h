#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_graphics/juce_graphics.h>
#include <map>

namespace perf
{

/** Icons for plugins.

    No plugin format defines an icon, so this takes what exists and fills the
    rest: a Windows plugin (yabridge bundle) may embed an icon resource in its
    PE file, a native VST3 bundle may ship a GUI snapshot, and everything else
    gets a generated badge (manufacturer colour, plugin initials) that stays the
    same across runs. Native images are cached as PNG under cacheDir. */
class PluginIcons
{
public:
    explicit PluginIcons (const juce::File& cacheDir);

    /** Square image of `size` pixels; never null. */
    juce::Image get (const juce::PluginDescription&, int size);

    /** The plugin's own image, or a null Image when it has none. */
    juce::Image native (const juce::PluginDescription&);

    /** Generated fallback: colour from the manufacturer name, initials of the plugin name. */
    static juce::Image badge (const juce::PluginDescription&, int size);
    static juce::String initials (const juce::String& pluginName);
    static juce::Colour badgeColour (const juce::String& manufacturer);

    /** The Windows binary behind a yabridge bundle (the x86_64-win symlink's target), or a non-existent File. */
    static juce::File windowsBinaryFor (const juce::PluginDescription&);
    /** Largest icon (up to 256 px) embedded in a PE file's RT_GROUP_ICON/RT_ICON resources, or null. */
    static juce::Image fromWindowsBinary (const juce::File& pe);
    /** A native VST3 bundle's Contents/Resources/Snapshots image, or null. */
    static juce::Image fromVst3Snapshot (const juce::File& bundle);

private:
    juce::File cacheDir;
    std::map<juce::String, juce::Image> memo;    // key: identifier string; null Image = known to have none
};

} // namespace perf
