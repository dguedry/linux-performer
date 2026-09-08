#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>

namespace perf
{

/** A top-level window hosting a plugin's editor (or a generic one). */
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioPluginInstance& inst, const juce::String& title, std::function<void()> onClose)
        : DocumentWindow (title, juce::Colours::darkgrey, DocumentWindow::closeButton),
          instance (inst), closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);

        juce::AudioProcessorEditor* editor = instance.hasEditor() ? instance.createEditorIfNeeded() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (instance);

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        if (closeCallback) closeCallback();
    }

    juce::AudioPluginInstance& instance;

private:
    std::function<void()> closeCallback;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

} // namespace perf
