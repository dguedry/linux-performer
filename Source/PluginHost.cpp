#include "PluginHost.h"

using namespace juce;

namespace perf
{

struct PluginHost::ListListener : public ChangeListener
{
    explicit ListListener (PluginHost& h) : host (h) {}
    void changeListenerCallback (ChangeBroadcaster*) override { host.saveKnownPlugins(); }
    PluginHost& host;
};

PluginHost::PluginHost (PropertiesFile& s) : settings (s)
{
    addDefaultFormatsToManager (formatManager);

    deadMansPedal = settings.getFile().getSiblingFile ("RecentlyCrashedPluginsList");
    templates = std::make_unique<MappingTemplates> (settings.getFile().getSiblingFile ("mapping-templates.json"));

    if (auto xml = settings.getXmlValue ("pluginList"))
        knownPlugins.recreateFromXml (*xml);

    // Anything that crashed during a previous scan gets blacklisted.
    for (auto& line : StringArray::fromLines (deadMansPedal.loadFileAsString()))
        if (line.isNotEmpty())
            knownPlugins.addToBlacklist (line);

    listListener = std::make_unique<ListListener> (*this);
    knownPlugins.addChangeListener (listListener.get());
}

PluginHost::~PluginHost()
{
    knownPlugins.removeChangeListener (listListener.get());
}

void PluginHost::saveKnownPlugins()
{
    if (auto xml = knownPlugins.createXml())
        settings.setValue ("pluginList", xml.get());
    settings.saveIfNeeded();
}

std::unique_ptr<AudioPluginInstance> PluginHost::createInstance (const PluginDescription& desc,
                                                                 double sampleRate, int blockSize,
                                                                 String& errorMessage)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto instance = formatManager.createPluginInstance (desc, sampleRate, blockSize, errorMessage);
    if (instance == nullptr && errorMessage.isEmpty())
        errorMessage = "Unknown error creating plugin " + desc.name;
    return instance;
}

} // namespace perf
