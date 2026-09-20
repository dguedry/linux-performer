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

    /* Ports above the phone's own. One pair per window, so two plugin windows
       can be open at once without fighting over a port. */
    constexpr int kFirstVncPort = 5910;
    constexpr int kFirstWebPort = 6910;

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

    whatIsMissing = "Showing a plugin's window on the phone needs "
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

    ChildProcess p;
    if (! p.start (StringArray { xdotool.getFullPathName(), "search", "--name", title }))
        return 0;

    const auto out = p.readAllProcessOutput();
    p.waitForProcessToFinish (4000);

    for (const auto& line : StringArray::fromLines (out))
        if (const auto id = line.trim().getLargeIntValue(); id > 0)
            return (unsigned long) id;
    return 0;
}

//==============================================================================
PluginView::Session PluginView::start (const String& title, String& error)
{
    const ScopedLock sl (lock);

    for (auto* r : sessions)
        if (r->session.title == title)
        {
            // Already serving, unless the window has gone since.
            if (findWindow (title) == r->session.windowId) return r->session;
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

    // A free pair of ports: one per running session.
    int vncPort = kFirstVncPort, webPort = kFirstWebPort;
    for (auto* r : sessions)
    {
        vncPort = jmax (vncPort, r->session.vncPort + 1);
        webPort = jmax (webPort, r->session.webPort + 1);
    }

    auto running = std::make_unique<Running>();
    running->session = { title, window, vncPort, webPort };

    /* -localhost: the stream itself never leaves this machine. The phone talks
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
            // Loopback only: the phone reaches this through Performer's own
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
    const ScopedLock sl (lock);
    for (auto* r : sessions)
    {
        if (r->web != nullptr) r->web->kill();
        if (r->vnc != nullptr) r->vnc->kill();
    }
    sessions.clear();

    /* Belt and braces: anything of ours still holding a port after its
       ChildProcess has gone. A websockify that outlived Performer once kept
       port 7777 bound, so the next run could not start its phone server at
       all -- and nothing about that failure pointed at the real cause. */
    if (auto pkill = findTool ("pkill"); pkill != File())
        for (int port = kFirstVncPort; port < kFirstVncPort + 10; ++port)
        {
            ChildProcess p;
            p.start (StringArray { pkill.getFullPathName(), "-f",
                                   "x11vnc .*-rfbport " + String (port) });
            p.waitForProcessToFinish (1500);
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
