# Performer

A live-performance plugin host for Linux. Point one or more MIDI sources
(e.g. the upper and lower manuals of a keyboard rig) at programs, and let each
program load one or more instrument plugins. Program Change messages switch
programs live; MIDI controllers can be mapped to any plugin parameter.

Built with [JUCE](https://juce.com) 8. Hosts **VST3**, **LV2** and **LADSPA**
plugins natively. Windows VST3s bridged with [yabridge](https://github.com/robbert-vdh/yabridge)
appear as normal VST3s and work too.

## Concepts

| Term | Meaning |
|------|---------|
| **Input** | A MIDI source: a MIDI device plus an optional channel filter. Each input has its own current program. Two inputs may share one device (e.g. channel 1 = Upper, channel 2 = Lower). |
| **Program** | One of 128 per input, selected by MIDI Program Change or by clicking. Holds plugin slots and MIDI mappings. |
| **Slot** | One instrument inside a program, with enable, gain, transpose, key range and output-channel settings, plus its own insert-effect chain. |
| **Effect chain** | Ordered effects with bypass. Each slot has one (instrument → effects → gain); the program has one more applied to the sum of all slots. Effects receive the same MIDI as their slot. |
| **Mapping** | A CC / pitch-bend / aftertouch message → one parameter on any instrument or effect, scaled between min and max. Optionally also passed through to the plugin. |
| **Setup** | The whole document (inputs, programs, plugin states). Saved as `*.performer.json`. |

Program switching keeps the outgoing program rendering for a configurable
release tail so held notes fade naturally. With **Preload all programs** on,
every used program stays loaded and changes are instant (at the cost of RAM).

## Build

Requirements (Ubuntu 24.04 names): `build-essential cmake ninja-build
libasound2-dev libjack-jackd2-dev libx11-dev libxrandr-dev libxinerama-dev
libxcursor-dev libfreetype-dev libfontconfig1-dev libgl1-mesa-dev ladspa-sdk`.

```sh
git clone --depth 1 --branch 8.0.15 https://github.com/juce-framework/JUCE.git external/JUCE   # if not present
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/Performer_artefacts/Release/Performer            # opens the last setup
./build/Performer_artefacts/Release/Performer my.performer.json
cmake --install build --prefix ~/.local                  # optional: binaries, launcher entry and icon
```

## Using it

1. **Audio...** – pick the output device and the live block size. Performer
   defaults to the JACK device type at 48 kHz, which under PipeWire is a
   native graph client (no resampling, no extra buffering). See *Low latency*
   below.
2. **Plugins...** – press **Scan VST3** (or LV2 / LADSPA) to look in the
   standard folders, or **Scan folder...** to add one of your own. Progress is
   shown at the bottom of the window and the scan keeps running if you close
   it. Each plugin file is probed in a separate `performer-plugin-host` process;
   Wine-bridged plugins are probed one at a time and retried once if they stall,
   so a crashing or hanging plugin can't take the scan down. A plugin shown in
   red as "Deactivated" hung twice while being probed; select it and press
   **Remove selected** to give it another chance, or **Clear list** to start over.
3. Select an input on the left, give it a **MIDI device** and **channel**.
4. Click a program in the middle list (this also activates it), name it, and
   **Add instrument...**. Use **Edit GUI** to open the plugin's own editor.
   **Add effect...** under a slot inserts an effect after that instrument;
   the **Program effects** section at the bottom processes the mix of all slots.
   Toggle an effect off to bypass it, use ^ / v to reorder.
5. To map a controller: open **MIDI mappings**, pick a target plugin and parameter
   (or move a knob in the plugin GUI and press **Use touched parameter**),
   press **Learn MIDI**, move the controller, then **Add**.
   **Suggest...** proposes a whole set at once: from the plugin's saved template
   if you made one with **Save template**, otherwise by matching parameter names
   to the General MIDI Level 2 sound controllers (CC 74 cutoff, 71 resonance,
   73/75/70/72 attack/decay/sustain/release, 76/77 LFO rate/depth, 7 volume,
   10 pan, 91 reverb, 93 chorus, 94 detune, 5 portamento, 12 drive, 13 delay mix).
   Untick what you don't want and press **Add selected**. Templates live in
   `~/.config/Performer/mapping-templates.json`.
6. **Keyboard** (toolbar) opens an on-screen keyboard with velocity, sustain,
   mod wheel, pitch bend and a free CC number/value pair. It plays into the
   selected input on that input's channel, through the same path as real MIDI,
   so it works for trying sounds, program key zones and MIDI learn without a
   controller. Click the keys, or focus the keyboard and use the computer
   keyboard (A S D F G H J K, W E T Y U for sharps, Z / X to shift octave).
7. **Save** the setup. It is reloaded automatically next start; the setup is
   also autosaved on quit.

Plugin state is captured into the setup file whenever a program is unloaded
or the setup is saved, so tweaks made in the plugin GUI persist. Changes are
also autosaved about once a minute to `~/.config/Performer/autosave.performer.json`;
if that file is newer than the setup you open, the status bar says so.

**ARA-only plugins** (e.g. "ACE Bridge ARA") need an ARA host such as Reaper.
Outside one their editor is empty, and bridged through yabridge the plugin can
stop answering. Since each plugin has its own process this only stalls that
plugin; Performer warns when such a plugin is added or opened anyway. Use the
plugin's non-ARA version instead.

## Windows plugins via yabridge and nilinux

Windows VST3s bridged with yabridge appear as normal plugins. If the Wine prefix
is managed by [nilinux](https://github.com/dguedry/nilinux) (Native Access on
Linux), that tool requires DAWs to start yabridge with *its* wine: the host's
wine would run its own prefix update and corrupt the prefix. Performer follows
that convention for every helper it starts: `~/.local/bin` is put first on the
helper's `PATH` when nilinux's `wine` shim is there, and variables from
`~/.config/environment.d/*.conf` (`WINELOADER`, `WINEFSYNC`) are applied when
the session lacks them, so it does not matter whether Performer was started
from a terminal or a desktop launcher.

While Native Access installs or updates a product, its Windows plugin file is
replaced and the yabridge bundle briefly points at nothing. Performer reports
that as "yabridge link points to a missing file" instead of a blacklist; when
nilinux has finished (it runs `yabridgectl sync` after Native Access exits),
scan again and the plugin comes back.

## Process model

Every plugin runs in its own `performer-plugin-host` process. Performer talks to
each one over a shared-memory block (audio, MIDI and parameter changes, one
block at a time, with two semaphores) plus a socket for everything that is not
real-time: loading, state, parameter lists, editor windows and notifications.

- A plugin that crashes takes only its own process down. The slot goes silent,
  the UI marks it red, and its **Reload** button starts a fresh process with
  the saved state.
- A plugin that hangs (a stuck editor, a bridge waiting on Wine) can't freeze
  Performer: requests to it time out, and the audio thread waits for each
  process only until 85 % of the block period has passed. Late blocks are
  counted in the status bar as "late N".
- Plugin editors are windows of the host process, so they survive independently
  of Performer's UI. Instruments of one program render in parallel, one process
  per core; effect chains run in order.
- Plugin scanning also happens in helper processes (`performer-plugin-host --scan`),
  so a crashing plugin can't kill the scan.

The helper is looked for next to the `Performer` executable, then in the build
tree, then via `$PERFORMER_PLUGIN_HOST`.

## Layout of the code

- `Source/Model.*` – the document (`Setup → InputDef → ProgramDef → SlotDef / MappingDef`) and JSON persistence.
- `Source/PluginHost.*` – plugin formats, known-plugin list, out-of-process probing, mapping templates.
- `Source/PluginScanner.*` – app-owned background scan; `Source/PluginManagerComponent.h` – the Plugins window.
- `Source/MappingSuggestions.*` – per-plugin mapping templates and name-based CC suggestions.
- `Source/Engine.*` – audio/MIDI engine: device management, program loading and switching, MIDI routing, mappings, learn.
- `Source/RemotePlugin.*` – host-side proxy for one plugin process (spawn, control channel, real-time block exchange).
- `Source/Ipc/Protocol.*` – shared-memory layout and framing shared by both executables.
- `Source/PluginHostProcess/PluginHostMain.cpp` – the `performer-plugin-host` executable.
- `Source/MainComponent.*` – the UI (inputs, programs, slots, mappings panels).
- `Source/KeyboardPanel.h` – on-screen keyboard and test controllers.
- `Source/Main.cpp` – application entry.

## Tests

```sh
ctest --test-dir build
# Optionally exercise a specific VST3 (e.g. a yabridge-bridged plugin) through the engine:
PERFORMER_TEST_VST3="$HOME/.vst3/yabridge/Kontakt 8.vst3" ./build/EngineTest_artefacts/Release/EngineTest
```

All plugin buses are kept active and rendered into a full-size buffer; only the
main bus is mixed. Disabling aux buses makes JUCE pass null channel pointers,
which crashes plugins that write to them regardless (Kontakt via yabridge).

Settings live in `~/.config/Performer/`.

## Low latency (live use)

Performer is meant to be played live, so it goes for the smallest safe block:

* It loads PipeWire's JACK client library itself (`pipewire-jack`, package
  `pipewire-jack` on Debian/Ubuntu), so it does not need to be started with
  `pw-jack`, and it defaults to the **JACK** device type at 48 kHz. That makes
  Performer a direct node in the PipeWire graph; the ALSA type goes through
  PipeWire's ALSA emulation with its own buffering and resampling.
* **Audio...** has a *Live latency* section: the block size (quantum) to ask
  for (default 128 samples = 2.7 ms at 48 kHz; 64 if the machine is quiet) and
  *Take over the PipeWire clock while Performer runs*. With the takeover on,
  Performer forces the PipeWire graph to that quantum at start
  (`clock.force-quantum`) and restores the previous value when it quits, so
  the desktop's `min-quantum`/`default.clock.quantum` settings do not get in
  the way. The status bar shows the block size actually delivered and the
  resulting latency.
* **Another application can hold the clock.** Bitwig Studio, for example,
  forces `clock.force-quantum` to its own block size every few seconds while
  it runs; Performer then reports *clock held by another app* in the status
  bar. Close that application or set its block size to Performer's.
* **Realtime scheduling.** Small blocks only stay glitch-free when the audio
  thread can run at realtime priority. Check `ulimit -r`: if it prints 0, add
  yourself to the `pipewire` group (`sudo usermod -aG pipewire $USER`, then
  log out and back in) or otherwise grant `rtprio`/`memlock` in
  `/etc/security/limits.d/`. The Audio dialog reports whether realtime
  scheduling is available.
* Plugins load in parallel (four at a time by default; `parallelLoads` in
  `~/.config/Performer/Performer.settings` changes it), so *Preload all
  programs* fills a set quickly. Wine-bridged plugins load in parallel too
  (`parallelBridgedLoads`, default the same); two Kontakts starting together on
  a cold prefix both come up in the time one takes. If a machine stalls on
  concurrent Wine start-ups, `bridgedColdStartSolo=1` starts the first bridged
  plugin alone when no wineserver is running yet.
* Every plugin runs in its own process; a plugin that does not finish its
  block in time is silenced for that block and counted as *late* in the
  status bar, so one slow plugin cannot stall the whole rig.
