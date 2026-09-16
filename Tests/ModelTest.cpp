// Round-trip test for the Performer setup file format.
#include "Model.h"
#include "ProgramMap.h"
#include "MappingSuggestions.h"
#include <juce_events/juce_events.h>
#include <cstdio>

using namespace perf;

static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    Setup s = Setup::makeDefault();
    s.preloadAllPrograms = true;
    s.releaseTailSeconds = 2.5;

    auto& upper = s.inputs[0];
    upper.midiDeviceIdentifier = "alsa:24:0";
    upper.midiDeviceName = "Nord Stage";
    upper.channel = 1;
    upper.currentProgram = 5;

    upper.programChangeChannel = 16;          // a workstation's global channel
    auto& prog = upper.programs[5];
    prog.name = "Rhodes + Pad";

    SlotDef slot;
    slot.plugin.name = "Test Synth";
    slot.plugin.pluginFormatName = "VST3";
    slot.plugin.fileOrIdentifier = "/usr/lib/vst3/Test.vst3";
    slot.plugin.uniqueId = 0x1234;
    slot.state.append ("\x00\x01\x02\xff state", 9);
    slot.gainDb = -6.5f;
    slot.transpose = -12;
    slot.lowKey = 36; slot.highKey = 72;
    slot.outChannel = 3;
    slot.enabled = false;
    slot.lowVelocity = 40; slot.highVelocity = 100;
    slot.velocityCurve = 0.5f;
    slot.pan = -0.3f;
    prog.slots.push_back (slot);

    MappingDef m;
    m.source = MappingDef::Source::CC; m.number = 74;
    m.slot = 0; m.paramId = "cutoff"; m.paramName = "Cutoff";
    m.minValue = 0.2f; m.maxValue = 0.9f; m.passThrough = true;
    prog.mappings.push_back (m);
    MappingDef pb; pb.source = MappingDef::Source::PitchBend; pb.paramId = "7";
    prog.mappings.push_back (pb);

    EffectDef fx;
    fx.plugin.name = "Test Reverb"; fx.plugin.pluginFormatName = "LV2"; fx.plugin.fileOrIdentifier = "urn:test:reverb";
    fx.bypassed = true;
    fx.state.append ("fx", 2);
    prog.slots[0].effects.push_back (fx);
    EffectDef progFx = fx; progFx.plugin.name = "Test Limiter"; progFx.bypassed = false;
    prog.effects.push_back (progFx);
    MappingDef fxMap; fxMap.number = 11; fxMap.slot = -1; fxMap.effect = 0; fxMap.paramId = "threshold";
    prog.mappings.push_back (fxMap);

    // Name-only program (no slots) must survive too.
    upper.programs[9].name = "Empty but named";

    juce::TemporaryFile tmp (".performer.json");
    CHECK (s.saveToFile (tmp.getFile()).wasOk());

    Setup loaded;
    auto r = Setup::loadFromFile (tmp.getFile(), loaded);
    CHECK (r.wasOk());
    if (r.failed()) std::printf ("  %s\n", r.getErrorMessage().toRawUTF8());

    CHECK (loaded.preloadAllPrograms == true);
    CHECK (std::abs (loaded.releaseTailSeconds - 2.5) < 1e-9);
    CHECK (loaded.inputs.size() == 2);
    CHECK (loaded.inputs[1].name == "Lower");
    CHECK (loaded.inputs[1].channel == 2);
    CHECK (loaded.inputs[0].programChangeChannel == 16);                       // a workstation's global channel
    CHECK (loaded.inputs[1].programChangeChannel == InputDef::pcChannelSameAsNotes);   // the default

    auto& u = loaded.inputs[0];
    CHECK (u.name == "Upper");
    CHECK (u.midiDeviceIdentifier == "alsa:24:0");
    CHECK (u.midiDeviceName == "Nord Stage");
    CHECK (u.currentProgram == 5);
    CHECK (u.programs.size() == 128);
    CHECK (u.programs[9].name == "Empty but named");
    CHECK (u.programs[4].isEmpty() && u.programs[4].name.isEmpty());

    auto& p = u.programs[5];
    CHECK (p.name == "Rhodes + Pad");
    CHECK (p.slots.size() == 1);
    if (p.slots.size() == 1)
    {
        auto& ls = p.slots[0];
        CHECK (ls.plugin.name == "Test Synth");
        CHECK (ls.plugin.pluginFormatName == "VST3");
        CHECK (ls.plugin.fileOrIdentifier == "/usr/lib/vst3/Test.vst3");
        CHECK (ls.plugin.uniqueId == 0x1234);
        CHECK (ls.state == slot.state);
        CHECK (std::abs (ls.gainDb - (-6.5f)) < 1e-6f);
        CHECK (ls.transpose == -12);
        CHECK (ls.lowKey == 36 && ls.highKey == 72);
        CHECK (ls.outChannel == 3);
        CHECK (ls.enabled == false);
        CHECK (ls.lowVelocity == 40 && ls.highVelocity == 100);
        CHECK (std::abs (ls.velocityCurve - 0.5f) < 1e-6f);
        CHECK (std::abs (ls.pan - (-0.3f)) < 1e-6f);
    }
    CHECK (p.slots.size() == 1 && p.slots[0].effects.size() == 1);
    if (p.slots.size() == 1 && p.slots[0].effects.size() == 1)
    {
        auto& e = p.slots[0].effects[0];
        CHECK (e.plugin.name == "Test Reverb" && e.plugin.pluginFormatName == "LV2");
        CHECK (e.bypassed == true);
        CHECK (e.state == fx.state);
    }
    CHECK (p.effects.size() == 1);
    if (p.effects.size() == 1)
    {
        CHECK (p.effects[0].plugin.name == "Test Limiter");
        CHECK (p.effects[0].bypassed == false);
    }
    CHECK (p.mappings.size() == 3);
    if (p.mappings.size() == 3)
    {
        CHECK (p.mappings[0].effect == -1);          // default target: the instrument
        CHECK (p.mappings[2].slot == -1 && p.mappings[2].effect == 0 && p.mappings[2].paramId == "threshold");
        auto& lm = p.mappings[0];
        CHECK (lm.source == MappingDef::Source::CC && lm.number == 74);
        CHECK (lm.slot == 0 && lm.paramId == "cutoff" && lm.paramName == "Cutoff");
        CHECK (std::abs (lm.minValue - 0.2f) < 1e-6f && std::abs (lm.maxValue - 0.9f) < 1e-6f);
        CHECK (lm.passThrough == true);
        CHECK (p.mappings[1].source == MappingDef::Source::PitchBend);
        CHECK (p.mappings[1].sourceDescription() == "Pitch Bend");
    }

    // Garbage file is rejected cleanly.
    juce::TemporaryFile bad (".json");
    bad.getFile().replaceWithText ("{ \"format\": \"something-else\" }");
    Setup ignored;
    CHECK (Setup::loadFromFile (bad.getFile(), ignored).failed());
    CHECK (Setup::loadFromFile (juce::File ("/nonexistent/x.json"), ignored).failed());

    // Mapping templates: store, reload, remove.
    {
        juce::TemporaryFile tf (".json");
        MappingTemplates t (tf.getFile());
        CHECK (t.size() == 0);
        juce::PluginDescription d; d.name = "Synth"; d.pluginFormatName = "VST3"; d.fileOrIdentifier = "/x/Synth.vst3"; d.uniqueId = 42;
        MappingDef m1; m1.number = 74; m1.paramId = "cut"; m1.slot = 3; m1.effect = 2; m1.minValue = 0.2f;
        MappingDef m2; m2.source = MappingDef::Source::PitchBend; m2.paramId = "bend";
        t.set (d, { m1, m2 });
        CHECK (t.has (d) && t.size() == 1);
        MappingTemplates t2 (tf.getFile());
        CHECK (t2.has (d));
        auto got = t2.get (d);
        CHECK (got.size() == 2);
        if (got.size() == 2)
        {
            CHECK (got[0].number == 74 && got[0].paramId == "cut" && std::abs (got[0].minValue - 0.2f) < 1e-6f);
            CHECK (got[0].slot == 0 && got[0].effect == -1);      // targets are stripped
            CHECK (got[1].source == MappingDef::Source::PitchBend && got[1].paramId == "bend");
        }
        juce::PluginDescription other = d; other.uniqueId = 43;
        CHECK (! t2.has (other));
        t2.remove (d);
        CHECK (! t2.has (d));
        MappingTemplates t3 (tf.getFile());
        CHECK (! t3.has (d) && t3.size() == 0);
    }

    // --- printable program map -------------------------------------------------------
    {
        Setup pm = Setup::makeDefault();
        pm.inputs[0].name = "Upper"; pm.inputs[0].midiDeviceName = "Keystation"; pm.inputs[0].channel = 1;
        auto& p7 = pm.inputs[0].programs[7];
        p7.name = "Rhodes & Strings";
        SlotDef a; a.plugin.name = "Kontakt 8"; p7.slots.push_back (a);
        SlotDef b; b.plugin.name = "Calf Monosynth"; p7.slots.push_back (b);
        auto& p9 = pm.inputs[0].programs[9];
        p9.name = "Organ <loud & proud>";                       // must be HTML-escaped
        SlotDef c; c.plugin.name = "Hammond B-3X"; p9.slots.push_back (c);

        const auto html = ProgramMap::toHtml (pm, "My Rig");
        CHECK (html.startsWith ("<!DOCTYPE html>"));
        CHECK (html.contains ("007") && html.contains ("Rhodes &amp; Strings"));
        CHECK (html.contains ("Kontakt 8 + Calf Monosynth"));
        CHECK (html.contains ("Organ &lt;loud &amp; proud&gt;"));      // escaped, not raw
        CHECK (! html.contains ("Organ <loud"));
        CHECK (html.contains ("Keystation") && html.contains ("channel 1"));
        CHECK (html.contains ("009"));
        CHECK (! html.contains ("<td class=\"n\">001</td>"));        // empty programs are omitted
        CHECK (html.contains ("No programs yet."));                    // the second, empty input
        std::printf ("     program map: %d bytes\n", html.length());

        // PERFORMER_MAP_FROM=setup.json PERFORMER_MAP_TO=out.html renders a real setup,
        // for eyeballing the printed sheet.
        const auto from = juce::SystemStats::getEnvironmentVariable ("PERFORMER_MAP_FROM", {});
        const auto to   = juce::SystemStats::getEnvironmentVariable ("PERFORMER_MAP_TO", {});
        if (from.isNotEmpty() && to.isNotEmpty())
        {
            Setup real;
            if (Setup::loadFromFile (juce::File (from), real).wasOk())
            {
                juce::File (to).replaceWithText (ProgramMap::toHtml (real, juce::File (from).getFileNameWithoutExtension()));
                std::printf ("     rendered %s -> %s\n", from.toRawUTF8(), to.toRawUTF8());
            }
            else std::printf ("     could not load %s\n", from.toRawUTF8());
        }
    }

    std::printf (failures == 0 ? "ModelTest: all checks passed\n" : "ModelTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
