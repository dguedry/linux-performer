#include "RemoteServer.h"
#include "BinaryData.h"
#include <juce_graphics/juce_graphics.h>

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
</style></head>
<body>
<header><h1>PERFORMER</h1><button id="panic">PANIC</button></header>
<div id="err"></div>
<div id="inputs"></div>
<script>
const token = new URLSearchParams(location.search).get("t") || localStorage.getItem("t") || "";
if (token) localStorage.setItem("t", token);
let rev = -1;

async function api(path, opts) {
  const r = await fetch(path + (path.includes("?") ? "&" : "?") + "t=" + encodeURIComponent(token), opts);
  if (!r.ok) throw new Error(r.status === 403 ? "Wrong or missing token — reopen the link from Performer" : "HTTP " + r.status);
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
  } catch (e) { show(e.message); }
}
document.getElementById("panic").onclick = async () => {
  try { await api("/api/panic", { method: "POST" }); show("All notes off sent"); setTimeout(() => show(""), 1500); }
  catch (e) { show(e.message); }
};
refresh(true);
setInterval(refresh, 1000);
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
    // A token per run: a stale bookmark from a previous gig cannot drive this one.
    token = String::toHexString (Random::getSystemRandom().nextInt64()).removeCharacters ("-");
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

String RemoteServer::getUrl() const
{
    if (! running.load()) return {};
    String host = "127.0.0.1";
    for (auto& ip : IPAddress::getAllAddresses())
        if (! ip.isNull() && ! ip.toString().startsWith ("127.") && ip.toString().containsChar ('.'))
            { host = ip.toString(); break; }
    return "http://" + host + ":" + String (port) + "/?t=" + token;
}

//==============================================================================
bool RemoteServer::authorised (const String& request) const
{
    return token.isNotEmpty() && request.contains ("t=" + token);
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

    if (path.startsWith ("/manifest.webmanifest")) { sendResponse (sock, "200 OK", "application/manifest+json", kManifest); return; }
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
    sendResponse (sock, "200 OK", "text/html; charset=utf-8", kIndexHtml);
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
