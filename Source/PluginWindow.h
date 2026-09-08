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

        // An editor that reports no size would give an unusable window; show the generic one instead.
        if (editor != nullptr && (editor->getWidth() < 10 || editor->getHeight() < 10))
        {
            delete editor;      // also detaches it from the processor
            editor = nullptr;
        }
        if (editor == nullptr)
        {
            auto* generic = new juce::GenericAudioProcessorEditor (instance);
            generic->setSize (juce::jmax (400, generic->getWidth()), juce::jmax (300, generic->getHeight()));
            editor = generic;
        }

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        setResizeLimits (200, 100, 8192, 8192);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        if (closeCallback) closeCallback();
    }

    /** Escape closes the window, in case it ends up without decorations. */
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey) { closeButtonPressed(); return true; }
        return DocumentWindow::keyPressed (key);
    }

    juce::AudioPluginInstance& instance;

private:
    std::function<void()> closeCallback;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

} // namespace perf
