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
            filter();
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
            count.setBounds (top.removeFromRight (90));
            search.setBounds (top);
            r.removeFromTop (4);
            list.setBounds (r);
        }

        void filter()
        {
            rows.clearQuick();
            juce::StringArray words;
            words.addTokens (search.getText().trim().toLowerCase(), true);
            for (int i = 0; i < owner.names.size(); ++i)
            {
                const auto hay = owner.names[i].toLowerCase() + " #" + juce::String (i);
                bool ok = true;
                for (auto& w : words) if (! hay.contains (w)) { ok = false; break; }
                if (ok) rows.add (i);
            }
            list.updateContent();
            count.setText (juce::String (rows.size()) + " / " + juce::String (owner.names.size()), juce::dontSendNotification);
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

        ParamPicker& owner;
        SearchBox search;
        juce::ListBox list;
        juce::Label count;
        juce::Array<int> rows;     // indices into owner.names that match the filter
    };

    void showPopup()
    {
        auto content = std::make_unique<Popup> (*this);
        content->setSize (juce::jmax (380, getWidth()), 360);
        juce::CallOutBox::launchAsynchronously (std::move (content), getScreenBounds(), nullptr);
    }

    juce::StringArray names;
    juce::Array<int> ids;
    int selected = 0;
    juce::String placeholder;
};

} // namespace perf
