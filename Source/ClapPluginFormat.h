#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace perf
{

/**
    CLAP hosting.

    JUCE has no CLAP implementation -- it ships AudioUnit, LADSPA, LV2, VST and
    VST3 and nothing else -- so this is a format written against the CLAP C API
    directly. It is deliberately shaped like JUCE's own formats so the rest of
    Performer (scanning, the known-plugin list, the out-of-process host) does
    not have to know which format a plugin is.

    CLAP matters on Linux because it is where new plugins increasingly appear
    first, and because yabridge bridges Windows CLAP plugins the same way it
    bridges VST3 -- so a CLAP host gets both.

    On Linux a .clap bundle is a plain shared object exporting one symbol,
    `clap_entry`. Scanning is: load it, ask its plugin factory how many plugins
    it holds, and read each descriptor. A bundle can hold several (Surge XT
    ships the synth and its effects rack in one file), which is why a
    description's identifier has to carry the plugin id as well as the path.
*/
class ClapPluginFormat : public juce::AudioPluginFormat
{
public:
    ClapPluginFormat();
    ~ClapPluginFormat() override;

    static juce::String getFormatName()                 { return "CLAP"; }
    juce::String getName() const override               { return getFormatName(); }

    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&,
                              const juce::String& fileOrIdentifier) override;

    bool fileMightContainThisPluginType (const juce::String& fileOrIdentifier) override;
    juce::String getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier) override;
    bool pluginNeedsRescanning (const juce::PluginDescription&) override;
    bool doesPluginStillExist (const juce::PluginDescription&) override;
    bool canScanForPlugins() const override             { return true; }
    bool isTrivialToScan() const override               { return false; }

    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool recursive, bool) override;
    juce::FileSearchPath getDefaultLocationsToSearch() override;

    /** False, and it matters: JUCE's synchronous createPluginInstance refuses
        outright for any format that claims otherwise, which is the call the
        out-of-process host makes. Creation here really is synchronous -- it
        dlopens the bundle and returns, never waiting on the message thread --
        so there is nothing to be unblocked for. */
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override
    {
        return false;
    }

    /** A description's fileOrIdentifier is "<path>|<clap plugin id>": a bundle
        can hold several plugins, so the path alone does not name one. These
        split and join that. */
    static juce::String makeIdentifier (const juce::String& path, const juce::String& pluginId);
    static juce::String pathFromIdentifier (const juce::String& identifier);
    static juce::String pluginIdFromIdentifier (const juce::String& identifier);

    /** Opens a bundle and instantiates one plugin from it, or returns null with
        `error` set. Public so the out-of-process host can use it directly. */
    static std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription&,
                                                                      double sampleRate, int blockSize,
                                                                      juce::String& error);

private:
    void createPluginInstance (const juce::PluginDescription&, double initialSampleRate,
                               int initialBufferSize, PluginCreationCallback) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClapPluginFormat)
};

} // namespace perf
