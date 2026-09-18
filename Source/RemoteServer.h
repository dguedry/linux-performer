#pragma once

#include "Engine.h"
#include "Favourites.h"
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
    /** The address to open on the phone, or empty when stopped. The code is not
        in the URL: the page asks for it, so a shortcut survives a restart. */
    juce::String getUrl() const;

    /** The address a phone should use to reach this machine, chosen over
        container and virtual-machine bridges. Public so tests can check it. */
    static juce::String getHostAddress();

    /** True when a parameter name is a placeholder rather than a real control
        ("#000", "<unassigned>", "MIDI CC 0|3"). Plugins spell these differently
        and none flag them, so this is a heuristic; public so it can be tested. */
    static bool isPlaceholderName (const juce::String& name);
    juce::String getToken() const          { return token; }

    /** The per-plugin chosen parameters, shared with the desktop so both views
        agree about what is worth a slider. */
    Favourites& getFavourites()            { return favourites; }

private:
    void run() override;
    void handle (juce::StreamingSocket&);
    juce::String stateJson() const;
    juce::String slotsJson (int inputIndex) const;
    juce::String paramsJson (int inputIndex, int slot, int effect, const juce::String& search, bool allChannels) const;
    bool authorised (const juce::String& request) const;

    void programChanged (int, int) override   { ++revision; }
    void setupChanged() override              { ++revision; }
    void programContentChanged (int, int) override { ++revision; }

    Engine& engine;
    juce::PropertiesFile& settings;
    Favourites favourites;
    std::unique_ptr<juce::StreamingSocket> listener;
    juce::String token;
    int port = 0;
    std::atomic<bool> running { false };
    std::atomic<int> revision { 0 };     // bumped on any change, so the page can poll cheaply

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RemoteServer)
};

} // namespace perf
