#include "PluginView.h"
#include <juce_events/juce_events.h>

using namespace juce;

namespace perf
{

namespace
{
    struct Running
    {
        PluginView::Session session;
        std::unique_ptr<ChildProcess> vnc, web;
    };

    CriticalSection lock;
    OwnedArray<Running> sessions;

    /* Ports above the tablet's own. One pair per window, so two plugin windows
       can be open at once without fighting over a port. */
    constexpr int kFirstVncPort = 5910;
    constexpr int kFirstWebPort = 6910;
    constexpr int kMaxSessions  = 10;      // ports 5910-5919 and 6910-6919 are ours

    File findTool (const String& name)
    {
        for (auto* dir : { "/usr/bin", "/usr/local/bin", "/bin" })
            if (auto f = File (dir).getChildFile (name); f.existsAsFile())
                return f;
        return {};
    }

    /** The noVNC web client, wherever the distribution put it. */
    File findNoVncRoot()
    {
        for (auto* dir : { "/usr/share/novnc", "/usr/share/webapps/novnc", "/usr/local/share/novnc" })
            if (auto d = File (dir); d.isDirectory() && d.getChildFile ("vnc.html").existsAsFile())
                return d;
        return {};
    }
}

//==============================================================================
bool PluginView::available (String& whatIsMissing)
{
    StringArray missing;
    if (findTool ("x11vnc") == File())     missing.add ("x11vnc");
    if (findTool ("websockify") == File()) missing.add ("websockify");
    if (findNoVncRoot() == File())         missing.add ("novnc");

    if (missing.isEmpty()) return true;

    whatIsMissing = "Showing a plugin's window on the tablet needs "
                  + missing.joinIntoString (", ") + ". Install "
                  + (missing.size() == 1 ? "it" : "them") + " and try again.";
    return false;
}

unsigned long PluginView::findWindow (const String& title)
{
    /* Ask the window manager rather than the plugin: the editor belongs to a
       helper process, and its X window is not ours to look up directly. The
       title is one Performer chose and is unique per slot. */
    auto xdotool = findTool ("xdotool");
    if (xdotool == File()) return 0;

    /* xdotool reads --name as a regular expression. A program called
       "Strings (Pad)" would then be looking for "Strings Pad" and never find
       its own window, so every character that means something to a regex is
       escaped first. */
    String pattern;
    for (auto c : title)
    {
        if (c < 128 && String ("\\^$.|?*+()[]{}").containsChar (c))
            pattern << '\\';
        pattern << c;
    }

    ChildProcess p;
    if (! p.start (StringArray { xdotool.getFullPathName(), "search", "--name", pattern }))
        return 0;

    const auto out = p.readAllProcessOutput();
    p.waitForProcessToFinish (4000);

    for (const auto& line : StringArray::fromLines (out))
        if (const auto id = line.trim().getLargeIntValue(); id > 0)
            return (unsigned long) id;
    return 0;
}

//==============================================================================
/* Brings the editor to the front. x11vnc injects the tablet's clicks with
   XTest at screen coordinates, so they land on whatever window is on top at
   that spot -- if the browser, or Performer's own window, overlaps the editor,
   the pointer visibly moves to the right place and the click goes to the wrong
   window. Measured: a covered button never fires; uncover it and the same
   click does. Raising the editor whenever the tablet asks for it is the most
   that can be done from here; the manual says the rest. */
static void raiseWindow (unsigned long windowId)
{
    auto xdotool = findTool ("xdotool");
    if (xdotool == File() || windowId == 0) return;

    for (const char* verb : { "windowactivate", "windowraise" })
    {
        ChildProcess p;
        if (p.start (StringArray { xdotool.getFullPathName(), verb, String ((int64) windowId) }))
            p.waitForProcessToFinish (1500);
    }
}

//==============================================================================
PluginView::Session PluginView::start (const String& title, String& error)
{
    const ScopedLock sl (lock);

    for (auto* r : sessions)
        if (r->session.title == title)
        {
            /* Already serving, unless the window has gone since -- or either
               helper has. A half-dead session is worse than none: the port
               answers, so the tab opens and then hangs. */
            if (findWindow (title) == r->session.windowId
                && r->vnc != nullptr && r->vnc->isRunning()
                && r->web != nullptr && r->web->isRunning())
            {
                raiseWindow (r->session.windowId);
                return r->session;
            }

            if (r->web != nullptr) r->web->kill();
            if (r->vnc != nullptr) r->vnc->kill();
            sessions.removeObject (r);
            break;
        }

    if (String missing; ! available (missing)) { error = missing; return {}; }

    const auto window = findWindow (title);
    if (window == 0)
    {
        error = "That plugin's window is not open. Open it with \"Edit GUI\" first.";
        return {};
    }
    raiseWindow (window);

    /* A free pair of ports. Probed, not assumed: a stray from a crash, or
       anything else on the machine, may be sitting on one, and starting x11vnc
       on a busy port fails silently from here -- the session looks fine and
       the tablet connects to nothing. */
    auto portFree = [] (int port)
    {
        StreamingSocket probe;
        const bool ok = probe.createListener (port, "127.0.0.1");
        probe.close();
        return ok;
    };
    auto inUse = [] (int vnc, int web)
    {
        for (auto* r : sessions)
            if (r->session.vncPort == vnc || r->session.webPort == web) return true;
        return false;
    };

    int vncPort = 0, webPort = 0;
    for (int i = 0; i < kMaxSessions; ++i)
    {
        const int v = kFirstVncPort + i, w = kFirstWebPort + i;
        if (inUse (v, w) || ! portFree (v) || ! portFree (w)) continue;
        vncPort = v; webPort = w;
        break;
    }
    if (vncPort == 0)
    {
        error = "No free port for the plugin window.";
        return {};
    }

    auto running = std::make_unique<Running>();
    running->session = { title, window, vncPort, webPort };

    /* -localhost: the stream itself never leaves this machine. The tablet talks
       to websockify, which is what we expose, so there is one door rather than
       two. -nopw is safe for the same reason. */
    running->vnc = std::make_unique<ChildProcess>();
    if (! running->vnc->start (StringArray {
            findTool ("x11vnc").getFullPathName(),
            "-id", String ((int64) window),
            "-localhost", "-rfbport", String (vncPort),
            "-nopw", "-forever", "-shared", "-quiet", "-noxdamage" }))
    {
        error = "Could not start x11vnc.";
        return {};
    }

    Thread::sleep (400);            // let it bind before websockify dials in

    running->web = std::make_unique<ChildProcess>();
    if (! running->web->start (StringArray {
            findTool ("websockify").getFullPathName(),
            "--web=" + findNoVncRoot().getFullPathName(),
            // Loopback only: the tablet reaches this through Performer's own
            // port, so the bridge itself never needs to be on the network.
            "127.0.0.1:" + String (webPort),
            "127.0.0.1:" + String (vncPort) }))
    {
        running->vnc->kill();
        error = "Could not start websockify.";
        return {};
    }

    const auto session = running->session;
    sessions.add (running.release());
    return session;
}

void PluginView::stop (const String& title)
{
    const ScopedLock sl (lock);
    for (auto* r : sessions)
        if (r->session.title == title)
        {
            if (r->web != nullptr) r->web->kill();
            if (r->vnc != nullptr) r->vnc->kill();
            sessions.removeObject (r);
            return;
        }
}

void PluginView::stopAll()
{
    {
        const ScopedLock sl (lock);
        for (auto* r : sessions)
        {
            if (r->web != nullptr) r->web->kill();
            if (r->vnc != nullptr) r->vnc->kill();
        }
        sessions.clear();
    }
    sweepStrays();
}

void PluginView::sweepStrays()
{
    /* Anything of ours still holding a port after its ChildProcess has gone --
       after a crash, or a websockify that outlived Performer, which once kept
       port 7777 bound so the next run could not start its tablet server at
       all. The patterns are the exact argument shapes this file launches, on
       our port range only, so nothing else on the machine can match: a VNC
       server someone runs for their own desktop is on other ports and has
       other arguments. */
    auto pkill = findTool ("pkill");
    if (pkill == File()) return;

    /* Anchored at both ends to the exact command lines start() builds. pkill
       -f matches anywhere in a process's whole command line, so an unanchored
       pattern also matches a terminal, an editor or a script that merely
       mentions those words -- which is how a test shell got killed while this
       was being checked. With ^ and $ only a process that IS one of ours fits.
       websockify is a Python script, so its argv[0] is the interpreter. */
    const String vncRange = "59[1][0-9]";        // 5910-5919
    const String webRange = "69[1][0-9]";        // 6910-6919
    for (const String pattern : {
            "^\\S*x11vnc -id [0-9]+ -localhost -rfbport " + vncRange
                + " -nopw -forever -shared -quiet -noxdamage$",
            "^(\\S+ )?\\S*websockify --web=\\S+ 127\\.0\\.0\\.1:" + webRange
                + " 127\\.0\\.0\\.1:" + vncRange + "$" })
    {
        ChildProcess p;
        if (p.start (StringArray { pkill.getFullPathName(), "-f", pattern }))
            p.waitForProcessToFinish (2000);
    }
}

void PluginView::dropDeadSessions()
{
    const ScopedLock sl (lock);
    for (int i = sessions.size(); --i >= 0;)
    {
        auto* r = sessions.getUnchecked (i);
        if (findWindow (r->session.title) == r->session.windowId
            && r->vnc != nullptr && r->vnc->isRunning()
            && r->web != nullptr && r->web->isRunning())
            continue;

        if (r->web != nullptr) r->web->kill();
        if (r->vnc != nullptr) r->vnc->kill();
        sessions.remove (i);
    }
}

Array<PluginView::Session> PluginView::active()
{
    const ScopedLock sl (lock);
    Array<Session> out;
    for (auto* r : sessions) out.add (r->session);
    return out;
}

} // namespace perf
