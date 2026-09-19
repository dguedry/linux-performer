#include "RemoteServer.h"
#include "MappingSuggestions.h"
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
  /* Controls. Tall rows and a fat thumb: this is aimed at by a finger, on a
     stand, in bad light -- not clicked with a mouse. */
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
  /* Faders stand up and sit side by side, the way drawbars do on the organ
     itself: nine of them across one row instead of nine rows down the page.
     They wrap when there are more than fit, and fall back to one wide
     horizontal fader for a lone continuous control, where a tall thin column
     would just be harder to hit. */
  /* One row wherever it can manage it: nine drawbars belong side by side, not
     six and then three. The faders share the width rather than taking a fixed
     size, down to a floor that still leaves something to grab. */
  .banks { display:flex; flex-wrap:wrap; gap:4px; margin:10px 0 4px; }
  .vert { display:flex; flex-direction:column; align-items:center; gap:3px;
          background:var(--row); border-radius:8px; padding:8px 2px 6px;
          flex:1 1 0; min-width:34px; max-width:56px; }
  .vert .v { font-size:12px; font-weight:700; color:var(--accent);
             font-variant-numeric:tabular-nums; line-height:1; }
  .vert .nm { font-size:10px; color:var(--dim); text-align:center; line-height:1.15;
              overflow:hidden; text-overflow:ellipsis; white-space:nowrap; max-width:100%; }
  /* A vertical range input: rotated, because writing-mode support is uneven
     across the mobile browsers this has to work on. */
  .vert .track { height:120px; width:34px; display:flex; align-items:center; justify-content:center; }
  .vert input[type=range] { width:120px; transform:rotate(-90deg); -webkit-appearance:none;
                            appearance:none; background:transparent; margin:0; }
  .vert input[type=range]::-webkit-slider-runnable-track { height:6px; border-radius:3px; background:var(--panel); }
  .vert input[type=range]::-moz-range-track { height:6px; border-radius:3px; background:var(--panel); }
  .vert input[type=range]::-webkit-slider-thumb { -webkit-appearance:none; width:26px; height:26px;
      margin-top:-10px; border-radius:6px; background:var(--accent); border:0; }
  .vert input[type=range]::-moz-range-thumb { width:26px; height:26px; border-radius:6px; background:var(--accent); border:0; }

  /* Controls are compact: the label and value share one line with the fader
     rather than sitting above it. Still a 26px thumb -- this is aimed at with a
     finger. */
  .ctl { margin:9px 0; }
  .ctl .lab { display:flex; justify-content:space-between; font-size:13px; margin-bottom:2px; gap:10px; }
  .ctl .lab .v { color:var(--dim); font-variant-numeric:tabular-nums; font-size:12px; }
  .ctl input[type=range] { width:100%; height:26px; -webkit-appearance:none; appearance:none; background:transparent; }
  .ctl input[type=range]::-webkit-slider-runnable-track { height:6px; border-radius:3px; background:var(--row); }
  .ctl input[type=range]::-moz-range-track { height:6px; border-radius:3px; background:var(--row); }
  .ctl input[type=range]::-webkit-slider-thumb { -webkit-appearance:none; width:26px; height:26px; margin-top:-10px;
      border-radius:50%; background:var(--accent); border:0; }
  .ctl input[type=range]::-moz-range-thumb { width:26px; height:26px; border-radius:50%; background:var(--accent); border:0; }

  /* Switches sit on one row, two to a line where they fit: an organ has a lot
     of them and each one only needs to say on or off. */
  .sw { display:flex; justify-content:space-between; align-items:center; gap:10px; margin:7px 0; }
  .sw .nm { font-size:13px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .sw button { border:0; border-radius:8px; padding:7px 0; font-size:12px; font-weight:800;
               background:var(--row); color:var(--dim); width:62px; flex:none; letter-spacing:.04em; }
  .sw button.on { background:var(--accent); color:#06121f; }
  .empty { color:var(--dim); font-size:14px; margin:6px 0 2px; }

  /* Picking what deserves a slider. Behind a button so the playing view stays
     big controls and nothing else. */
  #pick { display:none; position:fixed; inset:0; background:var(--bg); z-index:10; overflow-y:auto; padding:14px; }
  /* The title, the search box and Done stack rather than compete for one row:
     a plugin name like "1: Hammond B-3X" squeezed beside a search field wraps to
     three lines and looks broken. */
  #pick header { position:sticky; top:0; background:var(--bg); padding:0 0 10px; display:block; }
  #pick h3 { margin:0 0 10px; font-size:17px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  #pick input[type=search] { width:100%; font-size:17px; padding:12px; border-radius:10px;
      border:2px solid #3a3d47; background:var(--panel); color:#fff; }
  #pick input[type=search]:focus { outline:none; border-color:var(--accent); }
  #pick .done { width:100%; margin-top:10px; background:var(--accent); color:#06121f; border:0;
                border-radius:12px; padding:14px; font-size:16px; font-weight:700; }
  .prow { display:flex; align-items:center; gap:12px; padding:13px 10px; border-bottom:1px solid #2b2d35; font-size:15px; }
  .prow .nm { flex:1; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .prow .tick { width:30px; height:30px; border-radius:8px; border:2px solid #3a3d47; flex:none; }
  .prow.on .tick { background:var(--accent); border-color:var(--accent); }
  .note { color:var(--dim); font-size:13px; padding:10px 2px; }
  .prow .det { color:var(--dim); font-size:12px; margin-left:8px; }
  .more { width:100%; margin:14px 0 30px; background:var(--panel); color:var(--dim); border:1px solid #3a3d47;
          border-radius:10px; padding:13px; font-size:14px; }

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
<div id="pick">
  <header>
    <h3 id="picktitle">Choose controls</h3>
    <input id="picksearch" type="search" inputmode="search" autocomplete="off" placeholder="Search parameters">
    <button class="done" id="pickdone">Done</button>
  </header>
  <div id="picklist"></div>
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

/* How to repaint each control when the plugin moves it. Keyed by slot and
   parameter, rebuilt whenever the controls are, so a stale entry cannot paint
   a widget that is no longer on the page. */
let painters = {};
let paramRev = -1;

let picking = null;          // the slot whose controls are being chosen, if any
let showAll = false;         // include the per-channel copies in the picker
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
async function loadSlots(i) {
  const host = document.getElementById("slots" + i);
  if (!host) return;
  let data;
  try { data = await api("/api/slots?input=" + i); }
  catch (e) { if (e.gate) throw e; return; }

  host.innerHTML = "";
  Object.keys(painters).forEach(k => { if (k.startsWith(i + "/")) delete painters[k]; });
  if (data.params !== undefined) paramRev = data.params;
  data.slots.forEach(sl => {
    const box = document.createElement("div"); box.className = "slot";

    const h = document.createElement("h2");
    const nm = document.createElement("span"); nm.textContent = sl.name;
    const ed = document.createElement("button"); ed.textContent = "Choose";
    ed.onclick = () => openPicker(i, sl);
    h.appendChild(nm); h.appendChild(ed);
    box.appendChild(h);

    if (!sl.live) {
      const e = document.createElement("div"); e.className = "empty";
      e.textContent = "This plugin is still loading.";
      box.appendChild(e);
    } else if (!sl.params.length) {
      const e = document.createElement("div"); e.className = "empty";
      e.textContent = "No controls chosen yet. Press Choose to pick the ones you reach for.";
      box.appendChild(e);
    }

    /* Faders stand up and share a row; switches stay full-width rows, where the
       name has somewhere to go. A single fader on its own is left horizontal:
       one tall thin column is harder to hit than a wide one and saves nothing. */
    const faders = sl.params.filter(p => !p.boolean);
    const switches = sl.params.filter(p => p.boolean);

    if (faders.length > 1) {
      const bank = document.createElement("div"); bank.className = "banks";
      faders.forEach(pr => bank.appendChild(vertical(i, sl, pr)));
      box.appendChild(bank);
    } else {
      faders.forEach(pr => box.appendChild(control(i, sl, pr)));
    }
    switches.forEach(pr => box.appendChild(control(i, sl, pr)));
    host.appendChild(box);
  });
}

/* One control. A switch gets a button rather than a slider, because a two-value
   parameter on a fader is fiddly to hit and reads as broken. */
/* One upright fader. The label is the distinguishing part of the name -- nine
   parameters called "Upper Drawbar N" differ only in the last word -- with the
   whole name on the tooltip for anyone who hovers. */
function vertical(i, sl, pr) {
  const wrap = document.createElement("div"); wrap.className = "vert";
  wrap.title = pr.name;
  const key = i + "/" + sl.slot + "/" + sl.effect + "/" + pr.id;

  const stepped = pr.steps > 1 && pr.steps <= 32;
  const vv = document.createElement("div"); vv.className = "v";

  const track = document.createElement("div"); track.className = "track";
  const r = document.createElement("input");
  r.type = "range";
  r.min = 0;
  r.max = stepped ? pr.steps - 1 : 1000;
  r.step = 1;
  r.value = stepped ? Math.round(pr.value * (pr.steps - 1)) : Math.round(pr.value * 1000);

  const readOut = () => stepped ? String(r.value) : Math.round(r.value / 10) + "%";
  const normalised = () => stepped ? (r.value / (pr.steps - 1)) : (r.value / 1000);
  vv.textContent = readOut();

  let pendingTimer = null, lastSent = 0;
  const send = async () => {
    pendingTimer = null; lastSent = Date.now();
    try { await setParam(i, sl, pr.id, normalised()); } catch (e) { show(e.message); }
  };
  r.oninput = () => {
    vv.textContent = readOut();
    if (pendingTimer) return;
    pendingTimer = setTimeout(send, Math.max(0, 40 - (Date.now() - lastSent)));
  };
  r.onchange = send;

  const nm = document.createElement("div"); nm.className = "nm";
  const words = pr.name.trim().split(/\s+/);
  nm.textContent = words.length > 1 ? words[words.length - 1] : pr.name;

  /* Follow the plugin when it moves this itself -- its own preset menu, a knob
     in its window -- but never while a finger is on it, and never so soon after
     a send that our own change bounces back and fights the drag. */
  painters[key] = v => {
    if (document.activeElement === r) return;
    if (Date.now() - lastSent < 400) return;
    r.value = stepped ? Math.round(v * (pr.steps - 1)) : Math.round(v * 1000);
    vv.textContent = readOut();
  };

  track.appendChild(r);
  wrap.appendChild(vv); wrap.appendChild(track); wrap.appendChild(nm);
  return wrap;
}

function control(i, sl, pr) {
  const wrap = document.createElement("div"); wrap.className = "ctl";

  if (pr.boolean) {
    const row = document.createElement("div"); row.className = "sw";
    const lab = document.createElement("span"); lab.className = "nm"; lab.textContent = pr.name;
    const b = document.createElement("button");
    const paint = v => { b.textContent = v >= 0.5 ? "ON" : "OFF"; b.className = v >= 0.5 ? "on" : ""; };
    paint(pr.value);
    let switched = 0;
    b.onclick = async () => {
      const next = (b.className === "on") ? 0 : 1;
      switched = Date.now();
      paint(next);
      try { await setParam(i, sl, pr.id, next); } catch (e) { show(e.message); }
    };
    painters[i + "/" + sl.slot + "/" + sl.effect + "/" + pr.id] =
      v => { if (Date.now() - switched > 400) paint(v); };
    row.appendChild(lab); row.appendChild(b);
    wrap.appendChild(row);
    return wrap;
  }

  const lab = document.createElement("div"); lab.className = "lab";
  const nm = document.createElement("span"); nm.textContent = pr.name;
  const vv = document.createElement("span"); vv.className = "v";
  lab.appendChild(nm); lab.appendChild(vv);

  /* A parameter with a handful of positions -- a drawbar has nine -- gets a
     fader that snaps to them and reads out the position, rather than a
     percentage that can never land on a real setting. */
  const stepped = pr.steps > 1 && pr.steps <= 32;
  const r = document.createElement("input");
  r.type = "range";
  r.min = 0;
  r.max = stepped ? pr.steps - 1 : 1000;
  r.step = 1;
  r.value = stepped ? Math.round(pr.value * (pr.steps - 1)) : Math.round(pr.value * 1000);

  const readOut = () => stepped ? String(r.value)
                                : Math.round(r.value / 10) + "%";
  const normalised = () => stepped ? (r.value / (pr.steps - 1)) : (r.value / 1000);
  vv.textContent = readOut();

  /* Send while dragging so it feels live, but no faster than the plugin can
     keep up with: a finger drag fires far more events than are useful, and
     flooding the control channel makes the sound lag behind the finger. */
  let pendingTimer = null, lastSent = 0;
  const send = async () => {
    pendingTimer = null;
    lastSent = Date.now();
    try { await setParam(i, sl, pr.id, normalised()); } catch (e) { show(e.message); }
  };
  r.oninput = () => {
    vv.textContent = readOut();
    if (pendingTimer) return;
    const wait = Math.max(0, 40 - (Date.now() - lastSent));
    pendingTimer = setTimeout(send, wait);
  };
  r.onchange = send;      // always send the value the finger settled on

  painters[i + "/" + sl.slot + "/" + sl.effect + "/" + pr.id] = v => {
    if (document.activeElement === r) return;
    if (Date.now() - lastSent < 400) return;
    r.value = stepped ? Math.round(v * (pr.steps - 1)) : Math.round(v * 1000);
    vv.textContent = readOut();
  };

  wrap.appendChild(lab); wrap.appendChild(r);
  return wrap;
}

async function setParam(i, sl, id, value) {
  await api("/api/setparam?input=" + i + "&slot=" + sl.slot + "&effect=" + sl.effect
            + "&id=" + encodeURIComponent(id) + "&value=" + value, { method: "POST" });
}

// ---- choosing which parameters get a control --------------------------------

function openPicker(i, sl) {
  picking = { input: i, slot: sl };
  document.getElementById("picktitle").textContent = sl.name;
  document.getElementById("picksearch").value = "";
  document.getElementById("pick").style.display = "block";
  fillPicker("");
}

async function fillPicker(q) {
  if (!picking) return;
  const list = document.getElementById("picklist");
  let data;
  try {
    data = await api("/api/params?input=" + picking.input + "&slot=" + picking.slot.slot
                     + "&effect=" + picking.slot.effect + "&q=" + encodeURIComponent(q)
                     + (showAll ? "&all=1" : ""));
  } catch (e) { if (!e.gate) show(e.message); return; }

  list.innerHTML = "";

  /* A plugin that names nothing cannot be given useful controls, and saying so
     is kinder than 300 rows of "<unassigned>". */
  if (!q && data.named === 0 && data.total > 0) {
    const n = document.createElement("div"); n.className = "note";
    n.textContent = "This plugin does not name its controls: of its " + data.total
                  + " parameters none have usable names, so there is nothing worth a slider. "
                  + "Use its own window on the computer instead.";
    list.appendChild(n);
  }

  if (data.shown === 0) {
    const n = document.createElement("div"); n.className = "note";
    n.textContent = q
      ? "Nothing here matches \u201c" + q + "\u201d. This plugin has " + data.total + " parameters."
      : "This plugin reports no parameters that can be given a control.";
    list.appendChild(n);
  } else if (data.shown < data.total) {
    const n = document.createElement("div"); n.className = "note";
    n.textContent = q
      ? data.shown + " of " + data.total + " parameters match."
      : "Showing " + data.shown + " of " + data.total + " parameters. Type to narrow the list.";
    list.appendChild(n);
  }

  /* Same bargain the desktop picker offers: the per-channel copies and repeated
     names are folded away, and you can ask for them. */
  if (data.hidden > 0 || showAll) {
    const t = document.createElement("button"); t.className = "more";
    t.textContent = showAll ? "Hide repeated and other-channel copies"
                            : "Show " + data.hidden + " more (other MIDI channels and repeats)";
    t.onclick = () => { showAll = !showAll; fillPicker(document.getElementById("picksearch").value.trim()); };
    list.appendChild(t);
  }
  data.params.forEach(pr => {
    const row = document.createElement("div");
    row.className = "prow" + (pr.chosen ? " on" : "");
    const nm = document.createElement("div"); nm.className = "nm";
    nm.textContent = pr.name;
    if (pr.detail) {
      const d = document.createElement("span"); d.className = "det"; d.textContent = pr.detail;
      nm.appendChild(d);
    }
    const tick = document.createElement("div"); tick.className = "tick";
    row.appendChild(nm); row.appendChild(tick);
    row.onclick = async () => {
      const on = !row.classList.contains("on");
      row.classList.toggle("on", on);
      try {
        await api("/api/favourite?input=" + picking.input + "&slot=" + picking.slot.slot
                  + "&id=" + encodeURIComponent(pr.id) + "&on=" + (on ? 1 : 0), { method: "POST" });
      } catch (e) { show(e.message); row.classList.toggle("on", !on); }
    };
    list.appendChild(row);
  });
}

let pickTimer = null;
document.getElementById("picksearch").oninput = e => {
  clearTimeout(pickTimer);
  pickTimer = setTimeout(() => fillPicker(e.target.value.trim()), 180);
};
document.getElementById("pickdone").onclick = () => {
  document.getElementById("pick").style.display = "none";
  const i = picking ? picking.input : 0;
  picking = null;
  loadSlots(i);
};

/* Values only. A plugin changing its own program moves every drawbar at once,
   and the page has to follow without rebuilding: rebuilding would drop the
   group filter, close a picker and interrupt a drag. */
async function refreshValues() {
  if (picking || !lastState) return;
  for (let i = 0; i < lastState.inputs.length; ++i) {
    let data;
    try { data = await api("/api/slots?input=" + i); }
    catch (e) { return; }
    data.slots.forEach(sl => {
      sl.params.forEach(pr => {
        const f = painters[i + "/" + sl.slot + "/" + sl.effect + "/" + pr.id];
        if (f) f(pr.value);
      });
    });
    if (data.params !== undefined) paramRev = data.params;
  }
}

async function refresh(force) {
  /* Never rebuild the page while the picker is open: choosing a control bumps
     the revision, and re-rendering underneath would shut the picker on the
     first tap. */
  if (picking) return;
  try {
    const s = await api("/api/state");
    if (force || s.revision !== rev) { rev = s.revision; render(s); }
    // A parameter moved inside a plugin: refresh the faders, not the page.
    else if (s.params !== undefined && s.params !== paramRev) { paramRev = s.params; refreshValues(); }
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
    : Thread ("performer-remote"), engine (e), settings (s),
      favourites (s.getFile().getSiblingFile ("favourites.json"))
{
    engine.addListener (this);
}

RemoteServer::~RemoteServer()
{
    engine.removeListener (this);
    stop();
}

/** Moves choices made before controls were per instance onto the slots that
    were showing them.

    Without this, everyone who had already picked controls would open the phone
    after an update and find them gone -- the data is still on disk, just keyed
    by plugin rather than by slot. Runs once per slot: a slot that has anything
    of its own is left alone. */
void RemoteServer::migrateFavouritesToSlots()
{
    auto setup = engine.getSetup();
    bool changed = false;

    for (int i = 0; i < (int) setup.inputs.size(); ++i)
        for (int prog = 0; prog < InputDef::numPrograms; ++prog)
        {
            const auto& def = setup.inputs[(size_t) i].programs[(size_t) prog];
            for (int sIdx = 0; sIdx < (int) def.slots.size(); ++sIdx)
            {
                if (! def.slots[(size_t) sIdx].phoneControls.empty()) continue;
                for (const auto& id : favourites.get (def.slots[(size_t) sIdx].plugin))
                {
                    engine.setSlotPhoneControl (i, prog, sIdx, id, true);
                    changed = true;
                }
            }
        }

    if (changed)
        ++revision;
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
    migrateFavouritesToSlots();
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
    root->setProperty ("tempo", engine.getTempoBpm());
    root->setProperty ("params", paramRevision.load());
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

/** A placeholder the plugin never really named, so it sorts below anything with
    a meaningful name. Plugins spell these differently and there is no standard:

      Kontakt 8     "#000" .. "#2048"        (2049 empty automation slots)
      Numa Player   "<unassigned>"           (64 of them) and "MIDI CC 0|0" ..
      others        "Param 17", "" , "-"

    Guessing from the name is unavoidable here: none of these carry any flag
    saying "this is empty". Being wrong only changes the order, never whether a
    parameter can be chosen, so a false positive costs a scroll rather than a
    control. */
bool RemoteServer::isPlaceholderName (const String& name)
{
    const auto t = name.trim();
    if (t.isEmpty()) return true;
    if (t == "-" || t == "--") return true;

    // "<unassigned>", "<none>", "<empty>": anything a plugin brackets like that.
    if (t.startsWithChar ('<') && t.endsWithChar ('>')) return true;

    // "#000", "Param 17", "Parameter 3": a word plus a bare number.
    if (t.startsWithChar ('#') && t.substring (1).containsOnly ("0123456789")) return true;
    if (t.startsWithIgnoreCase ("param"))
    {
        const auto rest = t.fromFirstOccurrenceOf (" ", false, false).trim();
        if (rest.isNotEmpty() && rest.containsOnly ("0123456789")) return true;
    }

    /* "MIDI CC 0|0": a controller proxy the plugin named after the controller
       rather than after what it does. Useful to reach deliberately, but it
       should never crowd out the plugin's own controls. */
    if (t.startsWithIgnoreCase ("midi cc")) return true;

    return false;
}

/** Is this parameter a switch rather than something to sweep?

    Plugins are unreliable about saying so. Hammond B-3X reports every one of
    its parameters as non-boolean and continuous, including "Volume Switch" and
    "Percussion Switch", so trusting isBoolean alone gives a fader for something
    with two positions.

    Two steps is the strong signal. Failing that, a name that ends in "Switch"
    or reads like on/off is a good guess -- and a wrong guess is cheap here,
    since a switch shown as a fader still works and vice versa. */
bool RemoteServer::isSwitchLike (bool boolFlag, int numSteps, const String& name)
{
    if (boolFlag) return true;
    if (numSteps == 2) return true;

    const auto n = name.trim();
    if (n.endsWithIgnoreCase (" switch") || n.endsWithIgnoreCase (" on/off")
        || n.endsWithIgnoreCase (" enable") || n.endsWithIgnoreCase (" bypass")
        || n.equalsIgnoreCase ("bypass") || n.endsWithIgnoreCase (" mute"))
        return true;
    return false;
}

static bool looksLikeSwitch (const ParamInfo& p)
{
    return RemoteServer::isSwitchLike (p.boolean, p.numSteps, p.name);
}

/** Human name for a VST3 controller number, matching the desktop picker. */
static String controllerLabel (int controller)
{
    if (controller < 0)    return {};
    if (controller < 128)  return "CC " + String (controller);
    if (controller == 128) return "Aftertouch";
    if (controller == 129) return "Pitch bend";
    if (controller == 130) return "Program change";
    return {};
}

/** The current program's slots, each with the parameters chosen for its plugin.

    Grouped by slot because that is how the setup is built and how the desktop
    labels things: "1: Kontakt 8" is the same slot in both views. A split with an
    organ on one slot and a pad on another should not merge into one list of
    knobs with no clue which sound they belong to. */
String RemoteServer::slotsJson (int inputIndex) const
{
    DynamicObject::Ptr root (new DynamicObject());
    root->setProperty ("params", paramRevision.load());
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

            Array<var> sliders;
            auto* plugin = engine.getPlugin (inputIndex, prog, sIdx, -1);
            const bool live = plugin != nullptr;
            so->setProperty ("live", live);

            if (live)
            {
                const auto& params = plugin->getParameters();
                /* This instance's own controls, and only its own. An empty list
                   means this slot has chosen nothing and shows nothing: seeding
                   it from a shared per-plugin list is what made a control picked
                   on one Kontakt appear on every other one. */
                const auto chosen = engine.getSlotPhoneControls (inputIndex, prog, sIdx);

                for (const auto& id : chosen)
                {
                    const int idx = findParamIndex (params, id);
                    if (idx < 0) continue;              // the plugin no longer has it

                    const ParamInfo* info = nullptr;
                    for (const auto& c : params) if (c.index == idx) info = &c;
                    if (info == nullptr) continue;

                    DynamicObject::Ptr pd (new DynamicObject());
                    pd->setProperty ("id", info->id);
                    pd->setProperty ("name", info->name);
                    pd->setProperty ("value", plugin->getCachedParameterValue (idx));
                    pd->setProperty ("boolean", looksLikeSwitch (*info));
                    pd->setProperty ("steps", info->numSteps);
                    sliders.add (var (pd.get()));
                }
            }
            so->setProperty ("params", sliders);
            out.add (var (so.get()));
        }
    }

    root->setProperty ("slots", out);
    return JSON::toString (var (root.get()), true);
}

/** The parameters a user can choose from, for the picker.

    This follows the rules the desktop picker already uses (ParamPicker), rather
    than inventing a second set: two pickers that disagree about what is worth
    showing would be worse than either. In short:

      - A plugin that says which (channel, controller) each parameter stands for
        gets its OWN channel's controllers kept and the other fifteen channels'
        copies hidden. Hiding all of them, as a first cut of this did, throws
        away the ones that actually affect what you hear.
      - A plugin that does not say -- Kontakt names 2049 parameters "#000" and
        up -- gets runs of identically named parameters treated as the same
        thing, which is what those runs almost always are.
      - Searching matches the controller name too, so "CC 74" finds it. */
String RemoteServer::paramsJson (int inputIndex, int slot, int effect,
                                 const String& search, bool showSecondary) const
{
    DynamicObject::Ptr root (new DynamicObject());
    Array<var> out;
    int total = 0, shown = 0, hidden = 0, named = 0;

    const auto& setup = engine.getSetup();
    if (inputIndex >= 0 && inputIndex < (int) setup.inputs.size())
    {
        const auto& in = setup.inputs[(size_t) inputIndex];
        const int prog = in.currentProgram;

        if (auto* plugin = engine.getPlugin (inputIndex, prog, slot, effect))
        {
            const auto& params = plugin->getParameters();
            total = (int) params.size();
            const auto slotChosen = engine.getSlotPhoneControls (inputIndex, prog, slot);

            // Which entries are the "other channels" or "more of the same"?
            std::vector<bool> secondary ((size_t) total, false);
            bool hasChannels = false;
            for (const auto& info : params) if (info.midiChannel > 0) { hasChannels = true; break; }

            if (hasChannels)
            {
                const int pref = in.channel;     // 0 = omni: every channel matters
                if (pref > 0)
                    for (size_t i = 0; i < params.size(); ++i)
                        if (params[i].midiChannel > 0 && params[i].midiChannel != pref)
                            secondary[i] = true;
            }
            else
            {
                /* No channel information. A run of identically named parameters
                   is almost always the same control repeated per channel, so
                   keep the first and fold the rest away. */
                constexpr int kRunLength = 8;
                for (size_t i = 0; i < params.size(); )
                {
                    size_t j = i + 1;
                    while (j < params.size() && params[j].name == params[i].name) ++j;
                    if ((int) (j - i) >= kRunLength)
                        for (size_t k = i + 1; k < j; ++k) secondary[k] = true;
                    i = j;
                }
            }

            /* Order, not just filtering -- this is what the desktop picker does,
               and Kontakt shows why it matters. Kontakt names 2049 parameters
               "#000".."#2048" (empty automation slots) and puts them FIRST, with
               the genuinely useful ones ("Channel Volume(MSB)", "Pan(MSB)")
               after. Reading the list in order and stopping at a few hundred
               shows nothing but junk. So: parameters on this input's channel
               first, then the plugin's own named parameters, then the rest. */
            std::vector<size_t> order;
            order.reserve (params.size());
            {
                const int pref = in.channel;
                if (hasChannels)
                {
                    for (size_t i = 0; i < params.size(); ++i)
                        if (params[i].midiChannel > 0 && (pref == 0 || params[i].midiChannel == pref))
                            order.push_back (i);
                    for (size_t i = 0; i < params.size(); ++i)
                        if (params[i].midiChannel == 0 && ! RemoteServer::isPlaceholderName (params[i].name))
                            order.push_back (i);
                    for (size_t i = 0; i < params.size(); ++i)
                        if (params[i].midiChannel == 0 && RemoteServer::isPlaceholderName (params[i].name))
                            order.push_back (i);
                    for (size_t i = 0; i < params.size(); ++i)
                        if (params[i].midiChannel > 0 && pref != 0 && params[i].midiChannel != pref)
                            order.push_back (i);
                }
                else
                {
                    for (size_t i = 0; i < params.size(); ++i)
                        if (! RemoteServer::isPlaceholderName (params[i].name)) order.push_back (i);
                    for (size_t i = 0; i < params.size(); ++i)
                        if (RemoteServer::isPlaceholderName (params[i].name)) order.push_back (i);
                }
            }

            // An unnamed placeholder is never worth a slider before the named ones.
            for (size_t i = 0; i < params.size(); ++i)
                if (RemoteServer::isPlaceholderName (params[i].name)) secondary[i] = true;

            /* How many the plugin actually named. Some publish nothing useful at
               all -- Numa Player's 2145 parameters are 2048 controller proxies,
               64 "<unassigned>" and Bypass -- and a page of placeholders with no
               explanation reads as the app being broken rather than the plugin
               declining to say. */
            for (const auto& info : params)
                if (! RemoteServer::isPlaceholderName (info.name) && info.midiController < 0) ++named;

            for (size_t i : order)
            {
                const auto& info = params[i];

                if (search.isNotEmpty())
                {
                    const auto hay = info.name + " " + controllerLabel (info.midiController);
                    if (! hay.containsIgnoreCase (search)) continue;
                }

                // Something already chosen always shows, so it can be unchosen.
                // Ticked for THIS slot.
                const bool chosen = std::find (slotChosen.begin(), slotChosen.end(), info.id) != slotChosen.end();

                if (secondary[i] && ! showSecondary && ! chosen) { ++hidden; continue; }
                if (shown >= 300) { ++hidden; continue; }
                ++shown;

                DynamicObject::Ptr pd (new DynamicObject());
                pd->setProperty ("id", info.id);
                pd->setProperty ("name", info.name);
                pd->setProperty ("detail", controllerLabel (info.midiController)
                                             + (info.midiChannel > 0 ? " (ch " + String (info.midiChannel) + ")" : String()));
                pd->setProperty ("value", plugin->getCachedParameterValue (info.index));
                pd->setProperty ("boolean", looksLikeSwitch (info));
                pd->setProperty ("steps", info.numSteps);
                pd->setProperty ("chosen", chosen);
                out.add (var (pd.get()));
            }
        }
    }

    root->setProperty ("params", out);
    root->setProperty ("total", total);
    root->setProperty ("shown", shown);
    root->setProperty ("hidden", hidden);
    root->setProperty ("named", named);
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
   pages would be actively harmful: a phone showing a stale program list is worse
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

void RemoteServer::handle (StreamingSocket& sock)
{
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
    if (path.startsWith ("/api/params"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"bad token\"}"); return; }
        const int input  = path.fromFirstOccurrenceOf ("input=", false, false).getIntValue();
        const int slot   = path.fromFirstOccurrenceOf ("slot=", false, false).getIntValue();
        const int effect = path.contains ("effect=") ? path.fromFirstOccurrenceOf ("effect=", false, false).getIntValue() : -1;
        const bool all   = path.contains ("all=1");   // show the hidden copies too
        auto search = path.fromFirstOccurrenceOf ("q=", false, false).upToFirstOccurrenceOf ("&", false, false);
        search = URL::removeEscapeChars (search.replaceCharacter ('+', ' '));
        sendResponse (sock, "200 OK", "application/json", paramsJson (input, slot, effect, search, all));
        return;
    }
    if (path.startsWith ("/api/setparam"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"bad token\"}"); return; }
        const int input  = path.fromFirstOccurrenceOf ("input=", false, false).getIntValue();
        const int slot   = path.fromFirstOccurrenceOf ("slot=", false, false).getIntValue();
        const int effect = path.contains ("effect=") ? path.fromFirstOccurrenceOf ("effect=", false, false).getIntValue() : -1;
        const auto id    = URL::removeEscapeChars (path.fromFirstOccurrenceOf ("id=", false, false).upToFirstOccurrenceOf ("&", false, false));
        const float v    = path.fromFirstOccurrenceOf ("value=", false, false).getFloatValue();

        /* Touch the plugin on the message thread, like every other engine call
           here: the server runs on its own thread and the plugin connection is
           not ours to drive from it. */
        MessageManager::callAsync ([this, input, slot, effect, id, v]
        {
            const auto& setup = engine.getSetup();
            if (input < 0 || input >= (int) setup.inputs.size()) return;
            const int prog = setup.inputs[(size_t) input].currentProgram;
            if (auto* plugin = engine.getPlugin (input, prog, slot, effect))
                if (const int idx = findParamIndex (plugin->getParameters(), id); idx >= 0)
                    plugin->setParameterValue (idx, jlimit (0.0f, 1.0f, v));
        });
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (path.startsWith ("/api/favourite"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"bad token\"}"); return; }
        const int input  = path.fromFirstOccurrenceOf ("input=", false, false).getIntValue();
        const int slot   = path.fromFirstOccurrenceOf ("slot=", false, false).getIntValue();
        const auto id    = URL::removeEscapeChars (path.fromFirstOccurrenceOf ("id=", false, false).upToFirstOccurrenceOf ("&", false, false));
        const bool on    = path.contains ("on=1");

        /* Store against this slot, not this plugin type. Two Kontakts in a setup
           are two instruments: choosing a cutoff on the strings must not put the
           same control on the drums, which is what keying by plugin did.

           On the message thread, because it edits the setup the audio thread is
           reading, and every other engine call here does the same. */
        MessageManager::callAsync ([this, input, slot, id, on]
        {
            const auto& setup = engine.getSetup();
            if (input < 0 || input >= (int) setup.inputs.size()) return;
            const int prog = setup.inputs[(size_t) input].currentProgram;
            const auto& def = setup.inputs[(size_t) input].programs[(size_t) prog];
            if (slot < 0 || slot >= (int) def.slots.size()) return;

            engine.setSlotPhoneControl (input, prog, slot, id, on);
        });
        ++revision;
        sendResponse (sock, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (path.startsWith ("/api/tap"))
    {
        if (! authorised (path)) { sendResponse (sock, "403 Forbidden", "application/json", "{\"error\":\"bad token\"}"); return; }

        /* Tapped on the phone. The tap has to be timed where it happens, and it
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
