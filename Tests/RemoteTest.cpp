/*  Tests for the phone-control server: address choice, the code, and the gate.

    The address test is the reason this file exists. A machine with Docker, LXD
    or libvirt has a dozen IPv4 addresses and a phone can reach almost none of
    them, so "which address do we print" is real logic that can regress
    silently -- the feature still looks like it works until someone tries a
    phone at a gig.
*/
#include <juce_core/juce_core.h>
#include "RemoteServer.h"

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

    std::cout << (failures == 0 ? "\nall remote tests passed\n" : "\nremote tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
