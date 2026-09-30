#pragma once

#include "Engine.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace perf
{

/**
    An on-screen keyboard and a few controllers for testing sounds and mappings
    without a MIDI controller. Everything it sends goes through Engine::injectMidi
    into one input, on that input's channel, so program changes, key zones,
    mappings and learn all behave exactly as they would with real hardware.
*/
class KeyboardPanel : public juce::Component,
                      private juce::MidiKeyboardState::Listener
{
public:
    explicit KeyboardPanel (Engine& e)
        : engine (e), keyboard (state, juce::MidiKeyboardComponent::horizontalKeyboard)
    {
        state.addListener (this);

        addAndMakeVisible (keyboard);
        keyboard.setAvailableRange (24, 108);
        keyboard.setLowestVisibleKey (36);
        keyboard.setKeyWidth (22.0f);
        keyboard.setOctaveForMiddleC (4);
        keyboard.setVelocity (0.8f, false);
        keyboard.setWantsKeyboardFocus (true);

        addAndMakeVisible (targetLabel);
        targetLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        targetLabel.setColour (juce::Label::textColourId, juce::Colour (0xff4f9dff));

        addAndMakeVisible (hint);
        hint.setFont (juce::FontOptions (11.0f));
        hint.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0ab));
        hint.setText ("Click keys, or focus the keyboard and play A S D F G H J K (W E T Y U for sharps), Z / X to change octave.", juce::dontSendNotification);

        /* A LinearBar with a value near its minimum draws as an almost empty
           box, which is why these read as dead text fields rather than
           controls. Vertical wheels with a visible thumb and a number beside
           them look like what they are, and pitch and mod are vertical on
           every keyboard ever made. */
        auto setupWheel = [this] (juce::Slider& s, double min, double max, double step,
                                  const juce::String& tip)
        {
            s.setRange (min, max, step);
            s.setSliderStyle (juce::Slider::LinearVertical);
            s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 46, 16);
            s.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff15161c));
            s.setColour (juce::Slider::trackColourId,      juce::Colour (0xff4f9dff));
            s.setColour (juce::Slider::thumbColourId,      juce::Colour (0xffd7dbe2));
            s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            s.setTooltip (tip);
        };

        auto setupBar = [] (juce::Slider& s, double min, double max, double step,
                            const juce::String& tip)
        {
            s.setRange (min, max, step);
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
            s.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff15161c));
            s.setColour (juce::Slider::trackColourId,      juce::Colour (0xff4f9dff));
            s.setColour (juce::Slider::thumbColourId,      juce::Colour (0xffd7dbe2));
            s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            s.setTooltip (tip);
        };

        addAndMakeVisible (velocityLabel);
        addAndMakeVisible (velocity);
        setupBar (velocity, 1, 127, 1, "How hard the on-screen keys are struck");
        velocity.setValue (100, juce::dontSendNotification);
        velocity.onValueChange = [this] { keyboard.setVelocity ((float) (velocity.getValue() / 127.0), false); };

        addAndMakeVisible (sustain);
        sustain.setClickingTogglesState (true);
        sustain.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2b7a3f));
        sustain.setTooltip ("Sustain pedal (CC 64)");
        sustain.onClick = [this] { send (juce::MidiMessage::controllerEvent (channel(), 64, sustain.getToggleState() ? 127 : 0)); };

        addAndMakeVisible (modLabel);
        addAndMakeVisible (modWheel);
        setupWheel (modWheel, 0, 127, 1, "Mod wheel (CC 1). Stays where you leave it, like the real thing.");
        modWheel.onValueChange = [this] { send (juce::MidiMessage::controllerEvent (channel(), 1, (int) modWheel.getValue())); };

        addAndMakeVisible (bendLabel);
        addAndMakeVisible (pitchBend);
        setupWheel (pitchBend, -8192, 8191, 1, "Pitch bend (springs back to centre when released)");
        /* Centred, because that is where a bend wheel rests. It is bipolar, so
           the thumb belongs in the middle of its travel, not at the bottom. */
        pitchBend.setValue (0, juce::dontSendNotification);
        pitchBend.setDoubleClickReturnValue (true, 0);
        pitchBend.onValueChange = [this] { send (juce::MidiMessage::pitchWheel (channel(), (int) pitchBend.getValue() + 8192)); };
        /* Back to centre when let go. onDragEnd covers a normal drag; the mouse
           being released off the slider, or the value being nudged with the
           keyboard, would otherwise leave the pitch hanging. */
        pitchBend.onDragEnd = [this] { pitchBend.setValue (0); };

        addAndMakeVisible (ccLabel);
        addAndMakeVisible (ccNumber);
        ccNumber.setRange (0, 127, 1);
        ccNumber.setSliderStyle (juce::Slider::IncDecButtons);
        /* Say how wide the number is, or JUCE gives the text box its default
           width and squeezes the +/- buttons into whatever is left -- which is
           how the + ended up half drawn. */
        ccNumber.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 52, 22);
        ccNumber.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
        ccNumber.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff3a3d47));
        ccNumber.setValue (74, juce::dontSendNotification);
        ccNumber.setTooltip ("Which controller to send. Set it, then use Learn MIDI on a plugin parameter.");
        addAndMakeVisible (ccValue);
        setupBar (ccValue, 0, 127, 1, "Value to send for that controller");
        ccValue.onValueChange = [this] { send (juce::MidiMessage::controllerEvent (channel(), (int) ccNumber.getValue(), (int) ccValue.getValue())); };

        for (auto* l : { &velocityLabel, &ccLabel })
        {
            l->setFont (juce::FontOptions (12.0f));
            l->setColour (juce::Label::textColourId, juce::Colour (0xff9aa0ab));
            l->setJustificationType (juce::Justification::centredRight);
        }
        for (auto* l : { &bendLabel, &modLabel })
        {
            l->setFont (juce::FontOptions (11.0f));
            l->setColour (juce::Label::textColourId, juce::Colour (0xff9aa0ab));
            l->setJustificationType (juce::Justification::centred);
        }
    }

    ~KeyboardPanel() override
    {
        releaseAll();
        state.removeListener (this);
    }

    /** Which input receives the notes. Held notes are released when this changes. */
    void setTargetInput (int inputIndex)
    {
        if (inputIndex == target) { refreshLabel(); return; }
        releaseAll();
        target = inputIndex;
        refreshLabel();
    }

    void refreshLabel()
    {
        const auto& inputs = engine.getSetup().inputs;
        if (target >= 0 && target < (int) inputs.size())
            targetLabel.setText ("Playing into: " + inputs[(size_t) target].name + "  (ch " + juce::String (channel()) + ")", juce::dontSendNotification);
        else
            targetLabel.setText ("No input selected", juce::dontSendNotification);
    }

    /** All notes off + reset controllers on the target. */
    void releaseAll()
    {
        state.allNotesOff (0);
        if (sustain.getToggleState()) { sustain.setToggleState (false, juce::dontSendNotification); send (juce::MidiMessage::controllerEvent (channel(), 64, 0)); }
    }

    /* Tall enough for upright wheels with their readouts underneath. */
    static constexpr int preferredHeight = 150;

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 4);
        auto top = r.removeFromTop (22);
        targetLabel.setBounds (top.removeFromLeft (260));
        hint.setBounds (top);
        r.removeFromTop (4);

        /* The two wheels sit together on the left, upright and full height,
           the way they do on a keyboard -- bend nearest the keys. Velocity,
           sustain and the CC sender stack beside them, because those are set
           once and left alone rather than played. */
        auto wheels = r.removeFromLeft (128);
        r.removeFromLeft (10);

        auto labels = wheels.removeFromBottom (14);
        bendLabel.setBounds (labels.removeFromLeft (60));
        labels.removeFromLeft (8);
        modLabel.setBounds (labels);

        pitchBend.setBounds (wheels.removeFromLeft (60));
        wheels.removeFromLeft (8);
        modWheel.setBounds (wheels);

        auto controls = r.removeFromLeft (300);
        r.removeFromLeft (10);
        keyboard.setBounds (r);

        auto row = controls.removeFromTop (24);
        velocityLabel.setBounds (row.removeFromLeft (52)); row.removeFromLeft (4);
        velocity.setBounds (row);
        controls.removeFromTop (6);

        row = controls.removeFromTop (24);
        sustain.setBounds (row.removeFromLeft (120));
        controls.removeFromTop (6);

        row = controls.removeFromTop (24);
        ccLabel.setBounds (row.removeFromLeft (52)); row.removeFromLeft (4);
        ccNumber.setBounds (row.removeFromLeft (104)); row.removeFromLeft (8);
        ccValue.setBounds (row);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff26282f));
    }

private:
    int channel() const
    {
        const auto& inputs = engine.getSetup().inputs;
        if (target >= 0 && target < (int) inputs.size() && inputs[(size_t) target].channel > 0)
            return inputs[(size_t) target].channel;
        return 1;
    }

    void send (const juce::MidiMessage& m)
    {
        if (target >= 0) engine.injectMidi (target, m);
    }

    // MidiKeyboardState::Listener (message thread)
    void handleNoteOn (juce::MidiKeyboardState*, int, int note, float vel) override
    {
        send (juce::MidiMessage::noteOn (channel(), note, vel));
    }
    void handleNoteOff (juce::MidiKeyboardState*, int, int note, float) override
    {
        send (juce::MidiMessage::noteOff (channel(), note));
    }

    Engine& engine;
    int target = -1;
    juce::MidiKeyboardState state;
    juce::MidiKeyboardComponent keyboard;
    juce::Label targetLabel, hint;
    juce::Label velocityLabel { {}, "Velocity" }, modLabel { {}, "Mod" }, bendLabel { {}, "Bend" }, ccLabel { {}, "CC" };
    juce::Slider velocity, modWheel, pitchBend, ccNumber, ccValue;
    juce::TextButton sustain { "Sustain" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyboardPanel)
};

} // namespace perf
