#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>
#include "MappingSuggestions.h"
#include <functional>

namespace perf
{

/** Owns the plugin format manager and the list of known/scanned plugins. */
class PluginHost
{
public:
    explicit PluginHost (juce::PropertiesFile& settings);
    ~PluginHost();

    juce::AudioPluginFormatManager& getFormatManager()    { return formatManager; }
    juce::KnownPluginList& getKnownPlugins()              { return knownPlugins; }
    juce::File getDeadMansPedalFile() const               { return deadMansPedal; }
    juce::PropertiesFile& getSettings()                   { return settings; }
    MappingTemplates& getMappingTemplates()               { return *templates; }

    /** Creates a plugin instance. Must be called on the message thread. */
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription&,
                                                               double sampleRate, int blockSize,
                                                               juce::String& errorMessage);

    void saveKnownPlugins();

    /** Runs `exe args...`, collecting stdout, with a hard timeout and a cancellation
        check. Never blocks inside a read, so the calling thread can always be stopped
        cleanly. Returns false (and kills the child) on timeout or cancellation. */
    static bool runHelperWithTimeout (const juce::File& exe, const juce::StringArray& args, int timeoutMs,
                                      const std::function<bool()>& shouldStop, juce::String& output);

    /** Scans a plugin file in a performer-plugin-host process so a crashing plugin
        can't take Performer down. Falls back to in-process if the helper is missing. */
    struct OutOfProcessScanner : public juce::KnownPluginList::CustomScanner
    {
        bool findPluginTypesFor (juce::AudioPluginFormat&, juce::OwnedArray<juce::PluginDescription>&, const juce::String& fileOrIdentifier) override;
        void scanFinished() override {}
    };

private:
    juce::PropertiesFile& settings;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::File deadMansPedal;

    struct ListListener;
    std::unique_ptr<ListListener> listListener;
    std::unique_ptr<MappingTemplates> templates;
};

} // namespace perf
