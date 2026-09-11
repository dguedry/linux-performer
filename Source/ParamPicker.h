#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace perf
{

/** Drop-in replacement for a ComboBox that lists a plugin's parameters.

    Plugins such as Kontakt expose thousands of parameters; a PopupMenu that
    long builds a component per item and never finishes. This shows a field
    that looks like a combo box and opens a searchable list instead: type to
    filter, arrows to move, Return or click to pick, Escape to cancel. The
    subset of the ComboBox API the mapping panel uses is kept. */
class ParamPicker : public juce::Component
{
public:
    std::function<void()> onChange;

    void addItem (const juce::String& text, int id)          { names.add (text); ids.add (id); repaint(); }
    int getNumItems() const                                  { return names.size(); }
    juce::String getItemText (int index) const               { return names[index]; }
    int getSelectedId() const                                { return selected; }

    void clear (juce::NotificationType n = juce::sendNotificationAsync)
    {
        names.clear(); ids.clear();
        const bool had = selected != 0;
        selected = 0;
        repaint();
        if (had) notify (n);
    }

    void setSelectedId (int id, juce::NotificationType n = juce::sendNotificationAsync)
    {
        const int newId = ids.contains (id) ? id : 0;
        if (newId == selected) return;
        selected = newId;
        repaint();
        notify (n);
    }

    void setTextWhenNothingSelected (const juce::String& t)   { placeholder = t; repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (findColour (isEnabled() ? juce::ComboBox::outlineColourId : juce::ComboBox::outlineColourId).withAlpha (isEnabled() ? 1.0f : 0.5f));
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        const int idx = ids.indexOf (selected);
        const bool has = idx >= 0;
        g.setColour (findColour (juce::ComboBox::textColourId).withAlpha (has && isEnabled() ? 1.0f : 0.5f));
        g.setFont (juce::FontOptions ((float) getHeight() * 0.6f));
        g.drawText (has ? names[idx] : placeholder, getLocalBounds().withTrimmedLeft (8).withTrimmedRight (24),
                    juce::Justification::centredLeft, true);

        // arrow, like the default combo box
        juce::Path p;
        const float ax = (float) getWidth() - 14.0f, ay = (float) getHeight() * 0.5f;
        p.addTriangle (ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
        g.setColour (findColour (juce::ComboBox::arrowColourId).withAlpha (isEnabled() ? 1.0f : 0.4f));
        g.fillPath (p);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (isEnabled() && names.size() > 0)
            showPopup();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::returnKey || k == juce::KeyPress::spaceKey || k == juce::KeyPress::downKey)
        {
            if (isEnabled() && names.size() > 0) showPopup();
            return true;
        }
        return false;
    }

private:
    void notify (juce::NotificationType n)
    {
        if (n == juce::dontSendNotification || ! onChange) return;
        if (n == juce::sendNotificationSync) onChange();
        else juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<ParamPicker> (this)] { if (sp != nullptr && sp->onChange) sp->onChange(); });
    }

    /** Search box whose arrow/page keys drive the list instead of the caret. */
    struct SearchBox : public juce::TextEditor
    {
        std::function<bool (const juce::KeyPress&)> onNavigate;
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (onNavigate && (k == juce::KeyPress::upKey || k == juce::KeyPress::downKey
                               || k == juce::KeyPress::pageUpKey || k == juce::KeyPress::pageDownKey))
                return onNavigate (k);
            return juce::TextEditor::keyPressed (k);
        }
    };

    struct Popup : public juce::Component, private juce::ListBoxModel, private juce::TextEditor::Listener
    {
        explicit Popup (ParamPicker& o) : owner (o)
        {
            addAndMakeVisible (search);
            search.setTextToShowWhenEmpty ("Search " + juce::String (owner.names.size()) + " parameters...", juce::Colours::grey);
            search.setEscapeAndReturnKeysConsumed (true);
            search.setSelectAllWhenFocused (true);
            search.addListener (this);
            search.onNavigate = [this] (const juce::KeyPress& k) { return navigate (k); };

            addAndMakeVisible (list);
            list.setModel (this);
            list.setRowHeight (22);
            list.setColour (juce::ListBox::backgroundColourId, owner.findColour (juce::ComboBox::backgroundColourId));
            addAndMakeVisible (count);
            count.setFont (juce::FontOptions (11.0f));
            count.setColour (juce::Label::textColourId, juce::Colours::grey);
            count.setJustificationType (juce::Justification::centredRight);

            // VST3 plugins have no MIDI CC input; instead they publish one parameter
            // per controller per MIDI channel (Kontakt: 64 channels x every controller),
            // which shows up as long runs of identically named parameters. Those are
            // MIDI plumbing, not controls, so hide them unless asked.
            const int n = owner.names.size();
            for (int i = 0; i < n;)
            {
                int j = i + 1;
                while (j < n && owner.names[j] == owner.names[i]) ++j;
                if (j - i >= kRunLength)
                    for (int k = i; k < j; ++k) controllerCopy.setBit (k);
                i = j;
            }
            numHidden = controllerCopy.countNumberOfSetBits();
            if (numHidden > 0)
            {
                addAndMakeVisible (showAll);
                showAll.setButtonText ("Show MIDI controller parameters (" + juce::String (numHidden) + " copies of "
                                       + juce::String (countRuns()) + " names)");
                showAll.setTooltip ("Runs of identically named parameters are VST3's per-channel MIDI controller proxies. Performer already forwards CCs to the plugin, so these are rarely what you want to map.");
                showAll.setToggleState (showControllerParams, juce::dontSendNotification);
                showAll.onClick = [this] { showControllerParams = showAll.getToggleState(); filter(); };
            }
            filter();
        }

        int countRuns() const
        {
            int runs = 0;
            for (int i = 0; i < owner.names.size(); ++i)
                if (controllerCopy[i] && (i == 0 || ! controllerCopy[i - 1] || owner.names[i] != owner.names[i - 1]))
                    ++runs;
            return runs;
        }

        void parentHierarchyChanged() override
        {
            if (isShowing())
                juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<Popup> (this)] { if (sp != nullptr) sp->search.grabKeyboardFocus(); });
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (6);
            auto top = r.removeFromTop (26);
            count.setBounds (top.removeFromRight (130));
            search.setBounds (top);
            r.removeFromTop (4);
            if (numHidden > 0)
                showAll.setBounds (r.removeFromBottom (22));
            list.setBounds (r);
        }

        void filter()
        {
            rows.clearQuick();
            juce::StringArray words;
            words.addTokens (search.getText().trim().toLowerCase(), true);
            const int selectedIndex = owner.ids.indexOf (owner.selected);
            int hiddenMatches = 0;
            for (int i = 0; i < owner.names.size(); ++i)
            {
                const auto hay = owner.names[i].toLowerCase() + " #" + juce::String (i);
                bool ok = true;
                for (auto& w : words) if (! hay.contains (w)) { ok = false; break; }
                if (! ok) continue;
                if (controllerCopy[i] && ! showControllerParams && i != selectedIndex)
                    ++hiddenMatches;    // hidden, unless it is the current selection
                else
                    rows.add (i);
            }
            list.updateContent();
            count.setText (juce::String (rows.size()) + " / " + juce::String (owner.names.size())
                           + (hiddenMatches > 0 ? " (+" + juce::String (hiddenMatches) + " hidden)" : juce::String()), juce::dontSendNotification);
            const int cur = rows.indexOf (owner.ids.indexOf (owner.selected));
            list.selectRow (cur >= 0 ? cur : (rows.isEmpty() ? -1 : 0));
            if (cur >= 0) list.scrollToEnsureRowIsOnscreen (cur);
        }

        bool navigate (const juce::KeyPress& k)
        {
            if (rows.isEmpty()) return true;
            const int visible = juce::jmax (1, list.getHeight() / list.getRowHeight());
            int row = juce::jmax (0, list.getSelectedRow());
            if (k == juce::KeyPress::upKey)            row = juce::jmax (0, row - 1);
            else if (k == juce::KeyPress::downKey)     row = juce::jmin (rows.size() - 1, row + 1);
            else if (k == juce::KeyPress::pageUpKey)   row = juce::jmax (0, row - visible);
            else if (k == juce::KeyPress::pageDownKey) row = juce::jmin (rows.size() - 1, row + visible);
            list.selectRow (row);
            return true;
        }

        void choose (int row)
        {
            if (row < 0 || row >= rows.size()) return;
            owner.setSelectedId (owner.ids[rows[row]], juce::sendNotificationSync);
            dismiss();
        }

        void dismiss()
        {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();
        }

        // ListBoxModel
        int getNumRows() override { return rows.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
        {
            if (row < 0 || row >= rows.size()) return;
            if (sel) { g.setColour (owner.findColour (juce::TextEditor::highlightColourId).withAlpha (0.6f)); g.fillRect (0, 0, w, h); }
            const int i = rows[row];
            g.setFont (juce::FontOptions ((float) h * 0.6f));
            g.setColour (owner.findColour (juce::ComboBox::textColourId));
            g.drawText (owner.names[i], 8, 0, w - 70, h, juce::Justification::centredLeft, true);
            g.setColour (juce::Colours::grey);
            g.drawText ("#" + juce::String (i), w - 62, 0, 56, h, juce::Justification::centredRight, false);
        }
        void listBoxItemClicked (int row, const juce::MouseEvent&) override { choose (row); }
        void returnKeyPressed (int row) override                             { choose (row); }

        // TextEditor::Listener
        void textEditorTextChanged (juce::TextEditor&) override      { filter(); }
        void textEditorReturnKeyPressed (juce::TextEditor&) override { choose (list.getSelectedRow() >= 0 ? list.getSelectedRow() : 0); }
        void textEditorEscapeKeyPressed (juce::TextEditor&) override { dismiss(); }

        static constexpr int kRunLength = 8;

        ParamPicker& owner;
        SearchBox search;
        juce::ListBox list;
        juce::Label count;
        juce::ToggleButton showAll;
        juce::BigInteger controllerCopy;   // parameter indices that sit in a run of identical names
        int numHidden = 0;
        juce::Array<int> rows;     // indices into owner.names that match the filter
    };

    /** Remembered for the session, so the choice sticks between pickers. */
    static inline bool showControllerParams = false;

    void showPopup()
    {
        auto content = std::make_unique<Popup> (*this);
        content->setSize (juce::jmax (420, getWidth()), 380);
        juce::CallOutBox::launchAsynchronously (std::move (content), getScreenBounds(), nullptr);
    }

    juce::StringArray names;
    juce::Array<int> ids;
    int selected = 0;
    juce::String placeholder;
};

} // namespace perf
