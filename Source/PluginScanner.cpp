#include "PluginScanner.h"
#include "PluginHost.h"

using namespace juce;

namespace perf
{

std::atomic<bool> PluginScanner::hardStop { false };

struct PluginScanner::Job : public ThreadPoolJob
{
    explicit Job (PluginScanner& s) : ThreadPoolJob ("plugin scan"), owner (s) {}

    JobStatus runJob() override
    {
        String name;
        while (! owner.stopRequested.load() && ! shouldExit())
        {
            {
                const ScopedLock sl (owner.stateLock);
                owner.currentFile = name;
            }
            if (! owner.scanner->scanNextFile (true, name))
                break;
        }
        --owner.activeJobs;
        return jobHasFinished;
    }

    PluginScanner& owner;
};

PluginScanner::PluginScanner (PluginHost& h) : host (h) { hardStop.store (false); }

PluginScanner::~PluginScanner()
{
    shutdown();
}

bool PluginScanner::startScan (AudioPluginFormat& format, const FileSearchPath& paths)
{
    if (scanning.load()) return false;

    stopRequested.store (false);
    {
        const ScopedLock sl (stateLock);
        formatName = format.getName();
        currentFile.clear();
        failedFiles.clear();
        newlyBlacklisted.clear();
        initiallyBlacklisted = host.getKnownPlugins().getBlacklistedFiles();
    }

    scanner = std::make_unique<PluginDirectoryScanner> (host.getKnownPlugins(), format, paths, true,
                                                        host.getDeadMansPedalFile(), false);
    pool = std::make_unique<ThreadPool> (ThreadPoolOptions{}.withNumberOfThreads (numThreads));
    activeJobs.store (numThreads);
    scanning.store (true);
    for (int i = 0; i < numThreads; ++i)
        pool->addJob (new Job (*this), true);

    startTimer (50);
    sendChangeMessage();
    return true;
}

void PluginScanner::cancel()
{
    stopRequested.store (true);
}

void PluginScanner::shutdown()
{
    stopTimer();
    stopRequested.store (true);
    hardStop.store (true);
    if (pool != nullptr)
    {
        pool->removeAllJobs (true, 10000);
        pool.reset();
    }
    scanner.reset();
    scanning.store (false);
}

double PluginScanner::getProgress() const
{
    return scanner != nullptr ? (double) scanner->getProgress() : 0.0;
}

String PluginScanner::getCurrentFile() const     { const ScopedLock sl (stateLock); return currentFile; }
String PluginScanner::getFormatName() const      { const ScopedLock sl (stateLock); return formatName; }
StringArray PluginScanner::getFailedFiles() const          { const ScopedLock sl (stateLock); return failedFiles; }
StringArray PluginScanner::getNewlyBlacklistedFiles() const { const ScopedLock sl (stateLock); return newlyBlacklisted; }

void PluginScanner::timerCallback()
{
    if (activeJobs.load() == 0)
        finish();
    else
        sendChangeMessage();
}

void PluginScanner::finish()
{
    stopTimer();
    if (pool != nullptr)
    {
        pool->removeAllJobs (false, 2000);   // all jobs have already returned
        pool.reset();
    }

    {
        const ScopedLock sl (stateLock);
        failedFiles = scanner != nullptr ? scanner->getFailedFiles() : StringArray();
        newlyBlacklisted.clear();
        for (auto& f : host.getKnownPlugins().getBlacklistedFiles())
            if (! initiallyBlacklisted.contains (f))
                newlyBlacklisted.add (f);
        currentFile.clear();
    }
    scanner.reset();
    scanning.store (false);
    host.saveKnownPlugins();
    sendChangeMessage();
}

} // namespace perf
