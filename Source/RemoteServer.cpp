#include "RemoteServer.h"
#include "MappingSuggestions.h"
#include "PluginView.h"
#include <thread>
#include <fcntl.h>
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
  .now .title { font-size:22px; font-weight:700; overflow:hidden; text-overflow:ellipsis; white-space:nowrap;
                flex:1; min-width:0; }
  /* The plugin's interface, reachable from the header rather than from the slot
     rows further down: this is the one you want mid-set. Pushed to the right so
     it never crowds the program name, which is what the eye comes here for. */
  .now .gui { margin-left:auto; flex:none; background:var(--row); color:#fff; border:0;
              border-radius:10px; padding:10px 16px; font-size:13px; font-weight:800;
              letter-spacing:.04em; align-self:center; }
  .now .gui:active { background:var(--accent); color:#06121f; }
  .now .gui:disabled { opacity:.6; }

  /* Choosing between the instruments of a split. A sheet, not a dropdown: this
     is hit with a thumb on a stand. */
  .sheet { position:fixed; inset:0; background:#000a; display:flex; align-items:flex-end;
           justify-content:center; z-index:10; padding:16px;
           padding-bottom:calc(16px + env(safe-area-inset-bottom)); }
  .sheet .card { background:var(--panel); border-radius:16px; padding:14px; width:100%;
                 max-width:420px; display:flex; flex-direction:column; gap:8px; }
  .sheet .sheet-title { color:var(--dim); font-size:12px; font-weight:700;
                        letter-spacing:.08em; text-transform:uppercase; padding:4px 4px 6px; }
  .sheet button { background:var(--row); color:#fff; border:0; border-radius:12px;
                  padding:16px; font-size:16px; font-weight:700; text-align:left; }
  .sheet button.cancel { background:none; color:var(--dim); text-align:center; font-size:15px; }
  .loading { color:#d8a657; font-size:14px; }
  .grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(150px,1fr)); gap:8px; }
  button.p { background:var(--row); color:#fff; border:0; border-radius:10px; padding:14px 12px; text-align:left;
             font-size:15px; display:flex; gap:10px; align-items:baseline; min-height:56px; }
  button.p .n { color:var(--dim); font-variant-numeric:tabular-nums; font-weight:700; font-size:13px; }
  button.p.on { background:var(--accent); color:#06121f; }
  button.p.on .n { color:#06121f; }
  /* Group filters. Only drawn when the setup actually uses groups, so someone
     who has not touched the feature sees exactly what they saw before. */
  /* Tempo lives in the header beside PANIC: it is worth a glance and a tap, not
     a band across the top of the screen. */
  .hdr { display:flex; align-items:center; gap:8px; }
  .hdr .bpm { font-size:15px; font-weight:800; color:var(--accent);
              font-variant-numeric:tabular-nums; white-space:nowrap; }
  .hdr .bpm i { font-size:10px; color:var(--dim); font-style:normal; font-weight:600; margin-left:2px; }
  #tap { background:var(--row); color:#fff; border:0; border-radius:8px;
         padding:9px 14px; font-size:13px; font-weight:800; letter-spacing:.04em; }
  #tap:active { background:var(--accent); color:#06121f; }

  .groups { display:flex; flex-wrap:wrap; gap:8px; margin:0 0 10px; }
  .groups button { background:var(--row); color:var(--dim); border:0; border-radius:999px;
                   padding:9px 16px; font-size:13px; font-weight:700; letter-spacing:.02em; }
  .groups button.on { background:var(--accent); color:#06121f; }

  .slots { margin-top:14px; }
  .slot { background:var(--panel); border-radius:12px; padding:12px 14px; margin-bottom:10px; }
  .slot h2 { font-size:13px; color:var(--dim); letter-spacing:.06em; margin:0 0 10px; text-transform:uppercase;
             display:flex; justify-content:space-between; align-items:center; gap:10px; }
  .slot h2 button { background:none; border:1px solid #3a3d47; color:var(--dim); border-radius:8px;
                    padding:6px 12px; font-size:12px; font-weight:700; letter-spacing:.04em; }
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
<header>
  <h1>PERFORMER</h1>
  <div class="hdr">
    <span class="bpm" id="bpm">--<i>bpm</i></span>
    <button id="tap">TAP</button>
    <button id="panic">PANIC</button>
  </div>
</header>
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
</div>
<script>
/* Tapping is timed on the server, where the tempo lives: a few milliseconds of
   network on a local wifi is far below the precision of a finger, and doing the
   averaging in two places would let them disagree. */
document.getElementById("tap").onclick = async () => {
  try {
    const r = await api("/api/tap", { method: "POST" });
    if (r && r.tempo) showTempo(r.tempo);
  } catch (e) { if (!e.gate) show(e.message); }
};

function showTempo(bpm) {
  document.getElementById("bpm").innerHTML =
    (Math.round(bpm * 10) / 10).toFixed(1) + '<i>bpm</i>';
}


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

let lastState = null;
const filters = {};          // input index -> chosen group, "" for all

function render(s) {
  lastState = s;
  if (s.tempo) showTempo(s.tempo);
  const root = document.getElementById("inputs");
  root.innerHTML = "";
  s.inputs.forEach((inp, i) => {
    const wrap = document.createElement("div"); wrap.className = "input";
    const now = document.createElement("div"); now.className = "now";
    now.innerHTML = '<div class="name"></div><div class="prog"><span class="num"></span><span class="title"></span></div>';
    now.querySelector(".name").textContent = inp.name.toUpperCase();
    now.querySelector(".num").textContent = String(inp.current).padStart(3, "0");
    now.querySelector(".title").textContent = inp.currentName || "(empty)";

    /* Straight to the plugin's interface from the input's own header. This is
       the thing you reach for mid-set -- the slot rows below are further down
       the page, and on a tablet on a stand that is a scroll you do not want to
       be doing between songs.

       One slot opens it directly; several offer a choice, because a split with
       an organ over a pad has two interfaces and only you know which one you
       meant. Hidden entirely when the program has no plugins to show. */
    const gui = document.createElement("button");
    gui.className = "gui"; gui.textContent = "GUI";
    gui.style.display = "none";
    gui.onclick = () => {
      const list = slotCache[i] || [];
      if (!list.length) return;
      if (list.length === 1) { openPluginWindow(i, list[0].slot, list[0].name, gui, "GUI"); return; }
      pickSlot(i, list, gui);
    };
    now.querySelector(".prog").appendChild(gui);
    guiButtons[i] = gui;
    syncGuiButton(i);

    if (inp.loading) { const l = document.createElement("div"); l.className = "loading"; l.textContent = "loading…"; now.appendChild(l); }
    wrap.appendChild(now);
    /* Which group is being shown, per input. Kept across refreshes so a poll
       does not throw you back to "All" while you are looking for a sound. */
    const groups = [];
    inp.programs.forEach(p => { if (p.group && !groups.includes(p.group)) groups.push(p.group); });

    if (groups.length) {
      const bar = document.createElement("div"); bar.className = "groups";
      const mk = (label, value) => {
        const b = document.createElement("button");
        b.textContent = label;
        if ((filters[i] || "") === value) b.className = "on";
        b.onclick = () => { filters[i] = value; render(lastState); };
        bar.appendChild(b);
      };
      mk("All", "");
      groups.forEach(gname => mk(gname, gname));
      if (inp.programs.some(p => !p.group)) mk("Other", "\u0000other");
      wrap.appendChild(bar);
    }

    const active = filters[i] || "";
    const grid = document.createElement("div"); grid.className = "grid";
    inp.programs.forEach(p => {
      if (active === "\u0000other") { if (p.group) return; }
      else if (active && p.group !== active) return;
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
    wrap.appendChild(grid);

    const slots = document.createElement("div");
    slots.className = "slots"; slots.id = "slots" + i;
    wrap.appendChild(slots);

    root.appendChild(wrap);
    loadSlots(i);
  });
}

// ---- controls ---------------------------------------------------------------
// Fetched separately from the program list: the programs change rarely, the
// parameter values change while you are touching them.
/* What each input's current program has in it, so the header button knows
   whether to open a window, offer a choice, or stay hidden. Filled by
   loadSlots, which the page already calls for every input on each render. */
const slotCache = {};
const guiButtons = {};

function syncGuiButton(i) {
  const b = guiButtons[i];
  if (!b) return;
  const n = (slotCache[i] || []).length;
  b.style.display = n ? "" : "none";
  b.textContent = "GUI";
}

/* Which instrument's window, when a program has more than one. A small sheet
   rather than a dropdown: it is chosen with a thumb, at arm's length. */
function pickSlot(i, list, btn) {
  const back = document.createElement("div"); back.className = "sheet";
  const card = document.createElement("div"); card.className = "card";
  const t = document.createElement("div"); t.className = "sheet-title";
  t.textContent = "Which instrument?";
  card.appendChild(t);

  list.forEach(sl => {
    const b = document.createElement("button");
    b.textContent = sl.name;
    b.onclick = () => { back.remove(); openPluginWindow(i, sl.slot, sl.name, btn, "GUI"); };
    card.appendChild(b);
  });

  const cancel = document.createElement("button");
  cancel.className = "cancel"; cancel.textContent = "Cancel";
  cancel.onclick = () => back.remove();
  card.appendChild(cancel);

  back.appendChild(card);
  back.onclick = e => { if (e.target === back) back.remove(); };
  document.body.appendChild(back);
}

/* Opens a plugin's own window, mirrored. Some plugins cannot be followed any
   other way -- Kontakt reports nothing when you move a drawbar in it -- so this
   shows the real thing rather than a guess at its state.

   Shared by the button in each input's header and the one on each slot, so both
   land on the same tab for the same plugin. */
async function openPluginWindow(i, slot, fallbackName, btn, restore) {
  const was = btn.textContent;
  btn.textContent = "...";
  btn.disabled = true;
  try {
    const r = await api("/api/pluginview?input=" + i + "&slot=" + slot, { method: "POST" });
    if (r.error) { show(r.error); return; }
    /* Through this same port: a stream on a port of its own would need its own
       hole in the firewall, which is precisely what left the tab loading
       forever the first time. */
    /* Name the tab after the sound, not the plugin: on a tablet the tab strip
       shows a few characters, and "Oye Como" says which one this is where
       "Kontakt 8" does not when three programs all use Kontakt.

       The name is also the window target, so pressing it again for the same
       slot brings that tab forward instead of opening another one. Browsers key
       window targets by name, so this costs nothing. */
    const state = lastState && lastState.inputs && lastState.inputs[i];
    const label = state ? (state.currentName || ("Program " + state.current)) : fallbackName;
    const target = "performer-plugin-" + i + "-" + slot;

    const base = "/view/" + r.port;
    const url = base + "/plugin.html?path="
              + encodeURIComponent(base.slice(1) + "/websockify")
              + "&title=" + encodeURIComponent(label);

    const tab = window.open(url, target);
    if (tab) tab.focus();
  } catch (e) {
    if (!e.gate) show(e.message);
  } finally {
    btn.textContent = restore || was;
    btn.disabled = false;
  }
}

async function loadSlots(i) {
  const host = document.getElementById("slots" + i);
  if (!host) return;
  let data;
  try { data = await api("/api/slots?input=" + i); }
  catch (e) { if (e.gate) throw e; return; }

  host.innerHTML = "";
  slotCache[i] = data.slots;
  syncGuiButton(i);

  data.slots.forEach(sl => {
    const box = document.createElement("div"); box.className = "slot";

    const h = document.createElement("h2");
    const nm = document.createElement("span"); nm.textContent = sl.name;
    const win = document.createElement("button"); win.textContent = "Window";
    win.onclick = () => openPluginWindow(i, sl.slot, sl.name, win, "Window");

    h.appendChild(nm); h.appendChild(win);
    box.appendChild(h);
    host.appendChild(box);
  });
}

/* The plugin's own window is where its controls live now, so the page only has
   to follow the program the keyboard selected. */
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

// Registering this is what makes a browser offer "install" / "Add to Home
// Screen" as a prompt rather than something buried in a menu.
if ('serviceWorker' in navigator)
  addEventListener('load', () => navigator.serviceWorker.register('/sw.js').catch(() => {}));
</script></body></html>)HTML";

static const char* kManifest = R"JSON({
  "name": "Performer", "short_name": "Performer",
  "start_url": ".", "display": "standalone",
  "background_color": "#15161c", "theme_color": "#15161c",
  "icons": [
    { "src": "/icon-192.png", "sizes": "192x192", "type": "image/png", "purpose": "any" },
    { "src": "/icon.png", "sizes": "256x256", "type": "image/png", "purpose": "any" },
    { "src": "/icon-512.png", "sizes": "512x512", "type": "image/png", "purpose": "any" },
    { "src": "/icon-512.png", "sizes": "512x512", "type": "image/png", "purpose": "maskable" }
  ]
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

/* Our own page for a plugin window, rather than noVNC's.

   Its vnc_lite has no viewport meta tag, so a tablet lays the page out at
   desktop width and pinch does nothing; and it can only scale-to-fit, which
   makes a 1010px Kontakt window unreadable on a tablet. This sets the viewport
   so the browser's own pinch-zoom works, and shows the plugin at full size in a
   scrollable box so there is something to zoom into.

   Served from here rather than patched into /usr/share/novnc, which belongs to
   the distribution and would be overwritten by an update. */
static const char* kPluginViewHtml = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,minimum-scale=0.2,maximum-scale=6,user-scalable=yes,viewport-fit=cover">
<meta name="theme-color" content="#15161c">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<link rel="manifest" href="/manifest.webmanifest">
<title>Plugin</title>
<style>
  :root { color-scheme: dark; }
  html,body { margin:0; padding:0; background:#15161c; color:#d7dbe2;
              font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif; }
  /* Dynamic viewport height: 100vh is the height with the browser's chrome
     hidden, so on a tablet it puts the bottom of the plugin permanently under
     the address bar. 100dvh follows the bar as it comes and goes. The 100vh
     line is the fallback for browsers that do not know dvh. */
  #screen { width:100%; height:100vh; height:100dvh; overflow:auto; -webkit-overflow-scrolling:touch; }
  #msg { position:fixed; left:0; right:0; top:0; padding:10px 14px; font-size:14px;
         background:#26282f; border-bottom:1px solid #2b2d35; }
  #bar { position:fixed; right:10px; bottom:10px; display:flex; gap:8px;
         bottom:calc(10px + env(safe-area-inset-bottom)); }
  #bar button { background:#26282fdd; color:#d7dbe2; border:1px solid #3a3d47;
                border-radius:10px; padding:10px 14px; font-size:13px; font-weight:700; }
  #bar button.on { background:#5aa9ff; color:#06121f; border-color:#5aa9ff; }
</style>
</head><body>
<div id="msg">Connecting...</div>
<div id="screen"></div>
<div id="bar"><button id="full">Full</button><button id="fit">Fit</button></div>
<script type="module">
  import RFB from './core/rfb.js';

  const q = new URLSearchParams(location.search);
  const path = q.get('path') || 'websockify';
  const title = q.get('title');
  if (title) document.title = title;
  const url = (location.protocol === 'https:' ? 'wss' : 'ws') + '://'
            + location.hostname + (location.port ? ':' + location.port : '') + '/' + path;

  const msg = document.getElementById('msg');
  const rfb = new RFB(document.getElementById('screen'), url, {});

  /* Clipped rather than scaled: full size inside a scrollable box, so pinching
     magnifies real pixels instead of stretching a shrunken picture. */
  rfb.clipViewport = true;
  rfb.scaleViewport = false;
  rfb.resizeSession = false;

  /* Fullscreen: the address bar is a real cost on a small screen, and a plugin
     window is exactly the thing you want the whole screen for. Only offered
     where the browser has the API -- iOS Safari does not, and there the way to
     lose the bar is the home-screen shortcut, which the manual explains. */
  const full = document.getElementById('full');
  if (document.fullscreenEnabled) {
    full.onclick = () => {
      if (document.fullscreenElement) document.exitFullscreen();
      else document.documentElement.requestFullscreen({ navigationUI: 'hide' }).catch(() => {});
    };
    document.addEventListener('fullscreenchange', () => {
      const on = !!document.fullscreenElement;
      full.className = on ? 'on' : '';
      full.textContent = on ? 'Exit' : 'Full';
    });
  } else {
    full.style.display = 'none';
  }

  let fitting = false;
  const fit = document.getElementById('fit');
  fit.onclick = () => {
    fitting = !fitting;
    rfb.scaleViewport = fitting;
    fit.className = fitting ? 'on' : '';
    fit.textContent = fitting ? 'Actual size' : 'Fit';
  };

  rfb.addEventListener('connect', () => { msg.style.display = 'none'; });
  rfb.addEventListener('disconnect', e => {
    msg.style.display = 'block';
    msg.textContent = e.detail.clean ? 'The plugin window was closed.'
                                     : 'Lost the connection to the plugin.';
  });
</script>
</body></html>)HTML";

/** Copies bytes both ways until one end goes quiet.

    A websocket is long-lived and either side may speak at any time, so this
    cannot be request-then-response like the rest of the server: it has to sit
    on the connection and pump. One thread per open plugin window is acceptable
    -- there is one window, occasionally two. */
bool RemoteServer::relayToPluginView (StreamingSocket& client, const String& firstChunk, int webPort)
{
    StreamingSocket upstream;
    if (! upstream.connect ("127.0.0.1", webPort, 3000))
        return false;

    /* One request per connection. The browser would otherwise send several down
       the same socket, and only the first carries a prefix this relay has
       already stripped -- the rest reached the bridge as /view/6910/core/... and
       404'd, which looked like a flaky module loader.

       Asking the browser to close each connection sidesteps rewriting a stream
       that turns into websocket frames partway through. It costs a connection
       per file, on loopback, once per window opened. */
    auto request = firstChunk;
    if (! request.containsIgnoreCase ("upgrade: websocket"))
        request = request.replace ("\r\nConnection: keep-alive", "\r\nConnection: close")
                         .replace ("\r\nConnection: Keep-Alive", "\r\nConnection: close");

    const auto utf8 = request.toRawUTF8();
    if (upstream.write (utf8, (int) strlen (utf8)) <= 0)
        return false;

    char buf[16384];
    for (;;)
    {
        bool moved = false;

        if (upstream.waitUntilReady (true, 20) == 1)
        {
            const int n = upstream.read (buf, sizeof (buf), false);
            if (n <= 0) break;
            if (client.write (buf, n) <= 0) break;
            moved = true;
        }

        if (client.waitUntilReady (true, 20) == 1)
        {
            const int n = client.read (buf, sizeof (buf), false);
            if (n <= 0) break;
            if (upstream.write (buf, n) <= 0) break;
            moved = true;
        }

        if (! moved && (! client.isConnected() || ! upstream.isConnected()))
            break;
    }

    upstream.close();
    return true;
}

bool RemoteServer::start (int p)
{
    stop();
    // A short code you can read off the screen and type on a tablet, kept across
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
    /* Close this on exec, or every child we start inherits it. An orphaned
       websockify held port 7777 open after Performer had gone, so the next
       start could not bind and the tablet was unreachable until the stray
       process was found by hand. */
    if (! listener->createListener (p))
    {
        listener.reset();
        return false;
    }
    if (const int fd = listener->getRawSocketHandle(); fd >= 0)
        ::fcntl (fd, F_SETFD, FD_CLOEXEC);

    port = p;
    running = true;
    startThread();
    return true;
}

void RemoteServer::stop()
{
    PluginView::stopAll();      // no orphan x11vnc or websockify left behind
    running = false;
    if (listener != nullptr) listener->close();
    stopThread (2000);
    listener.reset();
    port = 0;
}

/** Which address should we print for the tablet to type?

    A developer machine can easily have a dozen IPv4 addresses -- Docker and LXD
    bridges, libvirt networks, one per container network -- and a tablet can reach
    none of them. Picking the first non-loopback address, as this used to, hands
    the user something like 172.17.0.1 and looks like the feature is broken.

    So rank by interface instead: a wireless interface first (on stage that is
    either the venue's network or our own hotspot), then wired, then anything
    else we do not recognise as virtual, and only then give up. Within wireless
    we prefer a hotspot-shaped address, because if the laptop is serving its own
    network that is certainly the one the tablet is on. */
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
    // Matched case-insensitively: the code is shown in capitals but a tablet keyboard
    // will happily offer lower case, and being fussy about that on stage is unkind.
    return token.isNotEmpty() && request.containsIgnoreCase ("t=" + token);
}

String RemoteServer::stateJson() const
{
    const auto& setup = engine.getSetup();
    DynamicObject::Ptr root (new DynamicObject());
    root->setProperty ("revision", revision.load());
    root->setProperty ("tempo", engine.getTempoBpm());
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
            if (def.group.isNotEmpty()) pd->setProperty ("group", def.group);
            progs.add (var (pd.get()));
        }
        o->setProperty ("programs", progs);
        inputs.add (var (o.get()));
    }
    root->setProperty ("inputs", inputs);
    return JSON::toString (var (root.get()), true);
}

/** The current program's slots, so the tablet can offer a window onto each one.

    Grouped by slot because that is how the setup is built and how the desktop
    labels things: "1: Kontakt 8" is the same slot in both views. */
String RemoteServer::slotsJson (int inputIndex) const
{
    DynamicObject::Ptr root (new DynamicObject());
    Array<var> out;

    const auto& setup = engine.getSetup();
    if (inputIndex >= 0 && inputIndex < (int) setup.inputs.size())
    {
        const auto& in = setup.inputs[(size_t) inputIndex];
        const int prog = in.currentProgram;
        const auto& def = in.programs[(size_t) prog];

        for (int sIdx = 0; sIdx < (int) def.slots.size(); ++sIdx)
        {
            const auto& slot = def.slots[(size_t) sIdx];

            // A slot with no plugin has nothing to show.
            if (slot.plugin.name.isEmpty()) continue;

            DynamicObject::Ptr so (new DynamicObject());
            so->setProperty ("slot", sIdx);
            so->setProperty ("effect", -1);
            so->setProperty ("name", String (sIdx + 1) + ": " + slot.plugin.name);
            so->setProperty ("enabled", slot.enabled);

            out.add (var (so.get()));
        }
    }

    root->setProperty ("slots", out);
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

/* Chrome refuses to offer "install" unless the page registers a service worker
   with a fetch handler, so there has to be one -- but caching a stage tool's
   pages would be actively harmful: a tablet showing a stale program list is worse
   than one showing none. This worker therefore always goes to the network and
   only falls back to a cached shell when the network is gone, which is the
   honest behaviour for something whose whole job is to reflect live state. */
static const char* kServiceWorker = R"JS(
const SHELL = 'performer-shell-v1';

self.addEventListener('install', e => {
  e.waitUntil(caches.open(SHELL).then(c => c.addAll(['./'])).then(() => self.skipWaiting()));
});

self.addEventListener('activate', e => {
  e.waitUntil(caches.keys()
    .then(ks => Promise.all(ks.filter(k => k !== SHELL).map(k => caches.delete(k))))
    .then(() => self.clients.claim()));
});

self.addEventListener('fetch', e => {
  const url = new URL(e.request.url);
  // Never serve state or commands from a cache: stale is worse than absent.
  if (url.pathname.startsWith('/api/')) return;

  e.respondWith(
    fetch(e.request)
      .then(r => {
        if (r && r.ok && e.request.method === 'GET' && url.pathname === '/')
          caches.open(SHELL).then(c => c.put('./', r.clone()));
        return r;
      })
      .catch(() => caches.match(e.request).then(hit => hit || caches.match('./')))
  );
});
)JS";

void RemoteServer::handle (StreamingSocket& sock, bool& takeOver)
{
    takeOver = false;
    char buf[4096] = {};
    const int got = sock.read (buf, sizeof (buf) - 1, false);
    if (got <= 0) return;
    const String request (CharPointer_UTF8 (buf), (size_t) got);
    const String line = request.upToFirstOccurrenceOf ("\r\n", false, false);
    const String path = line.fromFirstOccurrenceOf (" ", false, false).upToFirstOccurrenceOf (" ", false, false);

    if (path.startsWith ("/manifest.webmanifest")) { sendBytes (sock, "200 OK", "application/manifest+json", kManifest, (int) strlen (kManifest)); return; }
    if (path.startsWith ("/sw.js"))
    {
        sendBytes (sock, "200 OK", "text/javascript; charset=utf-8",
                   kServiceWorker, (int) strlen (kServiceWorker));
        return;
    }

    // The app icon, so an installed web app has one. Chrome wants at least 192
    // before it will offer to install, and 512 for the splash screen.
    if (path.startsWith ("/icon"))
    {
        const void* src = BinaryData::performer256_png;
        int srcSize = BinaryData::performer256_pngSize;
        int resize = 0;

        if (path.startsWith ("/icon-512"))
        {
            src = BinaryData::performer512_png;
            srcSize = BinaryData::performer512_pngSize;
        }
        else if (path.startsWith ("/icon-192"))
        {
            // No 192 asset: scale the 512 down, which is sharper than scaling up.
            src = BinaryData::performer512_png;
            srcSize = BinaryData::performer512_pngSize;
            resize = 192;
        }

        MemoryOutputStream png;
        if (auto img = ImageCache::getFromMemory (src, srcSize); img.isValid())
        {
            if (resize > 0)
                img = img.rescaled (resize, resize, Graphics::highResamplingQuality);
            PNGImageFormat().writeImageToStream (img, png);
        }
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

    /* A plugin window's stream, relayed so it uses this port rather than one of
       its own. The port is in the path because the tablet has no other way to
       say which window it wants. */
    if (path.startsWith ("/view/"))
    {
        const auto rest = path.fromFirstOccurrenceOf ("/view/", false, false);
        const int wanted = rest.upToFirstOccurrenceOf ("/", false, false).getIntValue();

        bool known = false;
        for (const auto& s : PluginView::active())
            if (s.webPort == wanted) known = true;

        if (! known)
        {
            sendResponse (sock, "404 Not Found", "text/plain", "No plugin window is being shown.");
            return;
        }

        // Our own page; everything else goes to the bridge untouched.
        if (rest.fromFirstOccurrenceOf ("/", false, false).startsWith ("plugin.html"))
        {
            sendBytes (sock, "200 OK", "text/html; charset=utf-8",
                       kPluginViewHtml, (int) strlen (kPluginViewHtml));
            return;
        }

        // Strip the /view/<port> prefix so the bridge sees the path it expects.
        auto forwarded = request;
        const auto prefix = "/view/" + String (wanted);
        forwarded = forwarded.replaceFirstOccurrenceOf (prefix, String());

        /* Hand the socket to its own thread. A websocket stays open for as long
           as someone is looking at the plugin, and this server accepts one
           connection at a time: relaying inline wedged everything, the page's
           own polling included, the moment a window was opened. */
        takeOver = true;
        std::thread ([this, sockPtr = &sock, forwarded, wanted]
        {
            relayToPluginView (*sockPtr, forwarded, wanted);
            sockPtr->close();
            delete sockPtr;
        }).detach();
        return;
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
    if (path.startsWith ("/api/slots"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"bad token\"}"); return; }
        const int input = path.fromFirstOccurrenceOf ("input=", false, false).getIntValue();
        sendResponse (sock, "200 OK", "application/json", slotsJson (input));
        return;
    }
    if (path.startsWith ("/api/tap"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"bad token\"}"); return; }

        /* Tapped on the tablet. The tap has to be timed where it happens, and it
           happens here: the round trip from a music stand is a few milliseconds
           on a local network, far below the precision of a human finger. What
           would ruin it is waiting for the next poll, so this answers with the
           new tempo rather than making the page wait a second to see it. */
        double bpm = 0.0;
        WaitableEvent done;
        MessageManager::callAsync ([this, &bpm, &done]
        {
            bpm = engine.tapTempo();
            if (bpm <= 0.0) bpm = engine.getTempoBpm();
            done.signal();
        });
        done.wait (500);
        ++revision;

        DynamicObject::Ptr o (new DynamicObject());
        o->setProperty ("tempo", bpm);
        sendResponse (sock, "200 OK", "application/json", JSON::toString (var (o.get()), true));
        return;
    }
    if (path.startsWith ("/api/settempo"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"bad token\"}"); return; }
        const double bpm = path.fromFirstOccurrenceOf ("bpm=", false, false).getDoubleValue();
        MessageManager::callAsync ([this, bpm] { engine.setTempoBpm (bpm); engine.resetTapTempo(); });
        ++revision;
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (path.startsWith ("/api/pluginview"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"bad token\"}"); return; }
        const int input = path.fromFirstOccurrenceOf ("input=", false, false).getIntValue();
        const int slot  = path.fromFirstOccurrenceOf ("slot=", false, false).getIntValue();

        /* Mirrors the plugin's own window. Needed because some plugins cannot be
           followed any other way: Kontakt's host-visible parameters are MIDI
           controller inputs, so reading one back gives what was last written to
           it, never where the drawbar actually is. */
        String title, error;
        WaitableEvent ready;
        MessageManager::callAsync ([this, input, slot, &title, &error, &ready]
        {
            const auto& setup = engine.getSetup();
            if (input >= 0 && input < (int) setup.inputs.size())
            {
                const auto& in = setup.inputs[(size_t) input];
                const int prog = in.currentProgram;
                const auto& def = in.programs[(size_t) prog];
                if (slot >= 0 && slot < (int) def.slots.size())
                {
                    auto* plugin = engine.getPlugin (input, prog, slot, -1);
                    if (plugin == nullptr || ! plugin->isAlive())
                        error = "That plugin is not running.";
                    else if (! plugin->isEditorOpen() && ! plugin->showEditor (
                                 in.name + " / " + String (prog).paddedLeft ('0', 3)
                                 + "  " + def.name + " / " + plugin->getName()))
                        error = "Could not open that plugin's window.";
                    else
                        title = in.name + " / " + String (prog).paddedLeft ('0', 3)
                              + "  " + def.name + " / " + plugin->getName();
                }
            }
            ready.signal();
        });
        ready.wait (20000);

        DynamicObject::Ptr o (new DynamicObject());
        if (error.isNotEmpty() || title.isEmpty())
        {
            o->setProperty ("error", error.isNotEmpty() ? error : String ("No plugin in that slot."));
        }
        else
        {
            String startError;
            // Give the window a moment to appear before looking for it.
            for (int i = 0; i < 20 && PluginView::findWindow (title) == 0; ++i)
                Thread::sleep (100);

            const auto session = PluginView::start (title, startError);
            if (! session.isValid()) o->setProperty ("error", startError);
            else                     o->setProperty ("port", session.webPort);
        }
        sendResponse (sock, "200 OK", "application/json", JSON::toString (var (o.get()), true));
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

        // A relayed plugin stream takes the socket with it and closes it itself.
        bool takenOver = false;
        if (conn->waitUntilReady (true, 2000) == 1)
            handle (*conn, takenOver);

        if (takenOver) { conn.release(); continue; }
        conn->close();
    }
}

} // namespace perf
