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
#include "QrCode.h"
#include "BinaryData.h"
#include "Favourites.h"
#include "Engine.h"
#include "PluginHost.h"

using namespace juce;

static int failures = 0;
static void check (bool ok, const String& what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << std::endl;
    if (! ok) ++failures;
}

int main()
{
    /* The engine delivers events through an async update, which needs a message
       manager. Without this the parameter-change path silently does nothing --
       which is exactly what it looked like when this test first failed. */
    ScopedJuceInitialiser_GUI juceInit;

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

    // ---- QR code -------------------------------------------------------------
    /* A hand-written encoder is worthless if a phone cannot read what it makes,
       and "it looks like a QR code" is not evidence. These check the structural
       invariants a scanner depends on; the module output was also verified
       against an independent decoder, which read back every address shape and
       port we can produce. */
    {
        const auto qr = perf::QrCode::encode ("http://10.42.0.1:7777/");
        check (qr.isValid(), "a typical address encodes");
        check (qr.getSize() == 25, "a typical address fits version 2 (25 modules)");

        // Finder patterns: three corners, dark centre 3x3, light ring around it.
        auto finderOk = [&qr] (int cx, int cy)
        {
            for (int dy = -4; dy <= 4; ++dy)
                for (int dx = -4; dx <= 4; ++dx)
                {
                    const int x = cx + dx, y = cy + dy;
                    if (x < 0 || y < 0 || x >= qr.getSize() || y >= qr.getSize()) continue;
                    const int d = std::max (std::abs (dx), std::abs (dy));
                    if (d <= 3 && qr.getModule (x, y) != (d != 2)) return false;
                }
            return true;
        };
        check (finderOk (3, 3), "top-left finder pattern is correct");
        check (finderOk (qr.getSize() - 4, 3), "top-right finder pattern is correct");
        check (finderOk (3, qr.getSize() - 4), "bottom-left finder pattern is correct");

        // The timing patterns must alternate without a break. A gap here costs a
        // scanner the grid it uses to locate every other module, and it was a
        // real bug: reserving the format area overwrote two timing modules.
        bool timingOk = true;
        for (int i = 8; i < qr.getSize() - 8; ++i)
        {
            if (qr.getModule (6, i) != (i % 2 == 0)) timingOk = false;
            if (qr.getModule (i, 6) != (i % 2 == 0)) timingOk = false;
        }
        check (timingOk, "timing patterns alternate with no break");

        // Longer and shorter addresses must both work, and the version has to
        // grow rather than silently truncate.
        const auto small = perf::QrCode::encode ("http://10.0.0.2:80/");
        const auto big   = perf::QrCode::encode ("http://192.168.100.254:65535/");
        check (small.isValid() && big.isValid(), "short and long addresses both encode");
        check (big.getSize() >= small.getSize(), "a longer address uses at least as many modules");

        // Something far too long must be refused, not mangled: the caller shows
        // the address as text instead.
        String tooLong;
        for (int i = 0; i < 200; ++i) tooLong << "x";
        check (! perf::QrCode::encode (tooLong).isValid(), "over-long text is refused rather than truncated");

        // The printed program map embeds the code as SVG.
        const auto svg = qr.toSvg (28);
        check (svg.startsWith ("<svg") && svg.endsWith ("</svg>"), "SVG output is a complete element");
        check (svg.contains ("28mm"), "SVG is sized for paper");
        check (! perf::QrCode::encode ("").isValid() || true, "empty text does not crash");
    }

    // ---- a program picked from the phone reaches the setup ---------------------
    /* The phone is often the only thing in reach mid-set, so a choice made there
       has to be as real as one made on the laptop: it must change the engine AND
       survive being written out and read back. */
    {
        auto dir = File::getSpecialLocation (File::tempDirectory).getChildFile ("performer-remote-persist");
        dir.deleteRecursively();
        dir.createDirectory();

        PropertiesFile::Options po;
        po.applicationName = "performer-remote-persist";
        PropertiesFile props (dir.getChildFile ("p.settings"), po);

        perf::PluginHost host (props);
        perf::Engine engine (host, props, false);
        engine.setProgramName (0, 1, "Second");

        perf::RemoteServer rs (engine, props);
        if (rs.start (0) || rs.start (7791))
        {
            const auto before = engine.getSetup().inputs[0].currentProgram;
            engine.selectProgram (0, 1);
            const auto after = engine.getSetup().inputs[0].currentProgram;
            check (before != after && after == 1, "selecting a program changes the live setup");

            const auto file = dir.getChildFile ("setup.performer.json");
            check (engine.captureSetup().saveToFile (file).wasOk(), "the setup writes to disk");

            perf::Setup reloaded;
            const auto r = perf::Setup::loadFromFile (file, reloaded);
            check (r.wasOk(), "the setup reads back");
            check (r.wasOk() && reloaded.inputs[0].currentProgram == 1,
                   "the chosen program survives a save and reload");
            rs.stop();
        }
        else
        {
            std::cout << "skip persistence check (could not bind a port)" << std::endl;
        }
        dir.deleteRecursively();
    }

    // ---- phone control is remembered across a restart --------------------------
    /* Someone who sets a phone on a stand expects it to still work after a
       restart. The toggle itself has to persist, not just the code: without this
       the app always came up with the server off, which looks like the phone has
       broken rather than like a setting was forgotten. */
    {
        auto tmp = File::getSpecialLocation (File::tempDirectory).getChildFile ("performer-remoteon.settings");
        tmp.deleteFile();
        PropertiesFile::Options o;
        o.applicationName = "performer-remoteon";

        {
            PropertiesFile pf (tmp, o);
            check (! pf.getBoolValue ("remoteOn", false), "phone control is off until it is turned on");
            pf.setValue ("remoteOn", true);
            pf.setValue ("remotePort", 7777);
            pf.saveIfNeeded();
        }
        {
            PropertiesFile pf (tmp, o);
            check (pf.getBoolValue ("remoteOn", false), "phone control being on survives a restart");
            check (pf.getIntValue ("remotePort", 0) == 7777, "the port survives a restart");
            pf.setValue ("remoteOn", false);
            pf.saveIfNeeded();
        }
        {
            PropertiesFile pf (tmp, o);
            check (! pf.getBoolValue ("remoteOn", true), "turning it off is remembered too");
        }
        tmp.deleteFile();
    }

    // ---- the manual is embedded and complete ---------------------------------
    /* The in-app help renders docs/MANUAL.md from the binary. If the embedding
       breaks, the Help button opens an empty window -- which is worse than no
       help at all, because it looks like the app is broken. */
    {
        const auto manual = String::createStringFromData (BinaryData::MANUAL_md, BinaryData::MANUAL_mdSize);
        check (manual.isNotEmpty(), "the manual is embedded in the binary");
        check (manual.length() > 10000, "the whole manual is embedded, not a fragment");
        check (manual.contains ("# Performer user manual"), "the manual starts with its title");

        // The sections someone needs when something is wrong at a venue.
        check (manual.contains ("If the phone cannot connect"), "the firewall section is present");
        check (manual.contains ("sudo ufw allow from"), "the firewall commands are present");
        check (manual.contains ("When the venue has no usable wifi"), "the hotspot section is present");
        check (manual.contains ("## When something goes wrong"), "the troubleshooting section is present");

        // UTF-8 must survive the round trip through BinaryData: the manual uses
        // typographic punctuation, and mojibake here would be very visible.
        check (! manual.contains ("â€"), "the manual is not mojibake");
    }

    // ---- favourites are remembered per plugin ---------------------------------
    /* Chosen controls are keyed by plugin, not by program, so picking the
       drawbars on a B-3X once makes them appear wherever it is loaded. They are
       stored by parameter ID rather than index: a plugin update can renumber
       its parameters, and a slider labelled "Leslie Speed" that silently moves
       reverb is worse than one that disappears. */
    {
        auto f = File::getSpecialLocation (File::tempDirectory).getChildFile ("performer-favourites-test.json");
        f.deleteFile();

        PluginDescription organ;  organ.name = "Hammond B-3X"; organ.pluginFormatName = "VST3"; organ.fileOrIdentifier = "/x/b3x.vst3";
        PluginDescription sampler; sampler.name = "Kontakt 8"; sampler.pluginFormatName = "VST3"; sampler.fileOrIdentifier = "/x/kontakt.vst3";

        {
            perf::Favourites fav (f);
            check (! fav.hasAny (organ), "a plugin starts with nothing chosen");

            fav.add (organ, "1662");
            fav.add (organ, "1664");
            fav.add (organ, "1662");                      // already there
            check (fav.get (organ).size() == 2, "the same parameter is not added twice");
            check (fav.contains (organ, "1662"), "a chosen parameter is remembered");
            check (! fav.contains (sampler, "1662"), "choices do not leak between plugins");

            fav.add (sampler, "7");
            check (fav.get (sampler).size() == 1, "a second plugin keeps its own list");
        }
        {
            perf::Favourites fav (f);                      // reload from disk
            check (fav.get (organ).size() == 2, "choices survive a restart");
            check (fav.get (organ)[0] == "1662", "the chosen order is kept");
            check (fav.get (sampler).size() == 1, "each plugin reloads its own list");

            fav.remove (organ, "1662");
            check (fav.get (organ).size() == 1, "a parameter can be removed");
            check (fav.contains (organ, "1664"), "removing one leaves the rest");

            fav.remove (organ, "1664");
            check (! fav.hasAny (organ), "removing the last one empties the plugin");
        }
        {
            perf::Favourites fav (f);
            check (! fav.hasAny (organ), "an emptied plugin stays empty across a restart");
            check (fav.hasAny (sampler), "emptying one plugin does not touch another");
        }
        f.deleteFile();

        // A missing file is the normal first-run state, not an error.
        auto missing = File::getSpecialLocation (File::tempDirectory).getChildFile ("performer-favourites-none.json");
        missing.deleteFile();
        perf::Favourites fresh (missing);
        check (! fresh.hasAny (organ), "a missing favourites file is not an error");
    }

    // ---- telling a real control from a placeholder ----------------------------
    /* Every plugin spells "this parameter is empty" differently and none flag
       it, so this is a heuristic -- and the cost of getting it wrong is
       asymmetric. A missed placeholder just adds a row to scroll past; a real
       control wrongly called a placeholder sorts below 2000 of them and is
       effectively lost. These pin the shapes seen on real plugins. */
    {
        auto ph = [] (const char* n) { return perf::RemoteServer::isPlaceholderName (n); };

        // Kontakt 8: 2049 empty automation slots.
        check (ph ("#000") && ph ("#2048"), "Kontakt's \"#000\" slots are placeholders");
        // Numa Player: 64 unassigned entries and 2048 controller proxies.
        check (ph ("<unassigned>"), "\"<unassigned>\" is a placeholder");
        check (ph ("MIDI CC 0|0") && ph ("MIDI CC 15|127"), "a controller-named proxy is a placeholder");
        // Common shapes from other hosts and plugins.
        check (ph ("") && ph ("   ") && ph ("-"), "empty and dash names are placeholders");
        check (ph ("Param 17") && ph ("param 3"), "\"Param 17\" is a placeholder");

        // Real controls must never be mistaken for placeholders.
        check (! ph ("Leslie Speed"), "a named control is kept");
        check (! ph ("Upper Drawbar 1"), "a name ending in a number is still a real control");
        check (! ph ("Channel Volume(MSB)"), "a controller with a real name is kept");
        check (! ph ("Bypass"), "Bypass is a real control");
        check (! ph ("Parameter Feedback"), "a name merely starting with \"param\" is kept");
        check (! ph ("Vibrato and Chorus"), "a multi-word name is kept");
        check (! ph ("#hashtag"), "a # name that is not all digits is kept");
    }

    // ---- switch or fader? ------------------------------------------------------
    /* Plugins are unreliable about saying which is which. Hammond B-3X reports
       every parameter as non-boolean and continuous, including "Volume Switch",
       but it does report step counts -- two for its switches, nine for its
       drawbars -- so the step count is the signal worth trusting. */
    {
        auto sw = [] (bool b, int steps, const char* n)
                  { return perf::RemoteServer::isSwitchLike (b, steps, n); };

        check (sw (true, 0, "Anything"), "a plugin that says boolean is believed");
        check (sw (false, 2, "Percussion Switch"), "two steps is a switch whatever the plugin claims");
        check (! sw (false, 9, "Upper Drawbar 1"), "a nine-position drawbar is not a switch");
        check (! sw (false, 0, "Leslie Speed"), "a continuous control is not a switch");
        check (! sw (false, 6, "Vibrato and Chorus"), "a six-position selector is not a switch");

        // Names, for plugins that report nothing useful at all.
        check (sw (false, 0, "Volume Switch"), "a name ending in Switch is a switch");
        check (sw (false, 0, "Bypass"), "Bypass is a switch");
        check (sw (false, 0, "Reverb Enable"), "a name ending in Enable is a switch");
        check (! sw (false, 0, "Switch Time"), "a name merely starting with Switch is not");
        check (! sw (false, 0, "Drawbar 3"), "an ordinary name is not a switch");
    }

    // ---- the page follows a plugin that changes its own values ------------------
    /* A B-3X program change moves every drawbar at once. Nothing the user did on
       the phone caused it, so nothing bumped the revision the page polls, and
       the faders kept showing the values from when they were drawn. The server
       counts parameter changes separately from structural ones, so the page can
       refresh fader positions without rebuilding and losing the group filter,
       an open picker, or a drag in progress. */
    {
        auto dir = File::getSpecialLocation (File::tempDirectory).getChildFile ("performer-paramrev");
        dir.deleteRecursively(); dir.createDirectory();
        PropertiesFile::Options po; po.applicationName = "performer-paramrev";
        PropertiesFile props (dir.getChildFile ("p.settings"), po);

        perf::PluginHost host (props);
        perf::Engine engine (host, props, false);
        perf::RemoteServer rs (engine, props);

        if (rs.start (7793))
        {
            auto paramsRev = [&]
            {
                const auto body = URL ("http://127.0.0.1:7793/api/state?t=" + rs.getToken())
                                    .readEntireTextStream (false);
                var parsed;
                if (JSON::parse (body, parsed).failed()) return -1;
                if (auto* o = parsed.getDynamicObject()) return (int) o->getProperty ("params");
                return -1;
            };

            const int before = paramsRev();
            check (before >= 0, "the state carries a parameter revision");


            /* What a plugin moving its own parameter looks like from outside: the
               engine tells every listener, and the server counts it. Going
               through the engine rather than poking the server directly is the
               point -- this is the wiring that was missing. */
            engine.notifyParameterTouchedForTesting (0, 0, 0, -1, 3);

            /* The engine queues this and delivers it on the message thread, so
               the loop has to run before the server has heard anything. */
            for (int i = 0; i < 40 && paramsRev() == before; ++i)
                MessageManager::getInstance()->runDispatchLoopUntil (25);

            const int after = paramsRev();
            check (after != before, "a plugin-side parameter change bumps that revision");
            rs.stop();
        }
        else std::cout << "skip parameter-revision check (could not bind)" << std::endl;
        dir.deleteRecursively();
    }

    std::cout << (failures == 0 ? "\nall remote tests passed\n" : "\nremote tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
