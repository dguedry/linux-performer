#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginHost.h"
#include "Engine.h"
#include "MainComponent.h"

class PerformerApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return "Performer"; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override             { return false; }

    void initialise (const juce::String&) override
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName = "Performer";
        opts.filenameSuffix = "settings";
        opts.folderName = "Performer";
        opts.osxLibrarySubFolder = "Application Support";
        opts.storageFormat = juce::PropertiesFile::storeAsXML;
        settings = std::make_unique<juce::PropertiesFile> (opts);

        host   = std::make_unique<perf::PluginHost> (*settings);
        engine = std::make_unique<perf::Engine> (*host, *settings);
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine, *settings);
    }

    void shutdown() override
    {
        mainWindow.reset();     // closes plugin editors and autosaves
        engine.reset();
        host.reset();
        if (settings != nullptr) settings->saveIfNeeded();
        settings.reset();
    }

    void systemRequestedQuit() override { quit(); }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, perf::Engine& engine, juce::PropertiesFile& settings)
            : DocumentWindow (name, juce::Colour (0xff1e1f24), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            content = new perf::MainComponent (engine, settings);
            setContentOwned (content, true);
            setResizable (true, false);
            setResizeLimits (960, 600, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        ~MainWindow() override
        {
            if (content != nullptr) content->saveOnQuit();
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        perf::MainComponent* content = nullptr;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<perf::PluginHost> host;
    std::unique_ptr<perf::Engine> engine;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (PerformerApplication)
