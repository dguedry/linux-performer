#pragma once
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace perf
{

/** Serving our own wifi network so phones can reach us.

    A venue's wifi is often absent, locked down, or on a guest network that
    blocks devices from seeing each other -- all of which break phone control.
    Making the laptop its own access point removes that dependency entirely.

    NetworkManager does the real work: "shared" mode runs a DHCP server and NAT
    on the interface, so a phone that joins gets an address automatically and
    still has internet through whatever the laptop is connected to. There is
    nothing to configure on the phone beyond the wifi password.

    The one machine-specific question is which wifi adapter to use, because a
    single radio usually cannot be a client and an access point at the same
    time: starting a hotspot on the only adapter drops the laptop off its
    network. So we ask once and remember it. */
class Hotspot
{
public:
    struct Adapter
    {
        juce::String interfaceName;     // wlp5s0
        juce::String description;       // "RTL8812AE 802.11ac PCIe Wireless Network Adapter"
        bool supportsAccessPoint = false;
        bool inUseAsClient = false;     // currently our connection to the world
    };

    /** Wifi adapters that could serve a hotspot, best candidate first: an
        adapter that is not currently carrying our own connection is preferred,
        since using it costs us nothing. Empty when there is no wifi at all. */
    static juce::Array<Adapter> adapters();

    /** True when NetworkManager is present and usable. Everything else here
        returns nothing useful if this is false. */
    static bool available();

    struct State
    {
        bool active = false;
        juce::String interfaceName;
        juce::String networkName;
        juce::String address;           // 10.42.0.1 while active
    };
    static State state();

    struct Config
    {
        juce::String interfaceName;
        juce::String networkName = "PerformerStage";
        juce::String password;          // at least 8 characters, per WPA
    };

    /** Brings the hotspot up, creating or updating the saved profile. Returns an
        empty string on success, otherwise a message fit to show a user. */
    static juce::String start (const Config&);
    static juce::String stop();

    /** Remembered across runs: which adapter to use, and the network name and
        password, so the phone reconnects on its own at the next gig. */
    static Config load (juce::PropertiesFile&);
    static void save (juce::PropertiesFile&, const Config&);

    /** A password we can offer as a default. Readable at arm's length on a
        stage, and long enough for WPA. */
    static juce::String suggestPassword();
};

} // namespace perf
