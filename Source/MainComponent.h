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
    /** @param initialSetup  a setup file to open instead of the last one (may be empty). */
    MainComponent (Engine&, juce::PropertiesFile& settings, const juce::File& initialSetup = {});
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Saves the current setup to its file (or the autosave file). Called on quit. */
    void saveOnQuit();

    int getSelectedInput() const            { return selectedInput; }
    int getEditedProgram() const;

    void openPluginEditor (int inputIndex, int program, int slot, int effect = -1);
    void showStatus (const juce::String&);
    /** Marks the setup as changed so the periodic autosave picks it up. */
    void markDirty()                        { dirty = true; }
    /** ARA-only plugins need an ARA host; they may show an empty editor and can hang. */
    static bool looksLikeAraPlugin (const juce::PluginDescription&);
    void selectInput (int);
    void loadSetupFile (const juce::File&);

private:
    // Engine::Listener
    void setupChanged() override;
    void programChanged (int inputIndex, int program) override;
    void programContentChanged (int inputIndex, int program) override;
    void learnReceived (int inputIndex, MappingDef::Source, int number) override;
    void parameterTouched (int inputIndex, int program, int slot, int effect, int paramIndex) override;
    void instanceAboutToBeDeleted (juce::AudioPluginInstance*) override;
    void statusMessage (const juce::String&) override;

    void timerCallback() override;

    void refreshAll();
    void refreshProgramView();

    // file handling
    void newSetup();
    void openSetup();
    void saveSetup (bool forceAskForFile);
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
    bool dirty = false;
    double lastAutosaveTime = 0;
    static constexpr double autosaveIntervalMs = 60000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace perf
