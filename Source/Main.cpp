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

    void initialise (const juce::String& commandLine) override
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName = "Performer";
        opts.filenameSuffix = "settings";
       #if JUCE_LINUX || JUCE_BSD
        opts.folderName = ".config/Performer";      // JUCE would otherwise use ~/Performer
        migrateOldSettingsFolder (opts);
       #else
        opts.folderName = "Performer";
       #endif
        opts.osxLibrarySubFolder = "Application Support";
        opts.storageFormat = juce::PropertiesFile::storeAsXML;
        settings = std::make_unique<juce::PropertiesFile> (opts);

        // Optional: `Performer path/to/setup.performer.json` opens that setup instead of the last one.
        juce::File initialSetup;
        juce::ArgumentList args ("Performer", commandLine);
        for (int i = 0; i < args.size(); ++i)
        {
            auto f = args[i].resolveAsFile();
            if (f.existsAsFile()) { initialSetup = f; break; }
        }

        host   = std::make_unique<perf::PluginHost> (*settings);
        engine = std::make_unique<perf::Engine> (*host, *settings);
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine, *settings, initialSetup);
    }

    /** Early builds stored settings in ~/Performer; move them to ~/.config/Performer once. */
    static void migrateOldSettingsFolder (const juce::PropertiesFile::Options& opts)
    {
        auto newFile = opts.getDefaultFile();
        auto oldDir  = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Performer");
        auto oldFile = oldDir.getChildFile (newFile.getFileName());
        if (newFile.existsAsFile() || ! oldFile.existsAsFile())
            return;

        newFile.getParentDirectory().createDirectory();
        for (auto& name : { newFile.getFileName(), juce::String ("autosave.performer.json"), juce::String ("RecentlyCrashedPluginsList") })
        {
            auto src = oldDir.getChildFile (name);
            if (src.existsAsFile())
                src.moveFileTo (newFile.getSiblingFile (name));
        }
        if (oldDir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
            oldDir.deleteFile();
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
        MainWindow (const juce::String& name, perf::Engine& engine, juce::PropertiesFile& settings, const juce::File& initialSetup)
            : DocumentWindow (name, juce::Colour (0xff1e1f24), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            content = new perf::MainComponent (engine, settings, initialSetup);
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
