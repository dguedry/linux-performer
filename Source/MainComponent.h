#pragma once

#include "Engine.h"
#include "PluginWindow.h"
#include <juce_gui_extra/juce_gui_extra.h>

namespace perf
{

class InputsPanel;
class ProgramsPanel;
class SlotsPanel;
class MappingsPanel;

class MainComponent : public juce::Component,
                      private Engine::Listener,
                      private juce::Timer
{
public:
    MainComponent (Engine&, juce::PropertiesFile& settings);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Saves the current setup to its file (or the autosave file). Called on quit. */
    void saveOnQuit();

    int getSelectedInput() const            { return selectedInput; }
    int getEditedProgram() const;

    void openPluginEditor (int inputIndex, int program, int slot);
    void showStatus (const juce::String&);
    void selectInput (int);

private:
    // Engine::Listener
    void setupChanged() override;
    void programChanged (int inputIndex, int program) override;
    void programContentChanged (int inputIndex, int program) override;
    void learnReceived (int inputIndex, MappingDef::Source, int number) override;
    void parameterTouched (int inputIndex, int program, int slot, int paramIndex) override;
    void instanceAboutToBeDeleted (juce::AudioPluginInstance*) override;
    void statusMessage (const juce::String&) override;

    void timerCallback() override;

    void refreshAll();
    void refreshProgramView();

    // file handling
    void newSetup();
    void openSetup();
    void saveSetup (bool forceAskForFile);
    void loadSetupFile (const juce::File&);
    bool writeSetupFile (const juce::File&);
    void setCurrentFile (const juce::File&);
    juce::File getAutosaveFile() const;

    void showAudioSettings();
    void showPluginManager();
    void closePluginWindowsFor (juce::AudioPluginInstance*);

    Engine& engine;
    juce::PropertiesFile& settings;
    juce::File currentFile;
    int selectedInput = 0;

    // toolbar
    juce::TextButton newBtn { "New" }, openBtn { "Open..." }, saveBtn { "Save" }, saveAsBtn { "Save As..." },
                     audioBtn { "Audio..." }, pluginsBtn { "Plugins..." }, midiRefreshBtn { "Rescan MIDI" },
                     panicBtn { "PANIC" };
    juce::ToggleButton preloadToggle { "Preload all programs" };
    juce::Label tailLabel { {}, "Tail" };
    juce::Slider tailSlider;
    juce::Label statusLabel, cpuLabel, fileLabel;

    std::unique_ptr<InputsPanel> inputsPanel;
    std::unique_ptr<ProgramsPanel> programsPanel;
    std::unique_ptr<SlotsPanel> slotsPanel;
    std::unique_ptr<MappingsPanel> mappingsPanel;

    std::vector<std::unique_ptr<PluginWindow>> pluginWindows;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::String statusText;
    double statusTime = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace perf
