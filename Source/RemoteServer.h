#pragma once

#include "Engine.h"
#include <juce_core/juce_core.h>

namespace perf
{

/** A phone or tablet as a program selector.

    Serves a small web app on the local network: each input with its current
    program, the list of programs that have something in them, and a tap to
    switch. It is a progressive web app, so it can be added to a home screen and
    opens fullscreen with no installing.

    Security: this binds to the local network, where a venue's wifi is not
    friendly. Every request must carry a token that is generated per run and
    embedded in the URL you scan; without it the server answers 403. That stops
    someone on the same network changing your sounds by guessing the address.
    It is not protection against someone who can read your screen, which is the
    right level for a stage tool. */
class RemoteServer : private juce::Thread,
                     private Engine::Listener
{
public:
    RemoteServer (Engine&, juce::PropertiesFile&);
    ~RemoteServer() override;

    bool start (int port);
    void stop();
    bool isRunning() const                 { return running.load(); }
    int getPort() const                    { return port; }
    /** The address to open on the phone, token included, or empty when stopped. */
    juce::String getUrl() const;
    juce::String getToken() const          { return token; }

private:
    void run() override;
    void handle (juce::StreamingSocket&);
    juce::String stateJson() const;
    bool authorised (const juce::String& request) const;

    void programChanged (int, int) override   { ++revision; }
    void setupChanged() override              { ++revision; }
    void programContentChanged (int, int) override { ++revision; }

    Engine& engine;
    juce::PropertiesFile& settings;
    std::unique_ptr<juce::StreamingSocket> listener;
    juce::String token;
    int port = 0;
    std::atomic<bool> running { false };
    std::atomic<int> revision { 0 };     // bumped on any change, so the page can poll cheaply

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RemoteServer)
};

} // namespace perf
