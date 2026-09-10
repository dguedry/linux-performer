#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>

namespace perf
{

class PluginHost;

/**
    Scans plugin folders in the background with a small thread pool, one format at a
    time. Lives for the whole application (owned by PluginHost), so closing the Plugins
    window doesn't interrupt a scan; the window just shows progress while it's open.

    Probing happens in performer-plugin-host processes (see PluginHost::OutOfProcessScanner).
    Cancelling stops taking new files and lets in-flight probes finish; shutdown() kills
    them so the app can exit promptly.
*/
class PluginScanner : public juce::ChangeBroadcaster,
                      private juce::Timer
{
public:
    explicit PluginScanner (PluginHost&);
    ~PluginScanner() override;

    /** Returns false if a scan is already running. */
    bool startScan (juce::AudioPluginFormat&, const juce::FileSearchPath&);
    void cancel();
    void shutdown();

    bool isScanning() const                 { return scanning.load(); }
    double getProgress() const;
    juce::String getCurrentFile() const;
    juce::String getFormatName() const;
    /** Results of the last finished scan. */
    juce::StringArray getFailedFiles() const;
    juce::StringArray getNewlyBlacklistedFiles() const;
    /** Plugins dropped from the list because their files were gone (uninstalled, un-bridged). */
    juce::StringArray getRemovedPlugins() const;

    /** Set while the application is shutting down: probes are killed immediately. */
    static std::atomic<bool> hardStop;

private:
    struct Job;
    void timerCallback() override;
    void finish();

    PluginHost& host;
    std::unique_ptr<juce::PluginDirectoryScanner> scanner;
    std::unique_ptr<juce::ThreadPool> pool;
    std::atomic<bool> scanning { false }, stopRequested { false };
    std::atomic<int> activeJobs { 0 };
    mutable juce::CriticalSection stateLock;
    juce::String currentFile, formatName;
    juce::StringArray initiallyBlacklisted, newlyBlacklisted, failedFiles, removedPlugins;
    juce::AudioPluginFormat* currentFormat = nullptr;
    static constexpr int numThreads = 4;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginScanner)
};

} // namespace perf
