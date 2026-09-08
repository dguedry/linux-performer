#include "MainComponent.h"
#include "MappingSuggestions.h"
#include <optional>

using namespace juce;

namespace perf
{

namespace
{
    const Colour bgDark    { 0xff1e1f24 };
    const Colour bgPanel   { 0xff26282f };
    const Colour bgRow     { 0xff2e313a };
    const Colour accent    { 0xff4f9dff };
    const Colour accentDim { 0xff2b4f80 };
    const Colour textDim   { 0xff9aa0ab };

    String programTitle (int index, const ProgramDef& p)
    {
        String s = String (index).paddedLeft ('0', 3);
        if (p.name.isNotEmpty()) s << "  " << p.name;
        return s;
    }

    String noteName (int n) { return MidiMessage::getMidiNoteName (n, true, true, 3); }

    void styleHeader (Label& l)
    {
        l.setFont (FontOptions (14.0f, Font::bold));
        l.setColour (Label::textColourId, Colours::white);
    }
}

//==============================================================================
//  InputsPanel
//==============================================================================
class InputsPanel : public Component,
                    private ListBoxModel
{
public:
    InputsPanel (Engine& e, MainComponent& o) : engine (e), owner (o)
    {
        styleHeader (header);
        addAndMakeVisible (header);
        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (44);
        list.setColour (ListBox::backgroundColourId, bgPanel);

        addAndMakeVisible (addBtn);
        addAndMakeVisible (removeBtn);
        addBtn.onClick = [this]
        {
            auto idx = engine.addInput ("Input " + String (engine.getSetup().inputs.size() + 1));
            lastNotified = -1; setSelected (idx);
        };
        removeBtn.onClick = [this]
        {
            int sel = list.getSelectedRow();
            if (sel >= 0) { engine.removeInput (sel); lastNotified = -1; setSelected (jmax (0, sel - 1)); }
        };

        addAndMakeVisible (nameEditor);
        nameEditor.setTextToShowWhenEmpty ("Name", textDim);
        nameEditor.onTextChange = [this]
        {
            int sel = list.getSelectedRow();
            if (sel >= 0) { engine.setInputName (sel, nameEditor.getText()); list.repaintRow (sel); }
        };

        addAndMakeVisible (deviceBox);
        deviceBox.onChange = [this]
        {
            int sel = list.getSelectedRow();
            if (sel < 0) return;
            int id = deviceBox.getSelectedId();
            MidiDeviceInfo info;
            if (id >= 2 && id - 2 < devices.size()) info = devices[id - 2];
            engine.setInputMidiDevice (sel, info);
            list.repaintRow (sel);
        };

        addAndMakeVisible (channelBox);
        channelBox.addItem ("Omni", 1);
        for (int c = 1; c <= 16; ++c) channelBox.addItem ("Ch " + String (c), c + 1);
        channelBox.onChange = [this]
        {
            int sel = list.getSelectedRow();
            if (sel >= 0) { engine.setInputChannel (sel, channelBox.getSelectedId() - 1); list.repaintRow (sel); }
        };

        addAndMakeVisible (pcToggle);
        pcToggle.onClick = [this]
        {
            int sel = list.getSelectedRow();
            if (sel >= 0) engine.setInputRespondToProgramChange (sel, pcToggle.getToggleState());
        };

        for (auto* l : { &deviceLabel, &channelLabel })
        {
            addAndMakeVisible (l);
            l->setColour (Label::textColourId, textDim);
            l->setFont (FontOptions (12.0f));
        }
    }

    void setSelected (int idx)
    {
        list.selectRow (idx, false, true);
        refreshEditor();
    }

    void refresh()
    {
        devices = MidiInput::getAvailableDevices();
        list.updateContent();
        list.repaint();
        refreshEditor();
    }

    void refreshEditor()
    {
        int sel = list.getSelectedRow();
        auto& inputs = engine.getSetup().inputs;
        const bool valid = sel >= 0 && sel < (int) inputs.size();
        for (auto* c : std::initializer_list<Component*> { &nameEditor, &deviceBox, &channelBox, &pcToggle, &removeBtn })
            c->setEnabled (valid);
        if (! valid) return;

        auto& def = inputs[(size_t) sel];
        nameEditor.setText (def.name, dontSendNotification);

        deviceBox.clear (dontSendNotification);
        deviceBox.addItem ("(no MIDI device)", 1);
        int selectedId = 1;
        for (int i = 0; i < devices.size(); ++i)
        {
            deviceBox.addItem (devices[i].name, i + 2);
            if (devices[i].identifier == def.midiDeviceIdentifier) selectedId = i + 2;
        }
        if (selectedId == 1 && def.midiDeviceName.isNotEmpty())
        {
            deviceBox.addItem (def.midiDeviceName + " (disconnected)", 1000);
            selectedId = 1000;
        }
        deviceBox.setSelectedId (selectedId, dontSendNotification);
        channelBox.setSelectedId (def.channel + 1, dontSendNotification);
        pcToggle.setToggleState (def.respondToProgramChange, dontSendNotification);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        header.setBounds (r.removeFromTop (22));
        auto editor = r.removeFromBottom (150);
        auto buttons = r.removeFromBottom (26);
        r.removeFromBottom (4);
        list.setBounds (r);

        addBtn.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2, 0));
        removeBtn.setBounds (buttons.reduced (2, 0));

        editor.removeFromTop (6);
        nameEditor.setBounds (editor.removeFromTop (24));
        editor.removeFromTop (6);
        deviceLabel.setBounds (editor.removeFromTop (16));
        deviceBox.setBounds (editor.removeFromTop (24));
        editor.removeFromTop (6);
        auto chRow = editor.removeFromTop (24);
        channelLabel.setBounds (chRow.removeFromLeft (56));
        channelBox.setBounds (chRow.removeFromLeft (90));
        editor.removeFromTop (6);
        pcToggle.setBounds (editor.removeFromTop (22));
    }

    // ListBoxModel
    int getNumRows() override { return (int) engine.getSetup().inputs.size(); }

    void paintListBoxItem (int row, Graphics& g, int w, int, bool selected) override
    {
        auto& inputs = engine.getSetup().inputs;
        if (row < 0 || row >= (int) inputs.size()) return;
        auto& def = inputs[(size_t) row];

        g.fillAll (selected ? accentDim : bgRow);
        g.setColour (Colours::white);
        g.setFont (FontOptions (15.0f, Font::bold));
        g.drawText (def.name, 8, 4, w - 16, 18, Justification::centredLeft, true);

        g.setFont (FontOptions (12.0f));
        g.setColour (textDim);
        String info = def.midiDeviceName.isNotEmpty() ? def.midiDeviceName : "no device";
        if (def.channel > 0) info << "  ch " << def.channel;
        g.drawText (info, 8, 22, w - 80, 16, Justification::centredLeft, true);

        g.setColour (accent);
        g.setFont (FontOptions (13.0f, Font::bold));
        g.drawText ("PC " + String (def.currentProgram).paddedLeft ('0', 3), w - 72, 22, 64, 16, Justification::centredRight, true);
    }

    void selectedRowsChanged (int row) override { owner_select (row); }

private:
    void owner_select (int row)
    {
        if (row < 0 || row == lastNotified) return;
        lastNotified = row;
        owner.selectInput (row);
        refreshEditor();
    }

    Engine& engine;
    MainComponent& owner;
    Label header { {}, "INPUTS" };
    ListBox list;
    TextButton addBtn { "+ Add" }, removeBtn { "- Remove" };
    TextEditor nameEditor;
    Label deviceLabel { {}, "MIDI device" }, channelLabel { {}, "Channel" };
    ComboBox deviceBox, channelBox;
    ToggleButton pcToggle { "Respond to Program Change" };
    Array<MidiDeviceInfo> devices;
    int lastNotified = -1;
};

//==============================================================================
//  ProgramsPanel
//==============================================================================
class ProgramsPanel : public Component,
                      private ListBoxModel
{
public:
    ProgramsPanel (Engine& e, MainComponent& o) : engine (e), owner (o)
    {
        styleHeader (header);
        addAndMakeVisible (header);
        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (24);
        list.setColour (ListBox::backgroundColourId, bgPanel);

        addAndMakeVisible (nameEditor);
        nameEditor.setTextToShowWhenEmpty ("Program name", textDim);
        nameEditor.onTextChange = [this]
        {
            engine.setProgramName (owner.getSelectedInput(), owner.getEditedProgram(), nameEditor.getText());
            list.repaintRow (owner.getEditedProgram());
        };

        addAndMakeVisible (copyBtn);
        addAndMakeVisible (pasteBtn);
        addAndMakeVisible (clearBtn);
        copyBtn.onClick  = [this] { clipboard = owner.getEditedProgram(); pasteBtn.setEnabled (true); owner.showStatus ("Copied program " + String (clipboard)); };
        pasteBtn.onClick = [this] { if (clipboard >= 0) engine.copyProgram (owner.getSelectedInput(), clipboard, owner.getEditedProgram()); };
        clearBtn.onClick = [this] { engine.clearProgram (owner.getSelectedInput(), owner.getEditedProgram()); };
        pasteBtn.setEnabled (false);
    }

    void refresh()
    {
        const auto& inputs = engine.getSetup().inputs;
        const int in = owner.getSelectedInput();
        const bool valid = in >= 0 && in < (int) inputs.size();
        list.setEnabled (valid);
        nameEditor.setEnabled (valid);
        if (valid)
        {
            const int p = inputs[(size_t) in].currentProgram;
            list.selectRow (p, false, true);
            nameEditor.setText (inputs[(size_t) in].programs[(size_t) p].name, dontSendNotification);
        }
        list.updateContent();
        list.repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        header.setBounds (r.removeFromTop (22));
        auto bottom = r.removeFromBottom (26);
        r.removeFromBottom (4);
        nameEditor.setBounds (r.removeFromBottom (24));
        r.removeFromBottom (4);
        list.setBounds (r);
        const int w = bottom.getWidth() / 3;
        copyBtn.setBounds (bottom.removeFromLeft (w).reduced (2, 0));
        pasteBtn.setBounds (bottom.removeFromLeft (w).reduced (2, 0));
        clearBtn.setBounds (bottom.reduced (2, 0));
    }

    int getNumRows() override { return InputDef::numPrograms; }

    void paintListBoxItem (int row, Graphics& g, int w, int h, bool selected) override
    {
        const auto& inputs = engine.getSetup().inputs;
        const int in = owner.getSelectedInput();
        if (in < 0 || in >= (int) inputs.size()) return;
        const auto& prog = inputs[(size_t) in].programs[(size_t) row];

        g.fillAll (selected ? accentDim : (row % 2 ? bgRow : bgPanel));
        g.setColour (prog.isEmpty() ? textDim : Colours::white);
        g.setFont (FontOptions (13.0f, prog.isEmpty() ? Font::plain : Font::bold));
        g.drawText (programTitle (row, prog), 8, 0, w - 60, h, Justification::centredLeft, true);

        if (! prog.isEmpty())
        {
            g.setColour (engine.isProgramLoaded (in, row) ? Colours::limegreen : textDim);
            g.setFont (FontOptions (11.0f));
            g.drawText (String (prog.slots.size()) + (prog.slots.size() == 1 ? " plugin" : " plugins"), w - 70, 0, 64, h, Justification::centredRight, true);
        }
    }

    void listBoxItemClicked (int row, const MouseEvent&) override
    {
        engine.selectProgram (owner.getSelectedInput(), row);
    }

    void returnKeyPressed (int row) override { engine.selectProgram (owner.getSelectedInput(), row); }

private:
    Engine& engine;
    MainComponent& owner;
    Label header { {}, "PROGRAMS" };
    ListBox list;
    TextEditor nameEditor;
    TextButton copyBtn { "Copy" }, pasteBtn { "Paste" }, clearBtn { "Clear" };
    int clipboard = -1;
};

//==============================================================================
//  EffectChainComponent -- an ordered list of insert effects (per slot or per program)
//==============================================================================
class EffectChainComponent : public Component
{
public:
    static constexpr int rowHeight = 26;

    EffectChainComponent (Engine& e, MainComponent& o, int slotIndex) : engine (e), owner (o), slot (slotIndex)
    {
        addAndMakeVisible (addBtn);
        addBtn.onClick = [this] { showAddMenu(); };
        addBtn.setTooltip (slot < 0 ? "Add an effect after the mix of all slots" : "Add an insert effect after this instrument");
    }

    void setSlot (int s) { slot = s; }

    void rebuild (const std::vector<EffectDef>& effects, int inputIndex, int program)
    {
        rows.clear();
        for (int i = 0; i < (int) effects.size(); ++i)
        {
            auto row = std::make_unique<Row> (*this, i);
            row->update (effects[(size_t) i],
                         engine.getPluginInstance (inputIndex, program, slot, i) != nullptr,
                         engine.getPluginLoadError (inputIndex, program, slot, i),
                         i > 0, i + 1 < (int) effects.size());
            addAndMakeVisible (row.get());
            rows.push_back (std::move (row));
        }
        resized();
    }

    int preferredHeight() const { return (int) rows.size() * rowHeight + rowHeight; }

    void resized() override
    {
        auto r = getLocalBounds();
        for (auto& row : rows)
            row->setBounds (r.removeFromTop (rowHeight));
        auto last = r.removeFromTop (rowHeight).reduced (0, 2);
        addBtn.setBounds (last.removeFromRight (110));
    }

    void paint (Graphics& g) override
    {
        if (rows.empty()) return;
        g.setColour (textDim.withAlpha (0.35f));
        g.drawVerticalLine (10, 0.0f, (float) rows.size() * rowHeight);
    }

private:
    struct Row : public Component
    {
        Row (EffectChainComponent& c, int idx) : chain (c), index (idx)
        {
            addAndMakeVisible (on);
            addAndMakeVisible (name);
            addAndMakeVisible (guiBtn);
            addAndMakeVisible (upBtn);
            addAndMakeVisible (downBtn);
            addAndMakeVisible (removeBtn);
            name.setFont (FontOptions (13.0f));
            on.setTooltip ("Effect active (off = bypassed)");
            upBtn.setTooltip ("Move earlier in the chain");
            downBtn.setTooltip ("Move later in the chain");

            auto& e = chain.engine;
            auto& o = chain.owner;
            on.onClick        = [this, &e, &o] { e.setEffectBypassed (o.getSelectedInput(), o.getEditedProgram(), chain.slot, index, ! on.getToggleState()); };
            guiBtn.onClick    = [this, &o]     { o.openPluginEditor (o.getSelectedInput(), o.getEditedProgram(), chain.slot, index); };
            upBtn.onClick     = [this, &e, &o] { e.moveEffect (o.getSelectedInput(), o.getEditedProgram(), chain.slot, index, index - 1); };
            downBtn.onClick   = [this, &e, &o] { e.moveEffect (o.getSelectedInput(), o.getEditedProgram(), chain.slot, index, index + 1); };
            removeBtn.onClick = [this, &e, &o] { e.removeEffect (o.getSelectedInput(), o.getEditedProgram(), chain.slot, index); };
        }

        void update (const EffectDef& def, bool loaded, const String& error, bool canUp, bool canDown)
        {
            on.setToggleState (! def.bypassed, dontSendNotification);
            name.setText (def.plugin.name + "  [" + def.plugin.pluginFormatName + "]", dontSendNotification);
            name.setColour (Label::textColourId, error.isNotEmpty() ? Colours::orangered : (def.bypassed ? textDim : Colours::white));
            name.setTooltip (error.isNotEmpty() ? error : def.plugin.fileOrIdentifier);
            guiBtn.setEnabled (loaded);
            upBtn.setEnabled (canUp);
            downBtn.setEnabled (canDown);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (0, 2);
            r.removeFromLeft (18);   // indent under the chain line
            on.setBounds (r.removeFromLeft (24));
            removeBtn.setBounds (r.removeFromRight (28)); r.removeFromRight (4);
            downBtn.setBounds (r.removeFromRight (26));
            upBtn.setBounds (r.removeFromRight (26));     r.removeFromRight (4);
            guiBtn.setBounds (r.removeFromRight (64));    r.removeFromRight (4);
            name.setBounds (r);
        }

        EffectChainComponent& chain;
        int index;
        ToggleButton on;
        Label name;
        TextButton guiBtn { "Edit GUI" }, upBtn { "^" }, downBtn { "v" }, removeBtn { "X" };
    };

    void showAddMenu()
    {
        Array<PluginDescription> types;
        for (auto& d : engine.getPluginHost().getKnownPlugins().getTypes())
            if (! d.isInstrument) types.add (d);
        if (types.isEmpty())
        {
            owner.showStatus ("No effect plugins known yet. Use Plugins... to scan for VST3/LV2 plugins.");
            return;
        }
        PopupMenu menu;
        KnownPluginList::addToMenu (menu, types, KnownPluginList::sortByManufacturer);
        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&addBtn), [this, types] (int result)
        {
            const int idx = KnownPluginList::getIndexChosenByMenu (types, result);
            if (idx < 0) return;
            String error;
            if (! engine.addEffect (owner.getSelectedInput(), owner.getEditedProgram(), slot, types[idx], error))
                owner.showStatus ("Could not load " + types[idx].name + ": " + error);
            else
                owner.showStatus ("Added effect " + types[idx].name);
        });
    }

    Engine& engine;
    MainComponent& owner;
    int slot;
    TextButton addBtn { "+ Add effect..." };
    std::vector<std::unique_ptr<Row>> rows;

    friend struct Row;
};

//==============================================================================
//  SlotsPanel
//==============================================================================
class SlotsPanel : public Component
{
public:
    static constexpr int slotHeaderHeight = 64;

    struct SlotRow : public Component
    {
        SlotRow (SlotsPanel& p, int idx) : panel (p), index (idx), chain (p.engine, p.owner, idx)
        {
            addAndMakeVisible (enabled);
            addAndMakeVisible (name);
            addAndMakeVisible (gain);
            addAndMakeVisible (transpose);
            addAndMakeVisible (lowKey);
            addAndMakeVisible (highKey);
            addAndMakeVisible (outCh);
            addAndMakeVisible (guiBtn);
            addAndMakeVisible (removeBtn);
            addAndMakeVisible (chain);

            name.setFont (FontOptions (14.0f, Font::bold));
            name.setColour (Label::textColourId, Colours::white);

            gain.setRange (-60.0, 12.0, 0.1);
            gain.setSliderStyle (Slider::LinearBar);
            gain.setTextValueSuffix (" dB");
            gain.setDoubleClickReturnValue (true, 0.0);
            gain.setTooltip ("Gain (after the slot's effects)");

            transpose.setRange (-36, 36, 1);
            transpose.setSliderStyle (Slider::LinearBar);
            transpose.setTextValueSuffix (" st");
            transpose.setDoubleClickReturnValue (true, 0.0);
            transpose.setTooltip ("Transpose");

            for (auto* s : { &lowKey, &highKey })
            {
                s->setRange (0, 127, 1);
                s->setSliderStyle (Slider::LinearBar);
                s->textFromValueFunction = [] (double v) { return noteName ((int) v); };
                s->valueFromTextFunction = [] (const String& t) { return (double) t.getIntValue(); };
            }
            lowKey.setTooltip ("Lowest key");
            highKey.setTooltip ("Highest key");

            outCh.addItem ("Ch: keep", 1);
            for (int c = 1; c <= 16; ++c) outCh.addItem ("Ch " + String (c), c + 1);
            outCh.setTooltip ("Force output MIDI channel");

            auto& e = panel.engine;
            auto& o = panel.owner;
            enabled.onClick   = [this, &e, &o] { e.setSlotEnabled   (o.getSelectedInput(), o.getEditedProgram(), index, enabled.getToggleState()); };
            gain.onValueChange      = [this, &e, &o] { e.setSlotGainDb    (o.getSelectedInput(), o.getEditedProgram(), index, (float) gain.getValue()); };
            transpose.onValueChange = [this, &e, &o] { e.setSlotTranspose (o.getSelectedInput(), o.getEditedProgram(), index, (int) transpose.getValue()); };
            lowKey.onValueChange    = [this, &e, &o] { if (lowKey.getValue() > highKey.getValue()) highKey.setValue (lowKey.getValue(), dontSendNotification);
                                                       e.setSlotKeyRange (o.getSelectedInput(), o.getEditedProgram(), index, (int) lowKey.getValue(), (int) highKey.getValue()); };
            highKey.onValueChange   = [this, &e, &o] { if (highKey.getValue() < lowKey.getValue()) lowKey.setValue (highKey.getValue(), dontSendNotification);
                                                       e.setSlotKeyRange (o.getSelectedInput(), o.getEditedProgram(), index, (int) lowKey.getValue(), (int) highKey.getValue()); };
            outCh.onChange    = [this, &e, &o] { e.setSlotOutChannel (o.getSelectedInput(), o.getEditedProgram(), index, outCh.getSelectedId() - 1); };
            guiBtn.onClick    = [this, &o] { o.openPluginEditor (o.getSelectedInput(), o.getEditedProgram(), index, -1); };
            removeBtn.onClick = [this, &e, &o] { e.removeSlot (o.getSelectedInput(), o.getEditedProgram(), index); };
        }

        void update (const SlotDef& def, bool loaded, const String& error, int inputIndex, int program)
        {
            enabled.setToggleState (def.enabled, dontSendNotification);
            name.setText (def.plugin.name + "  [" + def.plugin.pluginFormatName + "]", dontSendNotification);
            name.setColour (Label::textColourId, error.isNotEmpty() ? Colours::orangered : Colours::white);
            name.setTooltip (error.isNotEmpty() ? error : def.plugin.fileOrIdentifier);
            gain.setValue (def.gainDb, dontSendNotification);
            transpose.setValue (def.transpose, dontSendNotification);
            lowKey.setValue (def.lowKey, dontSendNotification);
            highKey.setValue (def.highKey, dontSendNotification);
            outCh.setSelectedId (def.outChannel + 1, dontSendNotification);
            guiBtn.setEnabled (loaded);
            chain.rebuild (def.effects, inputIndex, program);
        }

        int preferredHeight() const { return slotHeaderHeight + chain.preferredHeight() + 4; }

        void paint (Graphics& g) override
        {
            g.setColour (bgRow);
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0, 1), 4.0f);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (6, 4);
            auto top = r.removeFromTop (24);
            enabled.setBounds (top.removeFromLeft (24));
            removeBtn.setBounds (top.removeFromRight (28));
            top.removeFromRight (4);
            guiBtn.setBounds (top.removeFromRight (64));
            name.setBounds (top);

            r.removeFromTop (4);
            auto bottom = r.removeFromTop (22);
            const int w = bottom.getWidth();
            gain.setBounds (bottom.removeFromLeft (w * 28 / 100).reduced (2, 0));
            transpose.setBounds (bottom.removeFromLeft (w * 18 / 100).reduced (2, 0));
            lowKey.setBounds (bottom.removeFromLeft (w * 17 / 100).reduced (2, 0));
            highKey.setBounds (bottom.removeFromLeft (w * 17 / 100).reduced (2, 0));
            outCh.setBounds (bottom.reduced (2, 0));

            r.removeFromTop (6);
            chain.setBounds (r.withTrimmedLeft (24));
        }

        SlotsPanel& panel;
        int index;
        ToggleButton enabled;
        Label name;
        Slider gain, transpose, lowKey, highKey;
        ComboBox outCh;
        TextButton guiBtn { "Edit GUI" }, removeBtn { "X" };
        EffectChainComponent chain;
    };

    /** The program-level chain, shown after the slots. */
    struct ProgramChainRow : public Component
    {
        ProgramChainRow (SlotsPanel& p) : chain (p.engine, p.owner, -1)
        {
            addAndMakeVisible (header);
            addAndMakeVisible (chain);
            header.setFont (FontOptions (13.0f, Font::bold));
            header.setColour (Label::textColourId, accent);
        }
        int preferredHeight() const { return 26 + chain.preferredHeight() + 6; }
        void paint (Graphics& g) override
        {
            g.setColour (bgRow.darker (0.15f));
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0, 1), 4.0f);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (6, 4);
            header.setBounds (r.removeFromTop (22));
            chain.setBounds (r.withTrimmedLeft (24));
        }
        Label header { {}, "PROGRAM EFFECTS  (after the mix of all slots)" };
        EffectChainComponent chain;
    };

    SlotsPanel (Engine& e, MainComponent& o) : engine (e), owner (o)
    {
        styleHeader (header);
        addAndMakeVisible (header);
        addAndMakeVisible (addBtn);
        addBtn.onClick = [this] { showAddMenu(); };
        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&container, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (emptyLabel);
        emptyLabel.setColour (Label::textColourId, textDim);
        emptyLabel.setJustificationType (Justification::centred);
    }

    void refresh()
    {
        const auto& inputs = engine.getSetup().inputs;
        const int in = owner.getSelectedInput();
        const int prog = owner.getEditedProgram();
        rows.clear();
        programChain.reset();
        if (in >= 0 && in < (int) inputs.size())
        {
            const auto& def = inputs[(size_t) in].programs[(size_t) prog];
            for (int s = 0; s < (int) def.slots.size(); ++s)
            {
                auto row = std::make_unique<SlotRow> (*this, s);
                row->update (def.slots[(size_t) s], engine.getSlotInstance (in, prog, s) != nullptr, engine.getSlotLoadError (in, prog, s), in, prog);
                container.addAndMakeVisible (row.get());
                rows.push_back (std::move (row));
            }
            programChain = std::make_unique<ProgramChainRow> (*this);
            programChain->chain.rebuild (def.effects, in, prog);
            container.addAndMakeVisible (programChain.get());
        }
        emptyLabel.setVisible (rows.empty() && (programChain == nullptr || def_hasNoEffects (in, prog)));
        layoutRows();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        auto top = r.removeFromTop (24);
        addBtn.setBounds (top.removeFromRight (140));
        header.setBounds (top);
        r.removeFromTop (4);
        viewport.setBounds (r);
        emptyLabel.setBounds (r.withTrimmedBottom (r.getHeight() / 3));
        layoutRows();
    }

private:
    bool def_hasNoEffects (int in, int prog) const
    {
        const auto& inputs = engine.getSetup().inputs;
        if (in < 0 || in >= (int) inputs.size()) return true;
        return inputs[(size_t) in].programs[(size_t) prog].effects.empty();
    }

    void layoutRows()
    {
        int total = 0;
        for (auto& r : rows) total += r->preferredHeight() + 4;
        if (programChain != nullptr) total += programChain->preferredHeight() + 4;

        const int w = viewport.getWidth() - (total > viewport.getHeight() ? viewport.getScrollBarThickness() : 0);
        container.setSize (jmax (1, w), jmax (1, total));
        int y = 0;
        for (auto& r : rows) { r->setBounds (0, y, w, r->preferredHeight()); y += r->preferredHeight() + 4; }
        if (programChain != nullptr) programChain->setBounds (0, y, w, programChain->preferredHeight());
    }

    void showAddMenu()
    {
        auto types = engine.getPluginHost().getKnownPlugins().getTypes();
        if (types.isEmpty())
        {
            owner.showStatus ("No plugins known yet. Use Plugins... to scan for VST3/LV2 plugins.");
            return;
        }
        PopupMenu menu;
        KnownPluginList::addToMenu (menu, types, KnownPluginList::sortByManufacturer);
        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&addBtn), [this, types] (int result)
        {
            const int idx = KnownPluginList::getIndexChosenByMenu (types, result);
            if (idx < 0) return;
            String error;
            if (! engine.addSlot (owner.getSelectedInput(), owner.getEditedProgram(), types[idx], error))
                owner.showStatus ("Could not load " + types[idx].name + ": " + error);
            else
                owner.showStatus ("Added " + types[idx].name);
        });
    }

    Engine& engine;
    MainComponent& owner;
    Label header { {}, "PLUGINS" };
    TextButton addBtn { "+ Add instrument..." };
    Viewport viewport;
    Component container;
    std::vector<std::unique_ptr<SlotRow>> rows;
    std::unique_ptr<ProgramChainRow> programChain;
    Label emptyLabel { {}, "No plugins in this program. Click \"Add instrument...\"" };

    friend struct SlotRow;
    friend struct ProgramChainRow;
};

//==============================================================================
//  SuggestionsComponent -- confirm dialog for proposed mappings
//==============================================================================
class SuggestionsComponent : public Component,
                             private ListBoxModel
{
public:
    SuggestionsComponent (Engine& e, int input, int program, const String& targetName, std::vector<MappingSuggestion> s)
        : engine (e), inputIndex (input), programIndex (program), suggestions (std::move (s))
    {
        selected.insertMultiple (0, true, (int) suggestions.size());

        addAndMakeVisible (info);
        info.setColour (Label::textColourId, textDim);
        info.setText (suggestions.empty()
                        ? "No suggestions for " + targetName + ". Its parameter names don't match any standard controller, and no template is saved. Use Learn MIDI, then Save template."
                        : String ((int) suggestions.size()) + " proposed mappings for " + targetName + ". Untick any you don't want.",
                      dontSendNotification);
        info.setJustificationType (Justification::topLeft);

        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (24);
        list.setColour (ListBox::backgroundColourId, bgPanel);
        list.setClickingTogglesRowSelection (false);

        addAndMakeVisible (addBtn);
        addAndMakeVisible (cancelBtn);
        addBtn.setEnabled (! suggestions.empty());
        addBtn.onClick = [this]
        {
            int n = 0;
            for (int i = 0; i < (int) suggestions.size(); ++i)
                if (selected[i]) { engine.addMapping (inputIndex, programIndex, suggestions[(size_t) i].mapping); ++n; }
            close();
        };
        cancelBtn.onClick = [this] { close(); };
        setSize (620, jlimit (180, 520, 110 + 24 * (int) suggestions.size()));
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        info.setBounds (r.removeFromTop (40));
        auto buttons = r.removeFromBottom (28);
        cancelBtn.setBounds (buttons.removeFromRight (100));
        buttons.removeFromRight (6);
        addBtn.setBounds (buttons.removeFromRight (130));
        r.removeFromBottom (8);
        list.setBounds (r);
    }

    int getNumRows() override { return (int) suggestions.size(); }

    void paintListBoxItem (int row, Graphics& g, int w, int h, bool) override
    {
        if (row < 0 || row >= (int) suggestions.size()) return;
        const auto& s = suggestions[(size_t) row];
        g.fillAll (row % 2 ? bgRow : bgPanel);
        getLookAndFeel().drawTickBox (g, *this, 6.0f, 4.0f, 16.0f, 16.0f, selected[row], true, false, false);
        g.setColour (Colours::white);
        g.setFont (FontOptions (13.0f, Font::bold));
        g.drawText (s.mapping.sourceDescription(), 30, 0, 90, h, Justification::centredLeft, true);
        g.setFont (FontOptions (13.0f));
        g.drawText (s.mapping.paramName, 125, 0, w * 45 / 100, h, Justification::centredLeft, true);
        g.setColour (textDim);
        g.setFont (FontOptions (12.0f));
        g.drawText (s.reason, 125 + w * 45 / 100, 0, w - (125 + w * 45 / 100) - 6, h, Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const MouseEvent&) override
    {
        if (row >= 0 && row < selected.size()) { selected.set (row, ! selected[row]); list.repaintRow (row); }
    }

private:
    void close()
    {
        if (auto* dw = findParentComponentOfClass<DialogWindow>())
            dw->exitModalState (0);
    }

    Engine& engine;
    int inputIndex, programIndex;
    std::vector<MappingSuggestion> suggestions;
    Array<bool> selected;
    Label info;
    ListBox list;
    TextButton addBtn { "Add selected" }, cancelBtn { "Cancel" };
};

//==============================================================================
//  MappingsPanel
//==============================================================================
class MappingsPanel : public Component,
                      private TableListBoxModel
{
public:
    enum Columns { colSource = 1, colTarget, colParam, colMin, colMax, colPass };

    struct Target { int slot = 0, effect = -1; String label; };

    MappingsPanel (Engine& e, MainComponent& o) : engine (e), owner (o)
    {
        styleHeader (header);
        addAndMakeVisible (header);
        addAndMakeVisible (table);
        table.setModel (this);
        table.setColour (ListBox::backgroundColourId, bgPanel);
        table.setHeaderHeight (22);
        table.setRowHeight (22);
        auto& th = table.getHeader();
        th.addColumn ("Source", colSource, 90);
        th.addColumn ("Target", colTarget, 150);
        th.addColumn ("Parameter", colParam, 220);
        th.addColumn ("Min", colMin, 50);
        th.addColumn ("Max", colMax, 50);
        th.addColumn ("Pass", colPass, 44);
        th.setStretchToFitActive (true);

        // Editor row 1: target
        addAndMakeVisible (targetBox);
        addAndMakeVisible (paramBox);
        addAndMakeVisible (touchedBtn);
        targetBox.onChange = [this] { fillParams(); };
        touchedBtn.onClick = [this] { useTouched(); };
        touchedBtn.setTooltip ("Select the parameter you last moved in a plugin GUI");

        // Editor row 2: source + range
        addAndMakeVisible (learnBtn);
        addAndMakeVisible (sourceLabel);
        addAndMakeVisible (minSlider);
        addAndMakeVisible (maxSlider);
        addAndMakeVisible (passToggle);
        learnBtn.setClickingTogglesState (true);
        learnBtn.setColour (TextButton::buttonOnColourId, Colours::orangered);
        learnBtn.onClick = [this] { engine.setLearnArmed (learnBtn.getToggleState()); if (learnBtn.getToggleState()) owner.showStatus ("Move a MIDI controller..."); };
        sourceLabel.setColour (Label::textColourId, accent);
        sourceLabel.setFont (FontOptions (13.0f, Font::bold));
        for (auto* s : { &minSlider, &maxSlider })
        {
            s->setRange (0.0, 1.0, 0.01);
            s->setSliderStyle (Slider::LinearBar);
        }
        minSlider.setValue (0.0, dontSendNotification);
        maxSlider.setValue (1.0, dontSendNotification);
        minSlider.setTooltip ("Parameter value at controller minimum");
        maxSlider.setTooltip ("Parameter value at controller maximum");
        passToggle.setTooltip ("Also send the raw MIDI message to the plugins");

        // Editor row 3: actions
        addAndMakeVisible (addBtn);
        addAndMakeVisible (updateBtn);
        addAndMakeVisible (removeBtn);
        addAndMakeVisible (suggestBtn);
        addAndMakeVisible (templateBtn);
        suggestBtn.setTooltip ("Propose mappings for the selected target: from its saved template, or from parameter names matched to standard MIDI controller numbers");
        templateBtn.setTooltip ("Save the selected target's current mappings as the default template for this plugin");
        suggestBtn.onClick  = [this] { showSuggestions(); };
        templateBtn.onClick = [this] { saveTemplate(); };
        addBtn.onClick    = [this] { if (auto m = makeMapping()) engine.addMapping (owner.getSelectedInput(), owner.getEditedProgram(), *m); };
        updateBtn.onClick = [this] { int r = table.getSelectedRow(); if (r >= 0) if (auto m = makeMapping()) engine.updateMapping (owner.getSelectedInput(), owner.getEditedProgram(), r, *m); };
        removeBtn.onClick = [this] { int r = table.getSelectedRow(); if (r >= 0) engine.removeMapping (owner.getSelectedInput(), owner.getEditedProgram(), r); };
    }

    void refresh()
    {
        Target prev;
        bool hadPrev = false;
        if (auto* t = selectedTarget()) { prev = *t; hadPrev = true; }

        targets.clear();
        targetBox.clear (dontSendNotification);
        if (const auto* def = currentProgram())
        {
            for (int s = 0; s < (int) def->slots.size(); ++s)
            {
                targets.push_back ({ s, -1, String (s + 1) + ": " + def->slots[(size_t) s].plugin.name });
                for (int e = 0; e < (int) def->slots[(size_t) s].effects.size(); ++e)
                    targets.push_back ({ s, e, String (s + 1) + " > FX" + String (e + 1) + ": " + def->slots[(size_t) s].effects[(size_t) e].plugin.name });
            }
            for (int e = 0; e < (int) def->effects.size(); ++e)
                targets.push_back ({ -1, e, "Program FX" + String (e + 1) + ": " + def->effects[(size_t) e].plugin.name });
        }
        for (int i = 0; i < (int) targets.size(); ++i)
            targetBox.addItem (targets[(size_t) i].label, i + 1);

        int reselect = targets.empty() ? 0 : 1;
        if (hadPrev)
            for (int i = 0; i < (int) targets.size(); ++i)
                if (targets[(size_t) i].slot == prev.slot && targets[(size_t) i].effect == prev.effect) reselect = i + 1;
        if (reselect > 0) targetBox.setSelectedId (reselect, dontSendNotification);

        fillParams();
        table.updateContent();
        table.repaint();
        updateButtons();
    }

    void onLearn (MappingDef::Source s, int number)
    {
        learnedSource = s;
        learnedNumber = number;
        haveSource = true;
        learnBtn.setToggleState (false, dontSendNotification);
        MappingDef tmp; tmp.source = s; tmp.number = number;
        sourceLabel.setText (tmp.sourceDescription(), dontSendNotification);
        owner.showStatus ("Learned " + tmp.sourceDescription() + ". Pick a parameter and click Add.");
        updateButtons();
    }

    void onTouched (int slot, int effect, int paramIndex)
    {
        touchedSlot = slot; touchedEffect = effect; touchedParam = paramIndex;
        touchedBtn.setEnabled (true);
        if (auto* inst = engine.getPluginInstance (owner.getSelectedInput(), owner.getEditedProgram(), slot, effect))
        {
            auto& params = inst->getParameters();
            if (paramIndex >= 0 && paramIndex < params.size())
                touchedBtn.setButtonText ("Use touched: " + params[paramIndex]->getName (24));
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        header.setBounds (r.removeFromTop (22));
        auto editor = r.removeFromBottom (3 * 26 + 8);
        r.removeFromBottom (4);
        table.setBounds (r);

        auto row1 = editor.removeFromTop (24);
        targetBox.setBounds (row1.removeFromLeft (220)); row1.removeFromLeft (4);
        touchedBtn.setBounds (row1.removeFromRight (200)); row1.removeFromRight (4);
        paramBox.setBounds (row1);
        editor.removeFromTop (4);

        auto row2 = editor.removeFromTop (24);
        learnBtn.setBounds (row2.removeFromLeft (100)); row2.removeFromLeft (4);
        sourceLabel.setBounds (row2.removeFromLeft (100)); row2.removeFromLeft (4);
        passToggle.setBounds (row2.removeFromRight (140));
        auto half = row2.getWidth() / 2;
        minSlider.setBounds (row2.removeFromLeft (half).reduced (2, 0));
        maxSlider.setBounds (row2.reduced (2, 0));
        editor.removeFromTop (4);

        auto row3 = editor.removeFromTop (24);
        addBtn.setBounds (row3.removeFromLeft (100)); row3.removeFromLeft (4);
        updateBtn.setBounds (row3.removeFromLeft (100)); row3.removeFromLeft (4);
        removeBtn.setBounds (row3.removeFromLeft (100));
        templateBtn.setBounds (row3.removeFromRight (120)); row3.removeFromRight (4);
        suggestBtn.setBounds (row3.removeFromRight (110));
    }

    // TableListBoxModel
    int getNumRows() override { auto* d = currentProgram(); return d ? (int) d->mappings.size() : 0; }

    void paintRowBackground (Graphics& g, int row, int, int, bool selected) override
    {
        g.fillAll (selected ? accentDim : (row % 2 ? bgRow : bgPanel));
    }

    void paintCell (Graphics& g, int row, int col, int w, int h, bool) override
    {
        auto* def = currentProgram();
        if (def == nullptr || row < 0 || row >= (int) def->mappings.size()) return;
        const auto& m = def->mappings[(size_t) row];
        String text;
        switch (col)
        {
            case colSource: text = m.sourceDescription(); break;
            case colTarget: text = targetLabel (m.slot, m.effect); break;
            case colParam:  text = m.paramName.isNotEmpty() ? m.paramName : m.paramId; break;
            case colMin:    text = String (m.minValue, 2); break;
            case colMax:    text = String (m.maxValue, 2); break;
            case colPass:   text = m.passThrough ? "yes" : "-"; break;
            default: break;
        }
        g.setColour (Colours::white);
        g.setFont (FontOptions (13.0f));
        g.drawText (text, 4, 0, w - 8, h, Justification::centredLeft, true);
    }

    void selectedRowsChanged (int row) override
    {
        auto* def = currentProgram();
        if (def == nullptr || row < 0 || row >= (int) def->mappings.size()) { updateButtons(); return; }
        const auto& m = def->mappings[(size_t) row];
        learnedSource = m.source; learnedNumber = m.number; haveSource = true;
        sourceLabel.setText (m.sourceDescription(), dontSendNotification);
        for (int i = 0; i < (int) targets.size(); ++i)
            if (targets[(size_t) i].slot == m.slot && targets[(size_t) i].effect == m.effect)
                targetBox.setSelectedId (i + 1, dontSendNotification);
        fillParams();
        for (int i = 0; i < paramIds.size(); ++i)
            if (paramIds[i] == m.paramId) { paramBox.setSelectedId (i + 1, dontSendNotification); break; }
        minSlider.setValue (m.minValue, dontSendNotification);
        maxSlider.setValue (m.maxValue, dontSendNotification);
        passToggle.setToggleState (m.passThrough, dontSendNotification);
        updateButtons();
    }

private:
    const ProgramDef* currentProgram() const
    {
        const auto& inputs = engine.getSetup().inputs;
        const int in = owner.getSelectedInput();
        if (in < 0 || in >= (int) inputs.size()) return nullptr;
        return &inputs[(size_t) in].programs[(size_t) owner.getEditedProgram()];
    }

    String targetLabel (int slot, int effect) const
    {
        for (auto& t : targets)
            if (t.slot == slot && t.effect == effect) return t.label;
        if (slot < 0) return "Program FX" + String (effect + 1);
        return String (slot + 1) + (effect >= 0 ? " > FX" + String (effect + 1) : String());
    }

    const Target* selectedTarget() const
    {
        const int i = targetBox.getSelectedId() - 1;
        return (i >= 0 && i < (int) targets.size()) ? &targets[(size_t) i] : nullptr;
    }

    void fillParams()
    {
        const String prev = paramBox.getSelectedId() > 0 && paramBox.getSelectedId() <= paramIds.size()
                                ? paramIds[paramBox.getSelectedId() - 1] : String();
        paramBox.clear (dontSendNotification);
        paramIds.clear();
        auto* t = selectedTarget();
        auto* inst = t != nullptr ? engine.getPluginInstance (owner.getSelectedInput(), owner.getEditedProgram(), t->slot, t->effect) : nullptr;
        if (inst == nullptr)
        {
            paramBox.setTextWhenNothingSelected (t != nullptr ? "(plugin not loaded)" : "(no plugin)");
            updateButtons();
            return;
        }
        paramBox.setTextWhenNothingSelected ("Choose parameter...");
        int id = 1, reselect = 0;
        for (auto* p : inst->getParameters())
        {
            String name = p->getName (64);
            if (name.isEmpty()) name = "Param " + String (p->getParameterIndex());
            paramBox.addItem (name, id);
            paramIds.add (Engine::getParameterId (*p));
            if (paramIds[paramIds.size() - 1] == prev) reselect = id;
            ++id;
        }
        if (reselect > 0) paramBox.setSelectedId (reselect, dontSendNotification);
        updateButtons();
    }

    void useTouched()
    {
        if (touchedParam < 0) return;
        for (int i = 0; i < (int) targets.size(); ++i)
            if (targets[(size_t) i].slot == touchedSlot && targets[(size_t) i].effect == touchedEffect)
                targetBox.setSelectedId (i + 1, dontSendNotification);
        fillParams();
        if (touchedParam < paramIds.size())
            paramBox.setSelectedId (touchedParam + 1, dontSendNotification);
        updateButtons();
    }

    std::optional<MappingDef> makeMapping()
    {
        auto* t = selectedTarget();
        const int pi = paramBox.getSelectedId() - 1;
        if (! haveSource) { owner.showStatus ("Click Learn MIDI and move a controller first."); return {}; }
        if (t == nullptr || pi < 0 || pi >= paramIds.size()) { owner.showStatus ("Choose a target plugin and a parameter."); return {}; }
        MappingDef m;
        m.source = learnedSource; m.number = learnedNumber;
        m.slot = t->slot; m.effect = t->effect; m.paramId = paramIds[pi];
        m.paramName = paramBox.getItemText (pi);
        m.minValue = (float) minSlider.getValue();
        m.maxValue = (float) maxSlider.getValue();
        m.passThrough = passToggle.getToggleState();
        return m;
    }

    void updateButtons()
    {
        const bool canMake = haveSource && targetBox.getSelectedId() > 0 && paramBox.getSelectedId() > 0;
        addBtn.setEnabled (canMake);
        updateBtn.setEnabled (canMake && table.getSelectedRow() >= 0);
        removeBtn.setEnabled (table.getSelectedRow() >= 0);
        touchedBtn.setEnabled (touchedParam >= 0);

        auto* t = selectedTarget();
        const bool loaded = t != nullptr && engine.getPluginInstance (owner.getSelectedInput(), owner.getEditedProgram(), t->slot, t->effect) != nullptr;
        suggestBtn.setEnabled (loaded);
        int mappedHere = 0;
        if (auto* def = currentProgram(); def != nullptr && t != nullptr)
            for (auto& m : def->mappings) if (m.slot == t->slot && m.effect == t->effect) ++mappedHere;
        templateBtn.setEnabled (loaded && mappedHere > 0);
    }

    const PluginDescription* targetDescription (const Target& t) const
    {
        auto* def = currentProgram();
        if (def == nullptr) return nullptr;
        if (t.effect < 0) return (t.slot >= 0 && t.slot < (int) def->slots.size()) ? &def->slots[(size_t) t.slot].plugin : nullptr;
        auto* chain = def->chainFor (t.slot);
        return (chain != nullptr && t.effect < (int) chain->size()) ? &(*chain)[(size_t) t.effect].plugin : nullptr;
    }

    void showSuggestions()
    {
        auto* t = selectedTarget();
        auto* def = currentProgram();
        if (t == nullptr || def == nullptr) return;
        auto* inst = engine.getPluginInstance (owner.getSelectedInput(), owner.getEditedProgram(), t->slot, t->effect);
        auto* desc = targetDescription (*t);
        if (inst == nullptr || desc == nullptr) { owner.showStatus ("Plugin is not loaded"); return; }

        auto suggestions = suggestMappings (*inst, *desc, engine.getPluginHost().getMappingTemplates(), t->slot, t->effect, def->mappings);
        auto* content = new SuggestionsComponent (engine, owner.getSelectedInput(), owner.getEditedProgram(), t->label, std::move (suggestions));
        DialogWindow::LaunchOptions o;
        o.content.setOwned (content);
        o.dialogTitle = "Suggested mappings";
        o.dialogBackgroundColour = bgPanel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    void saveTemplate()
    {
        auto* t = selectedTarget();
        auto* def = currentProgram();
        if (t == nullptr || def == nullptr) return;
        auto* desc = targetDescription (*t);
        if (desc == nullptr) return;
        std::vector<MappingDef> mine;
        for (auto& m : def->mappings)
            if (m.slot == t->slot && m.effect == t->effect) mine.push_back (m);
        if (mine.empty()) { owner.showStatus ("No mappings on this target to save"); return; }
        engine.getPluginHost().getMappingTemplates().set (*desc, mine);
        owner.showStatus ("Saved " + String ((int) mine.size()) + " mappings as the template for " + desc->name + ". Suggest... will offer them for any " + desc->name + ".");
    }

    Engine& engine;
    MainComponent& owner;
    Label header { {}, "MIDI MAPPINGS" };
    TableListBox table;
    ComboBox targetBox, paramBox;
    std::vector<Target> targets;
    StringArray paramIds;
    TextButton touchedBtn { "Use touched parameter" }, learnBtn { "Learn MIDI" }, addBtn { "Add" }, updateBtn { "Update" }, removeBtn { "Remove" },
               suggestBtn { "Suggest..." }, templateBtn { "Save template" };
    Label sourceLabel { {}, "-" };
    Slider minSlider, maxSlider;
    ToggleButton passToggle { "Pass through" };
    MappingDef::Source learnedSource = MappingDef::Source::CC;
    int learnedNumber = 1;
    bool haveSource = false;
    int touchedSlot = -1, touchedEffect = -1, touchedParam = -1;
};

//==============================================================================
//  MainComponent
//==============================================================================
MainComponent::MainComponent (Engine& e, PropertiesFile& s, const File& initialSetup) : engine (e), settings (s)
{
    inputsPanel   = std::make_unique<InputsPanel> (engine, *this);
    programsPanel = std::make_unique<ProgramsPanel> (engine, *this);
    slotsPanel    = std::make_unique<SlotsPanel> (engine, *this);
    mappingsPanel = std::make_unique<MappingsPanel> (engine, *this);
    for (auto* c : std::initializer_list<Component*> { inputsPanel.get(), programsPanel.get(), slotsPanel.get(), mappingsPanel.get() })
        addAndMakeVisible (c);

    for (auto* b : { &newBtn, &openBtn, &saveBtn, &saveAsBtn, &audioBtn, &pluginsBtn, &midiRefreshBtn, &panicBtn })
        addAndMakeVisible (b);
    newBtn.onClick         = [this] { newSetup(); };
    openBtn.onClick        = [this] { openSetup(); };
    saveBtn.onClick        = [this] { saveSetup (false); };
    saveAsBtn.onClick      = [this] { saveSetup (true); };
    audioBtn.onClick       = [this] { showAudioSettings(); };
    pluginsBtn.onClick     = [this] { showPluginManager(); };
    midiRefreshBtn.onClick = [this] { engine.refreshMidiDevices(); showStatus ("MIDI devices rescanned"); };
    panicBtn.onClick       = [this] { engine.panic(); showStatus ("Panic: all notes off"); };
    panicBtn.setColour (TextButton::buttonColourId, Colours::darkred);

    addAndMakeVisible (preloadToggle);
    preloadToggle.setTooltip ("Keep every used program's plugins loaded so program changes are instant (uses more RAM)");
    preloadToggle.onClick = [this] { engine.setPreloadAllPrograms (preloadToggle.getToggleState()); };

    addAndMakeVisible (tailLabel);
    tailLabel.setColour (Label::textColourId, textDim);
    addAndMakeVisible (tailSlider);
    tailSlider.setRange (0.0, 20.0, 0.5);
    tailSlider.setSliderStyle (Slider::LinearBar);
    tailSlider.setTextValueSuffix (" s");
    tailSlider.setTooltip ("How long the previous program keeps sounding after a program change");
    tailSlider.onValueChange = [this] { engine.setReleaseTailSeconds (tailSlider.getValue()); };

    for (auto* l : { &statusLabel, &cpuLabel, &fileLabel })
    {
        addAndMakeVisible (l);
        l->setFont (FontOptions (12.0f));
        l->setColour (Label::textColourId, textDim);
    }
    cpuLabel.setJustificationType (Justification::centredRight);
    fileLabel.setJustificationType (Justification::centredRight);

    engine.addListener (this);

    // Open the requested setup, else restore the last one.
    File last (settings.getValue ("lastSetup"));
    if (initialSetup.existsAsFile())            loadSetupFile (initialSetup);
    else if (last.existsAsFile())               loadSetupFile (last);
    else if (getAutosaveFile().existsAsFile())  loadSetupFile (getAutosaveFile());
    else                                        refreshAll();

    startTimerHz (4);
    setSize (1280, 800);
}

MainComponent::~MainComponent()
{
    engine.removeListener (this);
    pluginWindows.clear();
}

int MainComponent::getEditedProgram() const
{
    const auto& inputs = engine.getSetup().inputs;
    if (selectedInput < 0 || selectedInput >= (int) inputs.size()) return 0;
    return inputs[(size_t) selectedInput].currentProgram;
}

void MainComponent::paint (Graphics& g)
{
    g.fillAll (bgDark);
    auto r = getLocalBounds();
    r.removeFromTop (44);
    r.removeFromBottom (24);
    g.setColour (bgPanel);
    g.fillRect (inputsPanel->getBounds());
    g.fillRect (programsPanel->getBounds());
    g.fillRect (slotsPanel->getBounds());
    g.fillRect (mappingsPanel->getBounds());
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    auto toolbar = r.removeFromTop (44).reduced (8, 8);
    for (auto* b : { &newBtn, &openBtn, &saveBtn, &saveAsBtn })
    {
        b->setBounds (toolbar.removeFromLeft (80));
        toolbar.removeFromLeft (4);
    }
    toolbar.removeFromLeft (12);
    audioBtn.setBounds (toolbar.removeFromLeft (80));       toolbar.removeFromLeft (4);
    pluginsBtn.setBounds (toolbar.removeFromLeft (80));     toolbar.removeFromLeft (4);
    midiRefreshBtn.setBounds (toolbar.removeFromLeft (100)); toolbar.removeFromLeft (12);
    panicBtn.setBounds (toolbar.removeFromRight (90));      toolbar.removeFromRight (12);
    tailSlider.setBounds (toolbar.removeFromRight (70));    toolbar.removeFromRight (2);
    tailLabel.setBounds (toolbar.removeFromRight (34));     toolbar.removeFromRight (8);
    preloadToggle.setBounds (toolbar.removeFromRight (170));

    auto status = r.removeFromBottom (24).reduced (8, 2);
    cpuLabel.setBounds (status.removeFromRight (110));
    fileLabel.setBounds (status.removeFromRight (400));
    statusLabel.setBounds (status);

    r.reduce (8, 0);
    inputsPanel->setBounds (r.removeFromLeft (260));
    r.removeFromLeft (8);
    programsPanel->setBounds (r.removeFromLeft (260));
    r.removeFromLeft (8);
    mappingsPanel->setBounds (r.removeFromBottom (jmax (200, r.getHeight() * 40 / 100)));
    r.removeFromBottom (8);
    slotsPanel->setBounds (r);
}

void MainComponent::timerCallback()
{
    cpuLabel.setText ("CPU " + String (engine.getCpuUsage() * 100.0, 1) + "%  |  " + String (engine.getSampleRate() / 1000.0, 1) + " kHz", dontSendNotification);
    if (statusText.isNotEmpty() && Time::getMillisecondCounterHiRes() - statusTime > 8000.0)
    {
        statusText.clear();
        statusLabel.setText ({}, dontSendNotification);
    }
}

void MainComponent::showStatus (const String& s)
{
    statusText = s;
    statusTime = Time::getMillisecondCounterHiRes();
    statusLabel.setText (s, dontSendNotification);
}

//==============================================================================
void MainComponent::selectInput (int idx)
{
    const auto& inputs = engine.getSetup().inputs;
    selectedInput = jlimit (0, jmax (0, (int) inputs.size() - 1), idx);
    refreshProgramView();
}

void MainComponent::refreshAll()
{
    const auto& setup = engine.getSetup();
    selectedInput = jlimit (0, jmax (0, (int) setup.inputs.size() - 1), selectedInput);
    preloadToggle.setToggleState (setup.preloadAllPrograms, dontSendNotification);
    tailSlider.setValue (setup.releaseTailSeconds, dontSendNotification);
    inputsPanel->refresh();
    inputsPanel->setSelected (selectedInput);
    refreshProgramView();
}

void MainComponent::refreshProgramView()
{
    programsPanel->refresh();
    slotsPanel->refresh();
    mappingsPanel->refresh();
}

// Engine::Listener ------------------------------------------------------------
void MainComponent::setupChanged() { refreshAll(); }

void MainComponent::programChanged (int inputIndex, int)
{
    inputsPanel->repaint();
    if (inputIndex == selectedInput) refreshProgramView();
}

void MainComponent::programContentChanged (int inputIndex, int program)
{
    if (inputIndex != selectedInput) return;
    programsPanel->repaint();
    if (program == getEditedProgram())
    {
        slotsPanel->refresh();
        mappingsPanel->refresh();
    }
}

void MainComponent::learnReceived (int, MappingDef::Source source, int number)
{
    mappingsPanel->onLearn (source, number);
}

void MainComponent::parameterTouched (int inputIndex, int program, int slot, int effect, int paramIndex)
{
    if (inputIndex == selectedInput && program == getEditedProgram())
        mappingsPanel->onTouched (slot, effect, paramIndex);
}

void MainComponent::instanceAboutToBeDeleted (AudioPluginInstance* inst) { closePluginWindowsFor (inst); }
void MainComponent::statusMessage (const String& s) { showStatus (s); }

//==============================================================================
void MainComponent::openPluginEditor (int inputIndex, int program, int slot, int effect)
{
    auto* inst = engine.getPluginInstance (inputIndex, program, slot, effect);
    if (inst == nullptr) { showStatus ("Plugin is not loaded"); return; }

    for (auto& w : pluginWindows)
        if (&w->instance == inst) { w->toFront (true); return; }

    const auto& inputs = engine.getSetup().inputs;
    String title = inst->getName();
    if (inputIndex < (int) inputs.size())
        title = inputs[(size_t) inputIndex].name + " / " + programTitle (program, inputs[(size_t) inputIndex].programs[(size_t) program]) + " / " + inst->getName();

    pluginWindows.push_back (std::make_unique<PluginWindow> (*inst, title, [this, inst]
    {
        // Defer: we're inside the window's own callback.
        MessageManager::callAsync ([this, inst] { closePluginWindowsFor (inst); });
    }));
}

void MainComponent::closePluginWindowsFor (AudioPluginInstance* inst)
{
    pluginWindows.erase (std::remove_if (pluginWindows.begin(), pluginWindows.end(),
                                         [inst] (auto& w) { return &w->instance == inst; }),
                         pluginWindows.end());
}

//==============================================================================
File MainComponent::getAutosaveFile() const
{
    return settings.getFile().getSiblingFile ("autosave.performer.json");
}

void MainComponent::setCurrentFile (const File& f)
{
    currentFile = f;
    settings.setValue ("lastSetup", f.getFullPathName());
    settings.saveIfNeeded();
    fileLabel.setText (f == File() ? "(unsaved setup)" : f.getFullPathName(), dontSendNotification);
}

void MainComponent::newSetup()
{
    pluginWindows.clear();
    engine.loadSetup (Setup::makeDefault());
    setCurrentFile ({});
    showStatus ("New setup");
}

void MainComponent::loadSetupFile (const File& f)
{
    Setup s;
    auto r = Setup::loadFromFile (f, s);
    if (r.failed())
    {
        showStatus ("Load failed: " + r.getErrorMessage());
        refreshAll();
        return;
    }
    pluginWindows.clear();
    engine.loadSetup (std::move (s));
    setCurrentFile (f == getAutosaveFile() ? File() : f);
    showStatus ("Loaded " + f.getFileName());
}

bool MainComponent::writeSetupFile (const File& f)
{
    auto r = engine.captureSetup().saveToFile (f);
    if (r.failed()) { showStatus ("Save failed: " + r.getErrorMessage()); return false; }
    return true;
}

void MainComponent::openSetup()
{
    fileChooser = std::make_unique<FileChooser> ("Open Performer setup",
                                                 currentFile.exists() ? currentFile.getParentDirectory() : File::getSpecialLocation (File::userHomeDirectory),
                                                 "*.performer.json;*.json");
    fileChooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f != File()) loadSetupFile (f);
    });
}

void MainComponent::saveSetup (bool forceAskForFile)
{
    if (! forceAskForFile && currentFile != File())
    {
        if (writeSetupFile (currentFile)) showStatus ("Saved " + currentFile.getFileName());
        return;
    }
    fileChooser = std::make_unique<FileChooser> ("Save Performer setup",
                                                 currentFile != File() ? currentFile : File::getSpecialLocation (File::userHomeDirectory).getChildFile ("MySetup.performer.json"),
                                                 "*.performer.json");
    fileChooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting, [this] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == File()) return;
        if (! f.hasFileExtension ("json")) f = f.withFileExtension ("performer.json");
        if (writeSetupFile (f)) { setCurrentFile (f); showStatus ("Saved " + f.getFileName()); }
    });
}

void MainComponent::saveOnQuit()
{
    if (currentFile != File()) writeSetupFile (currentFile);
    else                       writeSetupFile (getAutosaveFile());
}

//==============================================================================
void MainComponent::showAudioSettings()
{
    auto* selector = new AudioDeviceSelectorComponent (engine.getDeviceManager(), 0, 0, 1, 64, false, false, true, false);
    selector->setSize (520, 480);
    DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Audio Settings";
    o.dialogBackgroundColour = bgPanel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::showPluginManager()
{
    auto& host = engine.getPluginHost();
    auto* list = new PluginListComponent (host.getFormatManager(), host.getKnownPlugins(), host.getDeadMansPedalFile(), &host.getSettings(), true);
    list->setNumberOfThreadsForScanning (4);
    list->setSize (860, 560);
    DialogWindow::LaunchOptions o;
    o.content.setOwned (list);
    o.dialogTitle = "Plugins  -  use Options to scan for VST3 / LV2 / LADSPA";
    o.dialogBackgroundColour = bgPanel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

} // namespace perf
