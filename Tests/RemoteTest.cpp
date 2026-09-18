/*  Tests for the phone-control server: address choice, the code, and the gate.

    The address test is the reason this file exists. A machine with Docker, LXD
    or libvirt has a dozen IPv4 addresses and a phone can reach almost none of
    them, so "which address do we print" is real logic that can regress
    silently -- the feature still looks like it works until someone tries a
    phone at a gig.
*/
#include <juce_core/juce_core.h>
#include "RemoteServer.h"
#include "Hotspot.h"

using namespace juce;

static int failures = 0;
static void check (bool ok, const String& what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << std::endl;
    if (! ok) ++failures;
}

int main()
{
    // The chosen address must be one this machine actually has, and never a
    // container bridge or a link-local address.
    const auto host = perf::RemoteServer::getHostAddress();
    std::cout << "chosen address: " << host << std::endl;

    check (host.isNotEmpty(), "an address is chosen");
    check (! host.startsWith ("169.254."), "never link-local");

    StringArray mine;
    for (auto& ip : IPAddress::getAllAddresses()) mine.add (ip.toString());
    check (mine.contains (host), "the address belongs to this machine");

    // Docker's default bridge is the classic wrong answer: it is usually first
    // in the kernel's list on a developer machine.
    const auto hasDocker = mine.contains ("172.17.0.1");
    if (hasDocker)
        check (host != "172.17.0.1", "docker0 is not offered to the phone");
    else
        std::cout << "skip docker0 check (no docker bridge on this machine)" << std::endl;

    // ---- Hotspot -------------------------------------------------------------
    // Serving our own network is how phone control survives a venue with no
    // usable wifi, so the adapter list and the remembered choice are worth
    // guarding. These assert shape, not specific hardware: CI has no wifi.
    std::cout << "\nnetwork manager present: " << (perf::Hotspot::available() ? "yes" : "no") << std::endl;

    const auto adapters = perf::Hotspot::adapters();
    std::cout << "wifi adapters: " << adapters.size() << std::endl;
    for (auto& a : adapters)
        std::cout << "  " << a.interfaceName << "  ap=" << (a.supportsAccessPoint ? "yes" : "no")
                  << "  inUse=" << (a.inUseAsClient ? "yes" : "no")
                  << "  " << a.description << std::endl;

    // No virtual interface may ever be offered as a wifi adapter: a bridge shows
    // up in some nmcli output and would be a nonsense choice.
    bool anyVirtual = false, anyEmpty = false;
    for (auto& a : adapters)
    {
        if (a.interfaceName.startsWith ("br-") || a.interfaceName.startsWith ("docker")
            || a.interfaceName.startsWith ("virbr") || a.interfaceName.startsWith ("p2p-"))
            anyVirtual = true;
        if (a.interfaceName.isEmpty() || a.description.isEmpty()) anyEmpty = true;
    }
    check (! anyVirtual, "no virtual interface is offered as a wifi adapter");
    check (! anyEmpty, "every adapter has a name and a description");

    // Access-point-capable adapters sort ahead of ones that cannot, and a free
    // adapter ahead of one already carrying our connection: the first entry is
    // what a single-choice machine silently adopts, so the order is load-bearing.
    bool ordered = true;
    for (int i = 1; i < adapters.size(); ++i)
        if (! adapters[i - 1].supportsAccessPoint && adapters[i].supportsAccessPoint)
            ordered = false;
    check (ordered, "access-point-capable adapters are offered first");

    // A suggested password must be valid for WPA, or the hotspot cannot start.
    for (int i = 0; i < 20; ++i)
    {
        const auto pw = perf::Hotspot::suggestPassword();
        if (pw.length() < 8) { check (false, "suggested password is long enough for WPA"); break; }
        if (i == 19) check (true, "suggested password is long enough for WPA");
    }

    // The choice must survive a restart, which is the entire point of asking once.
    {
        auto tmp = File::getSpecialLocation (File::tempDirectory).getChildFile ("performer-hotspot-test.settings");
        tmp.deleteFile();
        PropertiesFile::Options o;
        o.applicationName = "performer-hotspot-test";
        {
            PropertiesFile pf (tmp, o);
            perf::Hotspot::Config c;
            c.interfaceName = "wlan-test0";
            c.networkName = "MyStage";
            c.password = "chorus42bridge";
            perf::Hotspot::save (pf, c);
        }
        PropertiesFile pf (tmp, o);
        const auto back = perf::Hotspot::load (pf);
        check (back.interfaceName == "wlan-test0", "the chosen adapter is remembered");
        check (back.networkName == "MyStage", "the network name is remembered");
        check (back.password == "chorus42bridge", "the password is remembered");
        tmp.deleteFile();
    }

    // With nothing saved we still get a usable default, so the prompt can be
    // pre-filled rather than blank.
    {
        auto tmp = File::getSpecialLocation (File::tempDirectory).getChildFile ("performer-hotspot-empty.settings");
        tmp.deleteFile();
        PropertiesFile::Options o;
        o.applicationName = "performer-hotspot-empty";
        PropertiesFile pf (tmp, o);
        const auto fresh = perf::Hotspot::load (pf);
        check (fresh.networkName.isNotEmpty(), "a default network name is offered");
        check (fresh.password.length() >= 8, "a valid default password is offered");
        check (fresh.interfaceName.isEmpty(), "no adapter is assumed before asking");
        tmp.deleteFile();
    }

    std::cout << (failures == 0 ? "\nall remote tests passed\n" : "\nremote tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
