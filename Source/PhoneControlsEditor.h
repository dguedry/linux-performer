#pragma once

#include "Engine.h"
#include "PhoneTemplate.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace perf
{

/** Editing what a slot shows on the phone, and sharing it as a template.

    Authoring belongs here rather than on the phone: naming a dozen controls for
    a Kontakt library means searching thousands of parameters and comparing them
    against the plugin's own window, which is desk work. The phone is where you
    check the result.

    Templates are the point. Kontakt's parameters are generic MIDI controllers,
    so "CC 3" says nothing about what the loaded library does with it. Someone
    who owns the library works that out once and everyone else gets the labels. */
class PhoneControlsEditor : public juce::Component
{
public:
    PhoneControlsEditor (Engine&, PhoneTemplates&, int inputIndex, int program, int slot);
    ~PhoneControlsEditor() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    /** Opens it in its own window. */
    static void show (Engine&, PhoneTemplates&, int inputIndex, int program, int slot,
                      juce::Colour background);

private:
    struct Row;
    class RowList;

    void refresh();
    void commit();                       // push the edited list back to the engine
    void addControls();                  // the picker
    void applyTemplate();
    void saveAsTemplate();
    void exportTemplate();
    void importTemplate();
    void move (int from, int to);

    juce::String pluginName() const;
    ParamInfoList pluginParams() const;

    Engine& engine;
    PhoneTemplates& templates;
    const int inputIndex, program, slot;

    std::vector<PhoneControl> controls;

    juce::Label title;
    juce::TextButton addBtn { "Add controls..." };
    juce::TextButton templateBtn { "Templates..." };
    juce::TextButton closeBtn { "Done" };
    std::unique_ptr<RowList> list;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PhoneControlsEditor)
};

} // namespace perf
