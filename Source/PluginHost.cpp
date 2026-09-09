#include "PluginHost.h"
#include "RemotePlugin.h"
#include <cstdio>
#include <functional>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

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

// Unlike juce::ChildProcess this never blocks inside a read: the scanner thread pool
// kills threads that don't stop, and a thread killed inside fread() aborts the whole
// application.
PluginHost::HelperResult PluginHost::runHelperWithTimeout (const File& exe, const StringArray& args, int timeoutMs,
                                                           int graceAfterStopMs, const std::function<bool()>& shouldStop, String& output)
{
    {
        int fds[2];
        if (::pipe (fds) != 0) return HelperResult::timedOut;

        posix_spawn_file_actions_t actions;
        ::posix_spawn_file_actions_init (&actions);
        ::posix_spawn_file_actions_adddup2 (&actions, fds[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_addclose (&actions, fds[0]);
        ::posix_spawn_file_actions_addclose (&actions, fds[1]);

        std::vector<std::string> storage { exe.getFullPathName().toStdString() };
        for (auto& a : args) storage.push_back (a.toStdString());
        std::vector<char*> argv;
        for (auto& a : storage) argv.push_back (const_cast<char*> (a.c_str()));
        argv.push_back (nullptr);

        pid_t pid = -1;
        const int rc = ::posix_spawn (&pid, storage[0].c_str(), &actions, nullptr, argv.data(), environ);
        ::posix_spawn_file_actions_destroy (&actions);
        ::close (fds[1]);
        if (rc != 0) { ::close (fds[0]); return HelperResult::timedOut; }

        MemoryOutputStream collected;
        const auto start = Time::getMillisecondCounterHiRes();
        double stopRequestedAt = -1.0;
        auto result = HelperResult::finished;

        for (;;)
        {
            pollfd pfd { fds[0], POLLIN, 0 };
            const int pr = ::poll (&pfd, 1, 100);
            if (pr > 0)
            {
                char buf[8192];
                const auto n = ::read (fds[0], buf, sizeof (buf));
                if (n > 0) { collected.write (buf, (size_t) n); continue; }
                if (n == 0 || (errno != EINTR && errno != EAGAIN)) break;   // EOF: child closed stdout
            }

            const auto now = Time::getMillisecondCounterHiRes();
            if (stopRequestedAt < 0.0 && shouldStop())
                stopRequestedAt = now;

            if (now - start > timeoutMs)                                      { result = HelperResult::timedOut; break; }
            if (stopRequestedAt >= 0.0 && now - stopRequestedAt > graceAfterStopMs) { result = HelperResult::stopped; break; }
        }
        ::close (fds[0]);

        if (result != HelperResult::finished)
            ::kill (pid, SIGKILL);
        int status = 0;
        ::waitpid (pid, &status, 0);

        output = collected.toString();
        return result;
    }
}

bool PluginHost::OutOfProcessScanner::findPluginTypesFor (AudioPluginFormat& format, OwnedArray<PluginDescription>& result, const String& fileOrIdentifier)
{
    const auto exe = RemotePlugin::findHostExecutable();
    if (exe == File())
    {
        format.findAllTypesForFile (result, fileOrIdentifier);
        return true;
    }

    // JUCE's scan dialog asks every still-running probe to exit the moment one thread
    // runs out of files, then waits up to a minute for them. Give slow (Wine-bridged)
    // plugins a real chance to finish in that window instead of treating it as failure.
    String output;
    const auto res = runHelperWithTimeout (exe, StringArray { "--scan", format.getName(), fileOrIdentifier }, 120000, 45000,
                                           [this] { return shouldExit(); }, output);

    int parsed = 0;
    for (auto& line : StringArray::fromLines (output))
        if (line.contains ("<PLUGIN"))
            if (auto xml = parseXML (line))
            {
                auto d = std::make_unique<PluginDescription>();
                if (d->loadFromXml (*xml)) { result.add (d.release()); ++parsed; }
            }

    if (SystemStats::getEnvironmentVariable ("PERFORMER_SCAN_DEBUG", {}).isNotEmpty())
        std::fprintf (stderr, "[scan] %s %s: %s, %d bytes of output, %d descriptions\n",
                      format.getName().toRawUTF8(), fileOrIdentifier.toRawUTF8(),
                      res == HelperResult::finished ? "ok" : res == HelperResult::timedOut ? "TIMED OUT" : "stopped before finishing",
                      (int) output.length(), parsed);

    // Only a plugin that hung for the whole timeout on its own is reported as failed
    // (and blacklisted). A probe that was asked to stop is simply not a result: the
    // plugin will be scanned again next time.
    return res != HelperResult::timedOut;
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
