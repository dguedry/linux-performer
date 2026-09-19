#include "PhoneControlsEditor.h"
#include "ParamPicker.h"
#include "MappingSuggestions.h"      // findParamIndex

using namespace juce;

namespace perf
{

namespace
{
    const Colour bgPanel  { 0xff26282f };
    const Colour bgRow    { 0xff2b2d35 };
    const Colour textDim  { 0xff9aa0ab };
}

//==============================================================================
/** One control: what it is, what to call it, and how to draw it. */
struct PhoneControlsEditor::Row : public Component
{
    Row (PhoneControlsEditor& o, int i) : owner (o), index (i)
    {
        addAndMakeVisible (source);
        source.setColour (Label::textColourId, textDim);
        source.setFont (FontOptions (12.0f));

        addAndMakeVisible (label);
        label.setTextToShowWhenEmpty ("(the plugin's own name)", textDim);
        label.setTooltip ("What to show on the phone. A short word fits where a plugin's own "
                          "name will not: \"Growl\" rather than \"CC 3\".");
        label.onTextChange = [this] { owner.controls[(size_t) index].label = label.getText().trim(); };
        label.onFocusLost  = [this] { owner.commit(); };

        addAndMakeVisible (widget);
        widget.addItem ("Automatic", 1);
        widget.addItem ("Fader", 2);
        widget.addItem ("Switch", 3);
        widget.setTooltip ("Automatic reads the number of positions the plugin reports, which is "
                           "right for most. Override it for a plugin that reports nothing useful.");
        widget.onChange = [this]
        {
            owner.controls[(size_t) index].widget = widget.getSelectedId() == 3 ? PhoneControl::Widget::sw
                                                 : widget.getSelectedId() == 2 ? PhoneControl::Widget::fader
                                                                               : PhoneControl::Widget::automatic;
            owner.commit();
        };

        addAndMakeVisible (up);
        addAndMakeVisible (down);
        addAndMakeVisible (remove);
        up.setTooltip ("Move earlier: the phone shows them in this order, and drawbars are "
                       "rarely in the order the plugin lists them.");
        up.onClick     = [this] { owner.move (index, index - 1); };
        down.onClick   = [this] { owner.move (index, index + 1); };
        remove.onClick = [this]
        {
            owner.controls.erase (owner.controls.begin() + index);
            owner.commit();
            owner.refresh();
        };
    }

    void update (const PhoneControl& c, const ParamInfoList& params)
    {
        // What the plugin calls it now, so a template applied to the wrong
        // library shows its mismatch here rather than only on the phone.
        const int idx = findParamIndex (params, c.paramId);
        String now;
        for (const auto& p : params) if (p.index == idx) now = p.name;

        if (idx < 0)                        source.setText (c.paramName + "  (not in this plugin)", dontSendNotification);
        else if (c.paramName.isNotEmpty() && c.paramName != now)
                                            source.setText (now + "  (template expected " + c.paramName + ")", dontSendNotification);
        else                                source.setText (now.isNotEmpty() ? now : c.paramId, dontSendNotification);

        source.setColour (Label::textColourId, idx < 0 ? Colours::orange : textDim);
        label.setText (c.label, dontSendNotification);
        widget.setSelectedId (c.widget == PhoneControl::Widget::sw ? 3
                            : c.widget == PhoneControl::Widget::fader ? 2 : 1, dontSendNotification);
    }

    void paint (Graphics& g) override { g.fillAll (index % 2 ? bgRow : bgPanel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 3);
        remove.setBounds (r.removeFromRight (26));  r.removeFromRight (4);
        down.setBounds   (r.removeFromRight (26));
        up.setBounds     (r.removeFromRight (26));  r.removeFromRight (8);
        widget.setBounds (r.removeFromRight (100)); r.removeFromRight (8);
        label.setBounds  (r.removeFromRight (160)); r.removeFromRight (8);
        source.setBounds (r);
    }

    PhoneControlsEditor& owner;
    int index;
    Label source;
    TextEditor label;
    ComboBox widget;
    TextButton up { "^" }, down { "v" }, remove { "X" };
};

//==============================================================================
class PhoneControlsEditor::RowList : public Component
{
public:
    explicit RowList (PhoneControlsEditor& o) : owner (o) {}

    void rebuild (const std::vector<PhoneControl>& controls, const ParamInfoList& params)
    {
        rows.clear();
        for (int i = 0; i < (int) controls.size(); ++i)
        {
            auto* row = rows.add (new Row (owner, i));
            addAndMakeVisible (row);
            row->update (controls[(size_t) i], params);
        }
        setSize (getWidth(), jmax (1, (int) controls.size()) * kRowHeight);
        resized();
    }

    void resized() override
    {
        int y = 0;
        for (auto* r : rows) { r->setBounds (0, y, getWidth(), kRowHeight); y += kRowHeight; }
    }

    void paint (Graphics& g) override
    {
        if (! rows.isEmpty()) return;
        g.setColour (textDim);
        g.setFont (FontOptions (13.5f));
        g.drawFittedText ("No controls yet. Press \"Add controls...\" to pick some, or apply a "
                          "template someone made for this plugin.",
                          getLocalBounds().reduced (20, 10), Justification::centredTop, 3);
    }

    static constexpr int kRowHeight = 30;

private:
    PhoneControlsEditor& owner;
    OwnedArray<Row> rows;
};

//==============================================================================
PhoneControlsEditor::PhoneControlsEditor (Engine& e, PhoneTemplates& t, int in, int prog, int sl)
    : engine (e), templates (t), inputIndex (in), program (prog), slot (sl)
{
    controls = engine.getSlotPhoneControls (inputIndex, program, slot);

    addAndMakeVisible (title);
    title.setFont (FontOptions (15.0f, Font::bold));
    title.setColour (Label::textColourId, Colours::white);
    title.setText (pluginName() + " -- phone controls", dontSendNotification);

    addAndMakeVisible (addBtn);
    addBtn.onClick = [this] { addControls(); };

    addAndMakeVisible (templateBtn);
    templateBtn.onClick = [this]
    {
        PopupMenu m;
        m.addSectionHeader ("Use a template");
        const auto mine = templates.forPlugin (engine.getSetup().inputs[(size_t) inputIndex]
                                                   .programs[(size_t) program].slots[(size_t) slot].plugin);
        if (mine.empty()) m.addItem (-1, "None for this plugin yet", false);
        else              m.addItem (1, "Apply a template...");
        m.addItem (2, "Import a template file...");

        m.addSeparator();
        m.addSectionHeader ("Share these controls");
        m.addItem (3, "Save as a template...", ! controls.empty());
        m.addItem (4, "Export to a file...", ! controls.empty());

        m.showMenuAsync (PopupMenu::Options().withTargetComponent (templateBtn), [this] (int c)
        {
            if (c == 1) applyTemplate();
            else if (c == 2) importTemplate();
            else if (c == 3) saveAsTemplate();
            else if (c == 4) exportTemplate();
        });
    };

    addAndMakeVisible (closeBtn);
    closeBtn.onClick = [this]
    {
        commit();
        if (auto* dw = findParentComponentOfClass<DialogWindow>()) dw->exitModalState (0);
    };

    list = std::make_unique<RowList> (*this);
    addAndMakeVisible (list.get());
    refresh();
    setSize (720, 460);
}

PhoneControlsEditor::~PhoneControlsEditor() { commit(); }

String PhoneControlsEditor::pluginName() const
{
    const auto& setup = engine.getSetup();
    if (inputIndex < 0 || inputIndex >= (int) setup.inputs.size()) return {};
    const auto& slots = setup.inputs[(size_t) inputIndex].programs[(size_t) program].slots;
    if (slot < 0 || slot >= (int) slots.size()) return {};
    return slots[(size_t) slot].plugin.name;
}

ParamInfoList PhoneControlsEditor::pluginParams() const
{
    if (auto* p = engine.getPlugin (inputIndex, program, slot, -1))
        return p->getParameters();
    return {};
}

void PhoneControlsEditor::refresh()
{
    list->setSize (getWidth() - 24, 10);
    list->rebuild (controls, pluginParams());
    resized();
    repaint();
}

void PhoneControlsEditor::commit()
{
    engine.setSlotPhoneControls (inputIndex, program, slot, controls);
}

void PhoneControlsEditor::move (int from, int to)
{
    if (from < 0 || from >= (int) controls.size() || to < 0 || to >= (int) controls.size()) return;
    std::swap (controls[(size_t) from], controls[(size_t) to]);
    commit();
    refresh();
}

//==============================================================================
void PhoneControlsEditor::addControls()
{
    const auto params = pluginParams();
    if (params.empty())
    {
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "Phone controls",
                                          "This plugin is not loaded, so its parameters are not "
                                          "available yet. Select the program it is in, or turn on "
                                          "\"Preload all programs\".", "OK");
        return;
    }

    /* The same picker the mappings panel uses, so the two agree about what is
       worth showing and what is a per-channel duplicate -- Kontakt's 4145
       parameters are unusable without that filtering.

       It stays open while you add: choosing controls one at a time through a
       dialog that closes after each would be tedious for a dozen of them. */
    auto* content = new Component();
    content->setSize (460, 420);

    auto* picker = new ParamPicker();
    for (int i = 0; i < (int) params.size(); ++i)
        picker->addItem (params[(size_t) i], i + 1);          // ids are 1-based
    picker->setPreferredChannel (engine.getSetup().inputs[(size_t) inputIndex].channel);
    picker->setBounds (0, 0, 460, 420);
    content->addAndMakeVisible (picker);

    picker->onChange = [this, picker, params]
    {
        const int id = picker->getSelectedId();
        if (id <= 0 || id > (int) params.size()) return;
        const auto& p = params[(size_t) (id - 1)];

        const bool already = std::any_of (controls.begin(), controls.end(),
                                          [&] (const PhoneControl& c) { return c.paramId == p.id; });
        if (! already)
        {
            controls.push_back ({ p.id, p.name, {}, PhoneControl::Widget::automatic });
            commit();
            refresh();
        }
    };

    DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = "Add a control";
    o.dialogBackgroundColour = Colour (0xff1e1f24);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.launchAsync();
}

//==============================================================================
void PhoneControlsEditor::applyTemplate()
{
    const auto& slotDef = engine.getSetup().inputs[(size_t) inputIndex]
                              .programs[(size_t) program].slots[(size_t) slot];
    const auto mine = templates.forPlugin (slotDef.plugin);
    if (mine.empty()) return;

    PopupMenu m;
    for (int i = 0; i < (int) mine.size(); ++i)
        m.addItem (i + 1, mine[(size_t) i].name);

    m.showMenuAsync (PopupMenu::Options().withTargetComponent (templateBtn), [this, mine] (int choice)
    {
        if (choice <= 0 || choice > (int) mine.size()) return;
        const auto& t = mine[(size_t) choice - 1];

        /* Say how well it fits before replacing anything. Nothing is detected
           automatically -- two instances of a plugin look identical to the host
           -- so this is the only warning that the Rhodes template is about to
           land on a drum kit. */
        const auto fit = t.checkAgainst (pluginParams());
        String message = t.name + "\n\n" + fit.summary();
        if (t.notes.isNotEmpty()) message += "\n\n" + t.notes;
        if (! controls.empty())   message += "\n\nThis replaces the " + String (controls.size())
                                           + " control(s) already here.";

        AlertWindow::showOkCancelBox (MessageBoxIconType::QuestionIcon, "Apply template",
                                      message, "Apply", "Cancel", nullptr,
                                      ModalCallbackFunction::create ([this, t] (int ok)
        {
            if (ok == 0) return;
            controls = t.controls;
            commit();
            refresh();
        }));
    });
}

void PhoneControlsEditor::saveAsTemplate()
{
    auto* w = new AlertWindow ("Save as a template",
                               "A name others will recognise -- usually the library or preset "
                               "these controls are for, not the plugin.",
                               MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", pluginName());
    w->addTextEditor ("notes", {}, "Notes (optional)");
    w->addButton ("Save", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));

    w->enterModalState (true, ModalCallbackFunction::create ([this, w] (int r)
    {
        std::unique_ptr<AlertWindow> owned (w);
        if (r == 0) return;
        const auto name = w->getTextEditorContents ("name").trim();
        if (name.isEmpty()) return;

        PhoneTemplate t;
        t.name = name;
        t.notes = w->getTextEditorContents ("notes").trim();
        t.controls = controls;

        const auto& slotDef = engine.getSetup().inputs[(size_t) inputIndex]
                                  .programs[(size_t) program].slots[(size_t) slot];
        t.pluginName = slotDef.plugin.name;
        t.pluginKey  = slotDef.plugin.createIdentifierString();

        // Record what the plugin calls each parameter now, so applying this
        // elsewhere can report a mismatch instead of quietly being wrong.
        const auto params = pluginParams();
        for (auto& c : t.controls)
        {
            const int idx = findParamIndex (params, c.paramId);
            for (const auto& p : params)
                if (p.index == idx) c.paramName = p.name;
        }

        templates.put (t);
    }), false);
}

void PhoneControlsEditor::exportTemplate()
{
    const auto& slotDef = engine.getSetup().inputs[(size_t) inputIndex]
                              .programs[(size_t) program].slots[(size_t) slot];

    PhoneTemplate t;
    t.name = pluginName();
    t.pluginName = slotDef.plugin.name;
    t.pluginKey  = slotDef.plugin.createIdentifierString();
    t.controls = controls;

    const auto params = pluginParams();
    for (auto& c : t.controls)
    {
        const int idx = findParamIndex (params, c.paramId);
        for (const auto& p : params)
            if (p.index == idx) c.paramName = p.name;
    }

    chooser = std::make_unique<FileChooser> ("Export template",
                                             File::getSpecialLocation (File::userDocumentsDirectory)
                                                 .getChildFile (t.name + ".performer-template.json"),
                                             "*.json");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting,
                          [t] (const FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f == File()) return;
        if (const auto r = PhoneTemplates::exportToFile (t, f); r.failed())
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Export", r.getErrorMessage(), "OK");
    });
}

void PhoneControlsEditor::importTemplate()
{
    chooser = std::make_unique<FileChooser> ("Import template",
                                             File::getSpecialLocation (File::userDocumentsDirectory),
                                             "*.json");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                          [this] (const FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f == File()) return;

        PhoneTemplate t;
        if (const auto r = PhoneTemplates::importFromFile (f, t); r.failed())
        {
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Import", r.getErrorMessage(), "OK");
            return;
        }

        templates.put (t);

        const auto fit = t.checkAgainst (pluginParams());
        const auto& slotDef = engine.getSetup().inputs[(size_t) inputIndex]
                                  .programs[(size_t) program].slots[(size_t) slot];
        String message = t.name;
        if (t.pluginName.isNotEmpty() && t.pluginName != slotDef.plugin.name)
            message += "\n\nMade for " + t.pluginName + ", and this slot holds "
                     + slotDef.plugin.name + ".";
        message += "\n\n" + fit.summary();
        if (t.notes.isNotEmpty()) message += "\n\n" + t.notes;
        message += "\n\nApply it to this slot now?";

        AlertWindow::showOkCancelBox (MessageBoxIconType::QuestionIcon, "Imported", message,
                                      "Apply", "Just keep it", nullptr,
                                      ModalCallbackFunction::create ([this, t] (int ok)
        {
            if (ok == 0) return;
            controls = t.controls;
            commit();
            refresh();
        }));
    });
}

//==============================================================================
void PhoneControlsEditor::paint (Graphics& g)
{
    g.fillAll (Colour (0xff1e1f24));

    /* Headings line up with the row layout below: label at 160 wide from the
       right, then the widget box at 100, then three small buttons. */
    g.setColour (textDim);
    g.setFont (FontOptions (11.0f, Font::bold));
    const int y = 52;
    g.drawFittedText ("PARAMETER", 18, y, 300, 14, Justification::centredLeft, 1);
    g.drawFittedText ("LABEL ON THE PHONE", getWidth() - 12 - 6 - 94 - 8 - 100 - 8 - 160, y, 160, 14,
                      Justification::centredLeft, 1);
    g.drawFittedText ("SHOWN AS", getWidth() - 12 - 6 - 94 - 8 - 100, y, 100, 14,
                      Justification::centredLeft, 1);
}

void PhoneControlsEditor::resized()
{
    auto r = getLocalBounds().reduced (12);

    auto top = r.removeFromTop (28);
    closeBtn.setBounds (top.removeFromRight (80));   top.removeFromRight (6);
    templateBtn.setBounds (top.removeFromRight (110)); top.removeFromRight (6);
    addBtn.setBounds (top.removeFromRight (120));    top.removeFromRight (10);
    title.setBounds (top);

    r.removeFromTop (34);                 // room for the column headings
    list->setSize (r.getWidth(), list->getHeight());
    list->setTopLeftPosition (r.getX(), r.getY());
}

//==============================================================================
void PhoneControlsEditor::show (Engine& e, PhoneTemplates& t, int in, int prog, int sl, Colour background)
{
    auto* content = new PhoneControlsEditor (e, t, in, prog, sl);

    DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = "Phone controls";
    o.dialogBackgroundColour = background;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

} // namespace perf
