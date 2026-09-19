#pragma once

#include "ParamInfo.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

namespace perf
{

//==============================================================================
/** One control as it should appear on the phone. */
struct PhoneControl
{
    juce::String paramId;      // the plugin's own parameter ID, or its index as text
    juce::String paramName;    // what the plugin called it when the template was made
    juce::String label;        // what to show instead, or empty to use the plugin's name

    /** How to draw it. `automatic` uses the step count and the name, which is
        right for most plugins; the others are an escape hatch for plugins that
        report nothing useful about themselves. */
    enum class Widget : int { automatic = 0, fader = 1, sw = 2 };
    Widget widget = Widget::automatic;

    juce::var toVar() const;
    static PhoneControl fromVar (const juce::var&);
};

//==============================================================================
/** A named set of phone controls for one plugin slot, made to be shared.

    A slot rather than a program, because a slot is what someone else can use:
    the controls that make sense for a Kontakt library are the same wherever it
    is loaded, while a program's layout is particular to one person's set.

    Kontakt is the reason this exists. Its parameters are generic MIDI
    controllers -- "CC 3", "CC 21" -- and what they do depends entirely on the
    library loaded into it, which the host cannot see. So a template is mostly
    labels: someone who owns the library works out once that CC 21 is vibrato
    depth, and everyone else gets that knowledge.

    Templates are chosen by name, never matched automatically. Two instances of
    the same plugin are indistinguishable to the host, and a wrong guess applied
    silently would be worse than asking. */
struct PhoneTemplate
{
    juce::String name;           // "Scarbee Rhodes", chosen by whoever made it
    juce::String pluginName;     // which plugin it was made for, for display
    juce::String pluginKey;      // PluginDescription::createIdentifierString()
    juce::String author;         // optional
    juce::String notes;          // optional: what the library is, what to expect
    std::vector<PhoneControl> controls;

    bool isEmpty() const { return controls.empty(); }

    juce::var toVar() const;
    static PhoneTemplate fromVar (const juce::var&);

    /** How well this template fits a plugin's actual parameters. Nothing is
        detected automatically, but applying the Rhodes template to a drum kit
        should say so rather than quietly producing nonsense. */
    struct Fit
    {
        int matched = 0;         // controls whose ID is present
        int renamed = 0;         // present, but the plugin now calls it something else
        int missing = 0;         // the ID is not there at all
        juce::String summary() const;
    };
    Fit checkAgainst (const ParamInfoList&) const;
};

//==============================================================================
/** The templates on this machine, as a JSON file beside the settings.

    One file rather than one per template: it is a handful of kilobytes, and a
    single file is easier to back up. Import and export move one template at a
    time, which is what people will share. */
class PhoneTemplates
{
public:
    explicit PhoneTemplates (const juce::File& storage);

    std::vector<PhoneTemplate> all() const;
    /** Templates made for this plugin, which is what to offer for a slot. */
    std::vector<PhoneTemplate> forPlugin (const juce::PluginDescription&) const;
    bool contains (const juce::String& name) const;

    /** Adds or replaces by name. */
    void put (const PhoneTemplate&);
    void remove (const juce::String& name);

    juce::Result save() const;
    juce::Result load();

    /** One template as a file someone can send. */
    static juce::Result exportToFile (const PhoneTemplate&, const juce::File&);
    static juce::Result importFromFile (const juce::File&, PhoneTemplate& out);

private:
    juce::File file;
    std::vector<PhoneTemplate> templates;
};

} // namespace perf
