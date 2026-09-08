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
| **Slot** | One plugin instance inside a program, with enable, gain, transpose, key range and output-channel settings. |
| **Mapping** | A CC / pitch-bend / aftertouch message → one plugin parameter, scaled between min and max. Optionally also passed through to the plugin. |
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
./build/Performer_artefacts/Release/Performer
```

## Using it

1. **Audio...** – pick the output device. Under PipeWire both the JACK and
   ALSA device types work; JACK gives the lowest latency.
2. **Plugins...** – *Options → Scan for new or updated VST3 plugins* (and LV2).
   Scanning happens in-process; a plugin that crashes the scan is
   blacklisted automatically on the next start.
3. Select an input on the left, give it a **MIDI device** and **channel**.
4. Click a program in the middle list (this also activates it), name it, and
   **Add plugin...**. Use **Edit GUI** to open the plugin's own editor.
5. To map a controller: open **MIDI mappings**, pick a slot and parameter
   (or move a knob in the plugin GUI and press **Use touched parameter**),
   press **Learn MIDI**, move the controller, then **Add**.
6. **Save** the setup. It is reloaded automatically next start; the setup is
   also autosaved on quit.

Plugin state is captured into the setup file whenever a program is unloaded
or the setup is saved, so tweaks made in the plugin GUI persist.

## Layout of the code

- `Source/Model.*` – the document (`Setup → InputDef → ProgramDef → SlotDef / MappingDef`) and JSON persistence.
- `Source/PluginHost.*` – plugin formats, known-plugin list, instantiation.
- `Source/Engine.*` – audio/MIDI engine: device management, program loading and switching, MIDI routing, mappings, learn.
- `Source/MainComponent.*` – the UI (inputs, programs, slots, mappings panels).
- `Source/PluginWindow.h` – window hosting a plugin editor.
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
