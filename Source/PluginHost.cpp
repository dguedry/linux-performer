#include "PluginHost.h"
#include "RemotePlugin.h"
#include <cstdio>

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
    knownPlugins.setCustomScanner (std::make_unique<OutOfProcessScanner>());
}

bool PluginHost::OutOfProcessScanner::findPluginTypesFor (AudioPluginFormat& format, OwnedArray<PluginDescription>& result, const String& fileOrIdentifier)
{
    const auto exe = RemotePlugin::findHostExecutable();
    if (exe == File())
    {
        format.findAllTypesForFile (result, fileOrIdentifier);
        return true;
    }

    ChildProcess proc;
    if (! proc.start (StringArray { exe.getFullPathName(), "--scan", format.getName(), fileOrIdentifier }, ChildProcess::wantStdOut))
        return false;

    // Drain stdout while it runs so a chatty plugin can't fill the pipe and stall.
    String output;
    const auto start = Time::getMillisecondCounterHiRes();
    while (proc.isRunning())
    {
        char buf[4096];
        const int n = proc.readProcessOutput (buf, sizeof (buf));
        if (n > 0) output += String::fromUTF8 (buf, n);
        else if (Time::getMillisecondCounterHiRes() - start > 120000.0) { proc.kill(); return false; }
        else Thread::sleep (10);
        if (shouldExit()) { proc.kill(); return false; }
    }
    output += proc.readAllProcessOutput();

    int parsed = 0;
    for (auto& line : StringArray::fromLines (output))
        if (line.contains ("<PLUGIN"))
            if (auto xml = parseXML (line))
            {
                auto d = std::make_unique<PluginDescription>();
                if (d->loadFromXml (*xml)) { result.add (d.release()); ++parsed; }
            }

    if (SystemStats::getEnvironmentVariable ("PERFORMER_SCAN_DEBUG", {}).isNotEmpty())
        std::fprintf (stderr, "[scan] %s %s: %d bytes of output, %d descriptions, exit %u\n",
                      format.getName().toRawUTF8(), fileOrIdentifier.toRawUTF8(), (int) output.length(), parsed, proc.getExitCode());
    return true;
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
