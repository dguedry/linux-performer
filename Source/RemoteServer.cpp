#include "RemoteServer.h"
#include "BinaryData.h"
#include <juce_graphics/juce_graphics.h>

#if JUCE_LINUX || JUCE_MAC
 #include <ifaddrs.h>
 #include <net/if.h>
 #include <netinet/in.h>
 #include <arpa/inet.h>
#endif

using namespace juce;

namespace perf
{

//==============================================================================
// The page. Kept in one string so the app is a single binary with nothing to install.
// Big touch targets, current program first: this is read at arm's length on a stand.
static const char* kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#15161c">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<link rel="manifest" href="/manifest.webmanifest">
<title>Performer</title>
<style>
  :root { --bg:#15161c; --panel:#1e1f24; --row:#272931; --accent:#5aa9ff; --dim:#9aa0ab; }
  * { box-sizing:border-box; -webkit-tap-highlight-color:transparent; }
  body { margin:0; background:var(--bg); color:#fff; font:16px/1.4 system-ui,sans-serif;
         padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left); }
  header { display:flex; align-items:center; gap:12px; padding:12px 16px; background:var(--panel); position:sticky; top:0; z-index:2; }
  header h1 { font-size:15px; margin:0; flex:1; color:var(--dim); font-weight:600; letter-spacing:.06em; }
  #panic { background:#a3282d; color:#fff; border:0; border-radius:8px; padding:10px 16px; font-weight:700; font-size:14px; }
  .input { margin:14px 12px 22px; }
  .now { background:var(--panel); border-radius:12px; padding:14px 16px; margin-bottom:10px; }
  .now .name { font-size:12px; color:var(--dim); letter-spacing:.08em; font-weight:700; }
  .now .prog { display:flex; align-items:baseline; gap:12px; margin-top:4px; }
  .now .num { font-size:34px; font-weight:800; color:var(--accent); font-variant-numeric:tabular-nums; }
  .now .title { font-size:22px; font-weight:700; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .loading { color:#d8a657; font-size:14px; }
  .grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(150px,1fr)); gap:8px; }
  button.p { background:var(--row); color:#fff; border:0; border-radius:10px; padding:14px 12px; text-align:left;
             font-size:15px; display:flex; gap:10px; align-items:baseline; min-height:56px; }
  button.p .n { color:var(--dim); font-variant-numeric:tabular-nums; font-weight:700; font-size:13px; }
  button.p.on { background:var(--accent); color:#06121f; }
  button.p.on .n { color:#06121f; }
  #err { display:none; background:#a3282d; padding:10px 16px; font-size:14px; }
  #gate { display:none; align-items:center; justify-content:center; min-height:70vh; padding:20px; }
  #gate form { text-align:center; max-width:320px; width:100%; }
  #gate p { color:var(--dim); font-size:15px; margin:0 0 14px; }
  #gate .hint { font-size:13px; color:#d8a657; margin-top:14px; min-height:18px; }
  #code { width:100%; font-size:34px; text-align:center; letter-spacing:.25em; text-transform:uppercase;
          padding:14px; border-radius:12px; border:2px solid #3a3d47; background:var(--panel); color:#fff; }
  #code:focus { outline:none; border-color:var(--accent); }
  #gate button { margin-top:14px; width:100%; background:var(--accent); color:#06121f; border:0;
                 border-radius:12px; padding:16px; font-size:17px; font-weight:700; }
</style></head>
<body>
<header><h1>PERFORMER</h1><button id="panic">PANIC</button></header>
<div id="err"></div>
<div id="gate">
  <form id="codeform">
    <p>Enter the code shown in Performer</p>
    <input id="code" inputmode="latin" autocapitalize="characters" autocomplete="off" spellcheck="false" maxlength="6" placeholder="ABC123">
    <button type="submit">Connect</button>
    <p class="hint" id="gatemsg"></p>
  </form>
</div>
<div id="inputs"></div>
<script>
let token = new URLSearchParams(location.search).get("t") || localStorage.getItem("t") || "";
if (token) localStorage.setItem("t", token);
let rev = -1;

// Asking for the code beats putting it in the URL: you can add this page to the home
// screen once and type the six characters shown by Performer, rather than re-scanning
// a link every time the app restarts.
function askForCode(message) {
  document.getElementById("gate").style.display = "flex";
  document.getElementById("gatemsg").textContent = message || "";
  document.getElementById("inputs").style.display = "none";
  const f = document.getElementById("codeform");
  f.onsubmit = (e) => {
    e.preventDefault();
    const v = document.getElementById("code").value.trim().toUpperCase();
    if (!v) return;
    token = v; localStorage.setItem("t", v);
    document.getElementById("gate").style.display = "none";
    document.getElementById("inputs").style.display = "";
    refresh(true);
  };
  document.getElementById("code").focus();
}

async function api(path, opts) {
  const r = await fetch(path + (path.includes("?") ? "&" : "?") + "t=" + encodeURIComponent(token), opts);
  if (r.status === 403) { localStorage.removeItem("t"); const e = new Error("gate"); e.gate = true; throw e; }
  if (!r.ok) throw new Error("HTTP " + r.status);
  return r.json();
}
function show(msg) { const e = document.getElementById("err"); e.textContent = msg; e.style.display = msg ? "block" : "none"; }

function render(s) {
  const root = document.getElementById("inputs");
  root.innerHTML = "";
  s.inputs.forEach((inp, i) => {
    const wrap = document.createElement("div"); wrap.className = "input";
    const now = document.createElement("div"); now.className = "now";
    now.innerHTML = '<div class="name"></div><div class="prog"><span class="num"></span><span class="title"></span></div>';
    now.querySelector(".name").textContent = inp.name.toUpperCase();
    now.querySelector(".num").textContent = String(inp.current).padStart(3, "0");
    now.querySelector(".title").textContent = inp.currentName || "(empty)";
    if (inp.loading) { const l = document.createElement("div"); l.className = "loading"; l.textContent = "loading…"; now.appendChild(l); }
    wrap.appendChild(now);
    const grid = document.createElement("div"); grid.className = "grid";
    inp.programs.forEach(p => {
      const b = document.createElement("button");
      b.className = "p" + (p.index === inp.current ? " on" : "");
      b.innerHTML = '<span class="n"></span><span class="t"></span>';
      b.querySelector(".n").textContent = String(p.index).padStart(3, "0");
      b.querySelector(".t").textContent = p.name || "(unnamed)";
      b.onclick = async () => {
        try { await api("/api/select?input=" + i + "&program=" + p.index, { method: "POST" }); await refresh(true); show(""); }
        catch (e) { show(e.message); }
      };
      grid.appendChild(b);
    });
    wrap.appendChild(grid); root.appendChild(wrap);
  });
}

async function refresh(force) {
  try {
    const s = await api("/api/state");
    if (force || s.revision !== rev) { rev = s.revision; render(s); }
    show("");
  } catch (e) {
    if (e.gate) askForCode("That code was not accepted — check Performer and try again.");
    else show(e.message);
  }
}
document.getElementById("panic").onclick = async () => {
  try { await api("/api/panic", { method: "POST" }); show("All notes off sent"); setTimeout(() => show(""), 1500); }
  catch (e) { show(e.message); }
};
if (token) refresh(true); else askForCode("");
setInterval(() => { if (token) refresh(false); }, 1000);
</script></body></html>)HTML";

static const char* kManifest = R"JSON({
  "name": "Performer", "short_name": "Performer",
  "start_url": ".", "display": "standalone",
  "background_color": "#15161c", "theme_color": "#15161c",
  "icons": [{ "src": "/icon.png", "sizes": "256x256", "type": "image/png", "purpose": "any maskable" }]
})JSON";

//==============================================================================
RemoteServer::RemoteServer (Engine& e, PropertiesFile& s)
    : Thread ("performer-remote"), engine (e), settings (s)
{
    engine.addListener (this);
}

RemoteServer::~RemoteServer()
{
    engine.removeListener (this);
    stop();
}

bool RemoteServer::start (int p)
{
    stop();
    // A short code you can read off the screen and type on a phone, kept across
    // restarts so a home-screen shortcut keeps working: a token regenerated every
    // run would break the bookmark exactly when you least want to re-pair, on stage.
    // Letters that misread (O/0, I/1/l) are excluded.
    token = settings.getValue ("remoteCode");
    if (token.length() != 6)
    {
        static const char* alphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
        auto& rng = Random::getSystemRandom();
        token.clear();
        for (int i = 0; i < 6; ++i) token << alphabet[rng.nextInt (31)];
        settings.setValue ("remoteCode", token);
        settings.saveIfNeeded();
    }
    listener = std::make_unique<StreamingSocket>();
    if (! listener->createListener (p))
    {
        listener.reset();
        return false;
    }
    port = p;
    running = true;
    startThread();
    return true;
}

void RemoteServer::stop()
{
    running = false;
    if (listener != nullptr) listener->close();
    stopThread (2000);
    listener.reset();
    port = 0;
}

/** Which address should we print for the phone to type?

    A developer machine can easily have a dozen IPv4 addresses -- Docker and LXD
    bridges, libvirt networks, one per container network -- and a phone can reach
    none of them. Picking the first non-loopback address, as this used to, hands
    the user something like 172.17.0.1 and looks like the feature is broken.

    So rank by interface instead: a wireless interface first (on stage that is
    either the venue's network or our own hotspot), then wired, then anything
    else we do not recognise as virtual, and only then give up. Within wireless
    we prefer a hotspot-shaped address, because if the laptop is serving its own
    network that is certainly the one the phone is on. */
static int addressRank (const String& iface, const String& addr)
{
    // Virtual interfaces: nothing external is ever on the other side of these.
    if (iface.startsWith ("docker") || iface.startsWith ("br-") || iface.startsWith ("virbr")
        || iface.startsWith ("lxdbr") || iface.startsWith ("veth") || iface.startsWith ("vnet")
        || iface.startsWith ("tun") || iface.startsWith ("tap") || iface.startsWith ("vmnet")
        || iface.startsWith ("zt") || iface.startsWith ("wg"))
        return 0;

    const auto wireless = iface.startsWith ("wl") || iface.startsWith ("wlan") || iface.startsWith ("ath");
    const auto wired    = iface.startsWith ("en") || iface.startsWith ("eth");

    // NetworkManager's shared mode always uses 10.42.x: that is our own hotspot.
    if (wireless && addr.startsWith ("10.42.")) return 4;
    if (wireless) return 3;
    if (wired)    return 2;
    return 1;
}

String RemoteServer::getUrl() const
{
    if (! running.load()) return {};
    return "http://" + getHostAddress() + ":" + String (port) + "/";
}

String RemoteServer::getHostAddress()
{
    String best = "127.0.0.1";
    int bestRank = -1;

   #if JUCE_LINUX || JUCE_MAC
    struct ifaddrs* list = nullptr;
    if (::getifaddrs (&list) == 0)
    {
        for (auto* i = list; i != nullptr; i = i->ifa_next)
        {
            if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET) continue;
            if ((i->ifa_flags & IFF_UP) == 0 || (i->ifa_flags & IFF_LOOPBACK) != 0) continue;

            char buf[INET_ADDRSTRLEN] = {};
            auto* in = reinterpret_cast<struct sockaddr_in*> (i->ifa_addr);
            if (::inet_ntop (AF_INET, &in->sin_addr, buf, sizeof (buf)) == nullptr) continue;

            const String iface (i->ifa_name), addr (buf);
            if (addr.startsWith ("127.") || addr.startsWith ("169.254.")) continue;

            if (const auto rank = addressRank (iface, addr); rank > bestRank)
            {
                bestRank = rank;
                best = addr;
            }
        }
        ::freeifaddrs (list);
    }
   #endif

    if (bestRank < 0)
        for (auto& ip : IPAddress::getAllAddresses())
            if (! ip.isNull() && ! ip.toString().startsWith ("127.") && ip.toString().containsChar ('.'))
                { best = ip.toString(); break; }

    return best;
}

//==============================================================================
bool RemoteServer::authorised (const String& request) const
{
    // Matched case-insensitively: the code is shown in capitals but a phone keyboard
    // will happily offer lower case, and being fussy about that on stage is unkind.
    return token.isNotEmpty() && request.containsIgnoreCase ("t=" + token);
}

String RemoteServer::stateJson() const
{
    const auto& setup = engine.getSetup();
    DynamicObject::Ptr root (new DynamicObject());
    root->setProperty ("revision", revision.load());
    Array<var> inputs;
    for (int i = 0; i < (int) setup.inputs.size(); ++i)
    {
        const auto& in = setup.inputs[(size_t) i];
        DynamicObject::Ptr o (new DynamicObject());
        o->setProperty ("name", in.name);
        o->setProperty ("current", in.currentProgram);
        const auto& cur = in.programs[(size_t) in.currentProgram];
        o->setProperty ("currentName", cur.name);
        bool loading = false;
        for (int s = 0; s < (int) cur.slots.size() && ! loading; ++s)
            loading = engine.isPluginLoading (i, in.currentProgram, s);
        o->setProperty ("loading", loading);
        Array<var> progs;
        for (int p = 0; p < InputDef::numPrograms; ++p)
        {
            const auto& def = in.programs[(size_t) p];
            if (def.isEmpty() && def.name.isEmpty()) continue;   // only what you have set up
            DynamicObject::Ptr pd (new DynamicObject());
            pd->setProperty ("index", p);
            pd->setProperty ("name", def.name);
            progs.add (var (pd.get()));
        }
        o->setProperty ("programs", progs);
        inputs.add (var (o.get()));
    }
    root->setProperty ("inputs", inputs);
    return JSON::toString (var (root.get()), true);
}

//==============================================================================
/** Writes raw bytes: the body is already UTF-8, and Content-Length counts bytes,
    not characters. Going through juce::String here once mangled an em-dash in the
    page, because the literal was reinterpreted rather than passed through. */
static void sendBytes (StreamingSocket& s, const String& status, const String& type,
                       const void* body, int len)
{
    String head;
    head << "HTTP/1.1 " << status << "\r\n"
         << "Content-Type: " << type << "\r\n"
         << "Content-Length: " << len << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "Connection: close\r\n\r\n";
    const auto h = head.toRawUTF8();
    s.write (h, (int) strlen (h));
    if (len > 0) s.write (body, len);
}

static void sendResponse (StreamingSocket& s, const String& status, const String& type, const String& body)
{
    const auto utf8 = body.toRawUTF8();
    const int len = (int) strlen (utf8);
    String head;
    head << "HTTP/1.1 " << status << "\r\n"
         << "Content-Type: " << type << "\r\n"
         << "Content-Length: " << len << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "Connection: close\r\n\r\n";
    s.write (head.toRawUTF8(), (int) strlen (head.toRawUTF8()));
    if (len > 0) s.write (utf8, len);
}

void RemoteServer::handle (StreamingSocket& sock)
{
    char buf[4096] = {};
    const int got = sock.read (buf, sizeof (buf) - 1, false);
    if (got <= 0) return;
    const String request (CharPointer_UTF8 (buf), (size_t) got);
    const String line = request.upToFirstOccurrenceOf ("\r\n", false, false);
    const String path = line.fromFirstOccurrenceOf (" ", false, false).upToFirstOccurrenceOf (" ", false, false);

    if (path.startsWith ("/manifest.webmanifest")) { sendBytes (sock, "200 OK", "application/manifest+json", kManifest, (int) strlen (kManifest)); return; }
    if (path.startsWith ("/icon.png"))
    {
        // the app icon, so an installed PWA has one
        MemoryOutputStream png;
        if (auto img = ImageCache::getFromMemory (BinaryData::performer256_png, BinaryData::performer256_pngSize); img.isValid())
            PNGImageFormat().writeImageToStream (img, png);
        String head;
        head << "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: " << (int) png.getDataSize()
             << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        sock.write (head.toRawUTF8(), (int) strlen (head.toRawUTF8()));
        sock.write (png.getData(), (int) png.getDataSize());
        return;
    }

    if (! authorised (path))
    {
        // The page itself is harmless without a token; the API is not.
        if (path.startsWith ("/api/")) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"bad token\"}"); return; }
    }

    if (path.startsWith ("/api/state"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"bad token\"}"); return; }
        sendResponse (sock, "200 OK", "application/json", stateJson());
        return;
    }
    if (path.startsWith ("/api/select"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"bad token\"}"); return; }
        const int input = path.fromFirstOccurrenceOf ("input=", false, false).getIntValue();
        const int prog  = path.fromFirstOccurrenceOf ("program=", false, false).getIntValue();
        // the engine is not thread-safe for this: do it on the message thread
        MessageManager::callAsync ([this, input, prog] { engine.selectProgram (input, prog); });
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (path.startsWith ("/api/panic"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"bad token\"}"); return; }
        MessageManager::callAsync ([this] { engine.panic(); });
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    sendBytes (sock, "200 OK", "text/html; charset=utf-8", kIndexHtml, (int) strlen (kIndexHtml));
}

void RemoteServer::run()
{
    while (! threadShouldExit() && running.load())
    {
        if (listener == nullptr) break;
        std::unique_ptr<StreamingSocket> conn (listener->waitForNextConnection());
        if (conn == nullptr) continue;
        if (conn->waitUntilReady (true, 2000) == 1)
            handle (*conn);
        conn->close();
    }
}

} // namespace perf
