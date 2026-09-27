#include "SongsPanel.h"

namespace perf
{

using namespace juce;

namespace
{
    const Colour bgPanel   { 0xff1b1d22 };
    const Colour bgRow     { 0xff23262d };
    const Colour accent    { 0xff4f9dff };
    const Colour accentDim { 0xff2b4f80 };
    const Colour textDim   { 0xff9aa0ab };
}

//==============================================================================
SongsPanel::SongsPanel (Engine& e) : engine (e)
{
    for (auto* l : { &songsHeader, &setHeader })
    {
        addAndMakeVisible (l);
        l->setColour (Label::textColourId, textDim);
        l->setFont (FontOptions (11.0f));
    }

    songsList.setModel (&songsModel);
    setList.setModel (&setModel);
    for (auto* b : { &songsList, &setList })
    {
        addAndMakeVisible (b);
        b->setRowHeight (34);          // two lines: the name, and what it loads
        b->setColour (ListBox::backgroundColourId, bgRow);
    }

    addAndMakeVisible (addBtn);
    addBtn.setTooltip ("A new song, ready to capture what is loaded now");
    addBtn.onClick = [this] { addSong(); };

    addAndMakeVisible (removeBtn);
    removeBtn.setTooltip ("Delete the selected song, and take it out of every set that used it");
    removeBtn.onClick = [this] { removeSong(); };

    addAndMakeVisible (captureBtn);
    captureBtn.setColour (TextButton::buttonColourId, accentDim);
    captureBtn.setTooltip ("Store what every keyboard is playing right now, and the tempo, in the selected song");
    captureBtn.onClick = [this] { captureIntoSelectedSong(); };

    addAndMakeVisible (editBtn);
    editBtn.setTooltip ("Name, stage notes and tempo for this song");
    editBtn.onClick = [this] { editSelectedSong(); };

    addAndMakeVisible (toSetBtn);
    toSetBtn.setTooltip ("Put the selected song at the end of the set");
    toSetBtn.onClick = [this] { addToSet(); };

    addAndMakeVisible (fromSetBtn);
    fromSetBtn.setTooltip ("Take this song out of the set. The song itself is kept.");
    fromSetBtn.onClick = [this] { removeFromSet(); };

    addAndMakeVisible (songUpBtn);
    songUpBtn.setTooltip ("Move this song up the list. Sets keep playing the same order.");
    songUpBtn.onClick = [this] { moveSongInList (-1); };
    addAndMakeVisible (songDownBtn);
    songDownBtn.setTooltip ("Move this song down the list");
    songDownBtn.onClick = [this] { moveSongInList (1); };

    addAndMakeVisible (upBtn);
    upBtn.onClick = [this] { moveInSet (-1); };
    addAndMakeVisible (downBtn);
    downBtn.onClick = [this] { moveInSet (1); };

    addAndMakeVisible (setBtn);
    setBtn.setTooltip ("Choose which set you are playing, or make a new one");
    setBtn.onClick = [this] { showSetMenu(); };

    addAndMakeVisible (footBtn);
    footBtn.setTooltip ("Pedals that step to the next and previous song");
    footBtn.onClick = [this] { showFootswitchMenu(); };

    /* The set can move without anyone touching this panel -- a footswitch, the
       tablet, a program change. Polling rather than listening keeps this panel
       out of the Engine's listener list, which is already long, and ten times a
       second is plenty for a list of songs. */
    startTimerHz (10);
    refresh();
}

SongsPanel::~SongsPanel()
{
    songsList.setModel (nullptr);
    setList.setModel (nullptr);
}

//==============================================================================
void SongsPanel::paint (Graphics& g)
{
    g.setColour (bgPanel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
}

void SongsPanel::resized()
{
    auto r = getLocalBounds().reduced (6);

    auto top = r.removeFromTop (24);
    setBtn.setBounds (top.removeFromLeft (top.getWidth() / 2).reduced (1, 0));
    footBtn.setBounds (top.reduced (1, 0));
    r.removeFromTop (6);

    // The set list is what you play from, so it gets the larger half.
    auto songsArea = r.removeFromTop (r.getHeight() * 45 / 100);
    r.removeFromTop (6);

    songsHeader.setBounds (songsArea.removeFromTop (14));
    auto songButtons = songsArea.removeFromBottom (26);
    songsArea.removeFromBottom (4);
    auto songMoveCol = songsArea.removeFromRight (28);
    songUpBtn.setBounds (songMoveCol.removeFromTop (26).reduced (1));
    songMoveCol.removeFromTop (2);
    songDownBtn.setBounds (songMoveCol.removeFromTop (26).reduced (1));
    songsArea.removeFromRight (4);
    songsList.setBounds (songsArea);

    auto half = songButtons.removeFromLeft (songButtons.getWidth() / 2);
    addBtn.setBounds (half.removeFromLeft (half.getWidth() / 2).reduced (1, 0));
    removeBtn.setBounds (half.reduced (1, 0));
    captureBtn.setBounds (songButtons.removeFromLeft (songButtons.getWidth() / 2).reduced (1, 0));
    editBtn.setBounds (songButtons.reduced (1, 0));

    setHeader.setBounds (r.removeFromTop (14));
    auto setButtons = r.removeFromBottom (26);
    r.removeFromBottom (4);
    auto moveCol = r.removeFromRight (28);
    upBtn.setBounds (moveCol.removeFromTop (26).reduced (1));
    moveCol.removeFromTop (2);
    downBtn.setBounds (moveCol.removeFromTop (26).reduced (1));
    r.removeFromRight (4);
    setList.setBounds (r);

    toSetBtn.setBounds (setButtons.removeFromLeft (setButtons.getWidth() * 60 / 100).reduced (1, 0));
    fromSetBtn.setBounds (setButtons.reduced (1, 0));
}

//==============================================================================
void SongsPanel::refresh()
{
    const auto& songs = engine.getSongs();
    const auto& sets  = engine.getSets();
    const int set = engine.getCurrentSet();

    songsHeader.setText (songs.empty() ? "SONGS  (a program per keyboard, and a tempo)"
                                      : "SONGS  (" + String ((int) songs.size()) + ")",
                         dontSendNotification);

    if (set >= 0 && set < (int) sets.size())
    {
        const auto& st = sets[(size_t) set];
        setHeader.setText ("SET: " + st.name.toUpperCase() + "  ("
                             + String ((int) st.songs.size()) + ")", dontSendNotification);
    }
    else
    {
        setHeader.setText (sets.empty() ? "SET  (none yet)" : "SET  (none chosen)", dontSendNotification);
    }

    const bool haveSong = selectedSong >= 0 && selectedSong < (int) songs.size();
    const bool haveSet  = set >= 0 && set < (int) sets.size();
    removeBtn.setEnabled (haveSong);
    captureBtn.setEnabled (haveSong);
    editBtn.setEnabled (haveSong);
    toSetBtn.setEnabled (haveSong && haveSet);
    fromSetBtn.setEnabled (haveSet && selectedInSet >= 0);
    songUpBtn.setEnabled (haveSong && selectedSong > 0);
    songDownBtn.setEnabled (haveSong && selectedSong < (int) songs.size() - 1);
    upBtn.setEnabled (haveSet && selectedInSet > 0);
    downBtn.setEnabled (haveSet && selectedInSet >= 0
                        && selectedInSet < (int) sets[(size_t) set].songs.size() - 1);

    songsList.updateContent();
    setList.updateContent();
    songsList.repaint();
    setList.repaint();
}

void SongsPanel::timerCallback()
{
    /* Follow the set moving under us. Only the highlight and the headers can
       change without a click here, so this is cheap. */
    static int lastSet = -2, lastPos = -2, lastSongs = -1, lastSets = -1;
    const int set = engine.getCurrentSet(), pos = engine.getCurrentSongInSet();
    const int nSongs = (int) engine.getSongs().size(), nSets = (int) engine.getSets().size();
    if (set != lastSet || pos != lastPos || nSongs != lastSongs || nSets != lastSets)
    {
        lastSet = set; lastPos = pos; lastSongs = nSongs; lastSets = nSets;
        refresh();
    }
}

//==============================================================================
int SongsPanel::getNumRows() { return 0; }
void SongsPanel::paintListBoxItem (int, Graphics&, int, int, bool) {}
void SongsPanel::listBoxItemClicked (int, const MouseEvent&) {}
void SongsPanel::listBoxItemDoubleClicked (int, const MouseEvent&) {}

int SongsPanel::ListProxy::getNumRows()
{
    if (! isSet) return (int) owner.engine.getSongs().size();
    const int set = owner.engine.getCurrentSet();
    const auto& sets = owner.engine.getSets();
    return set >= 0 && set < (int) sets.size() ? (int) sets[(size_t) set].songs.size() : 0;
}

void SongsPanel::ListProxy::paintListBoxItem (int row, Graphics& g, int w, int h, bool selected)
{
    const auto& songs = owner.engine.getSongs();

    int songIndex = -1;
    bool playing = false;
    String prefix;

    if (! isSet)
    {
        songIndex = row;
    }
    else
    {
        songIndex = owner.songAtSetPosition (row);
        playing = row == owner.engine.getCurrentSongInSet();
        prefix = String (row + 1) + ".  ";
    }
    if (songIndex < 0 || songIndex >= (int) songs.size()) return;
    const auto& sg = songs[(size_t) songIndex];

    // The song being played is the one that has to be findable at a glance.
    g.fillAll (playing ? accentDim : (selected ? Colour (0xff2c3038) : bgRow));

    auto r = Rectangle<int> (0, 0, w, h).reduced (6, 0);


    /* Two lines: the name, and which program each keyboard plays. The second
       line is the answer to "what is in this song" -- without it the panel
       looks like a list of labels rather than a list of rigs. */
    auto nameRow = r.removeFromTop (r.getHeight() / 2 + 1);

    /* The tempo sits beside the name, not beside the programs: the second line
       needs the whole width, and a truncated "001 ..." tells you nothing. */
    if (sg.tempoBpm > 0.0)
    {
        g.setColour (playing ? Colours::white.withAlpha (0.85f) : textDim);
        g.setFont (FontOptions (11.0f));
        g.drawText (String (roundToInt (sg.tempoBpm)) + " bpm",
                    nameRow.removeFromRight (52), Justification::centredRight);
    }

    g.setColour (playing ? Colours::white : Colours::white.withAlpha (0.92f));
    g.setFont (FontOptions (13.0f, playing ? Font::bold : Font::plain));
    g.drawText (prefix + (sg.name.isNotEmpty() ? sg.name : "(unnamed)"),
                nameRow, Justification::centredLeft, true);

    const auto& inputs = owner.engine.getSetup().inputs;
    StringArray parts;
    for (int i = 0; i < (int) inputs.size(); ++i)
    {
        const int prog = sg.programFor (i);
        if (prog < 0) continue;              // this song leaves that keyboard alone
        const auto& pd = inputs[(size_t) i].programs[(size_t) prog];
        parts.add (String (prog).paddedLeft ('0', 3)
                     + (pd.name.isNotEmpty() ? " " + pd.name : ""));
    }
    g.setColour (playing ? Colours::white.withAlpha (0.8f) : textDim);
    g.setFont (FontOptions (10.5f));
    g.drawText (parts.isEmpty() ? "(nothing captured)" : parts.joinIntoString ("   "),
                r, Justification::centredLeft, true);
}

void SongsPanel::ListProxy::listBoxItemClicked (int row, const MouseEvent&)
{
    /* One click loads it. That is what a tap does on the tablet and what a
       click does in the programs list, and a set list whose songs need
       double-clicking would be the odd one out -- on stage, twice is a miss.

       It also selects, so Capture, Rename and Add to set act on what you just
       clicked without a second step. */
    if (isSet) owner.selectedInSet = row;
    else       owner.selectedSong  = row;
    play (row);
}

void SongsPanel::ListProxy::play (int row)
{
    /* In the set that means "start here", which also moves the position a
       footswitch will step from; in the song list it is just "load this",
       without touching the set. */
    if (isSet) owner.engine.selectSongInSet (row);
    else       owner.engine.selectSong (row);
    owner.refresh();
}

void SongsPanel::ListProxy::listBoxItemDoubleClicked (int row, const MouseEvent&) { play (row); }
void SongsPanel::ListProxy::returnKeyPressed (int row)                            { play (row); }

int SongsPanel::songAtSetPosition (int position) const
{
    const int set = engine.getCurrentSet();
    const auto& sets = engine.getSets();
    if (set < 0 || set >= (int) sets.size()) return -1;
    const auto& v = sets[(size_t) set].songs;
    return position >= 0 && position < (int) v.size() ? v[(size_t) position] : -1;
}

//==============================================================================
String SongsPanel::describeCurrent() const
{
    /* What is loaded right now, in the same words the song row will use. Shown
       while naming so it is obvious what is about to be stored -- the answer to
       "what is a song?" is easier to see than to read. */
    const auto& inputs = engine.getSetup().inputs;
    StringArray parts;
    for (const auto& in : inputs)
    {
        const auto& prog = in.programs[(size_t) in.currentProgram];
        parts.add (in.name + "  " + String (in.currentProgram).paddedLeft ('0', 3)
                     + "  " + (prog.name.isNotEmpty() ? prog.name : "(empty)"));
    }
    parts.add ("Tempo  " + String (engine.getTempoBpm(), 1) + " bpm");
    return parts.joinIntoString ("\n");
}

void SongsPanel::addSong()
{
    /* Ask for the name when the song is made. It used to be named silently
       after whatever Upper was playing -- so every song was called "B3" until
       you found the Rename button, which is not a thing anyone should have to
       find. */
    String suggested;
    const auto& inputs = engine.getSetup().inputs;
    if (! inputs.empty())
    {
        const auto& prog = inputs[0].programs[(size_t) inputs[0].currentProgram];
        if (prog.name.isNotEmpty()) suggested = prog.name;
    }

    auto* w = new AlertWindow ("New song",
                               "The song will remember what is loaded now:\n\n"
                                 + describeCurrent()
                                 + "\n\nChanging a program later changes every song that uses it.",
                               MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", suggested);
    w->getTextEditor ("name")->setTextToShowWhenEmpty ("song name", textDim);
    w->addTextEditor ("notes", {});
    w->getTextEditor ("notes")->setTextToShowWhenEmpty ("stage notes: key, count-in, a reminder", textDim);
    w->addButton ("Create", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w] (int r)
    {
        std::unique_ptr<AlertWindow> owned (w);
        if (r == 0) return;

        auto name = w->getTextEditorContents ("name").trim();
        if (name.isEmpty()) name = "Song " + String ((int) engine.getSongs().size() + 1);

        selectedSong = engine.addSong (name);
        engine.captureSong (selectedSong);      // a new song remembers what is up now
        const auto notes = w->getTextEditorContents ("notes").trim();
        if (notes.isNotEmpty()) engine.setSongNotes (selectedSong, notes);
        refresh();
        songsList.selectRow (selectedSong);
    }), false);
}

void SongsPanel::removeSong()
{
    const auto& songs = engine.getSongs();
    if (selectedSong < 0 || selectedSong >= (int) songs.size()) return;

    const auto name = songs[(size_t) selectedSong].name;
    auto* w = new AlertWindow ("Delete song", "Delete \"" + name + "\"?\n\n"
                              "It will also be taken out of every set that uses it.",
                              MessageBoxIconType::QuestionIcon);
    w->addButton ("Delete", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w, idx = selectedSong] (int r)
    {
        std::unique_ptr<AlertWindow> owned (w);
        if (r == 0) return;
        engine.removeSong (idx);
        selectedSong = -1;
        selectedInSet = -1;
        refresh();
    }), false);
}

void SongsPanel::captureIntoSelectedSong()
{
    if (selectedSong < 0 || selectedSong >= (int) engine.getSongs().size()) return;
    engine.captureSong (selectedSong);
    refresh();
}

void SongsPanel::editSelectedSong()
{
    const auto& songs = engine.getSongs();
    if (selectedSong < 0 || selectedSong >= (int) songs.size()) return;
    const auto& sg = songs[(size_t) selectedSong];

    auto* w = new AlertWindow ("Edit song",
                               "The name, and anything you want to read on the stand.",
                               MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", sg.name);
    w->addTextEditor ("notes", sg.notes);
    w->addTextEditor ("tempo", sg.tempoBpm > 0.0 ? String (sg.tempoBpm, 1) : String());
    w->getTextEditor ("notes")->setTextToShowWhenEmpty ("stage notes: key, count-in, a reminder", textDim);
    w->getTextEditor ("tempo")->setTextToShowWhenEmpty ("tempo, or blank to keep the current one", textDim);
    w->addButton ("OK", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w, idx = selectedSong] (int r)
    {
        std::unique_ptr<AlertWindow> owned (w);
        if (r == 0) return;
        engine.setSongName (idx, w->getTextEditorContents ("name"));
        engine.setSongNotes (idx, w->getTextEditorContents ("notes"));

        // Blank means "leave the tempo alone", which is not the same as 0 bpm.
        const auto typed = w->getTextEditorContents ("tempo").trim();
        engine.setSongTempo (idx, typed.isEmpty() ? 0.0 : typed.getDoubleValue());
        refresh();
    }), false);
}

//==============================================================================
void SongsPanel::addToSet()
{
    const int set = engine.getCurrentSet();
    const auto& sets = engine.getSets();
    if (set < 0 || set >= (int) sets.size()) return;
    if (selectedSong < 0 || selectedSong >= (int) engine.getSongs().size()) return;

    auto songs = sets[(size_t) set].songs;
    songs.push_back (selectedSong);
    engine.setSetSongs (set, songs);
    selectedInSet = (int) songs.size() - 1;
    refresh();
    setList.selectRow (selectedInSet);
}

void SongsPanel::removeFromSet()
{
    const int set = engine.getCurrentSet();
    const auto& sets = engine.getSets();
    if (set < 0 || set >= (int) sets.size()) return;

    auto songs = sets[(size_t) set].songs;
    if (selectedInSet < 0 || selectedInSet >= (int) songs.size()) return;

    songs.erase (songs.begin() + selectedInSet);
    engine.setSetSongs (set, songs);
    selectedInSet = jmin (selectedInSet, (int) songs.size() - 1);
    refresh();
}

void SongsPanel::moveSongInList (int delta)
{
    if (selectedSong < 0 || selectedSong >= (int) engine.getSongs().size()) return;
    selectedSong = engine.moveSong (selectedSong, delta);
    refresh();
    songsList.selectRow (selectedSong);
}

void SongsPanel::moveInSet (int delta)
{
    const int set = engine.getCurrentSet();
    const auto& sets = engine.getSets();
    if (set < 0 || set >= (int) sets.size()) return;

    auto songs = sets[(size_t) set].songs;
    const int to = selectedInSet + delta;
    if (selectedInSet < 0 || to < 0 || to >= (int) songs.size()) return;

    std::swap (songs[(size_t) selectedInSet], songs[(size_t) to]);
    engine.setSetSongs (set, songs);
    selectedInSet = to;
    refresh();
    setList.selectRow (to);
}

//==============================================================================
void SongsPanel::newSet()
{
    auto* w = new AlertWindow ("New set", "What is this set called?", MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", "Set " + String ((int) engine.getSets().size() + 1));
    w->addButton ("Create", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w] (int r)
    {
        std::unique_ptr<AlertWindow> owned (w);
        if (r == 0) return;
        const int idx = engine.addSet (w->getTextEditorContents ("name"));
        engine.selectSet (idx);          // a set you just made is the one you are working on
        selectedInSet = -1;
        refresh();
    }), false);
}

void SongsPanel::showSetMenu()
{
    PopupMenu m;
    const auto& sets = engine.getSets();
    const int current = engine.getCurrentSet();

    if (! sets.empty())
    {
        m.addSectionHeader ("Play");
        for (int i = 0; i < (int) sets.size(); ++i)
            m.addItem (100 + i, sets[(size_t) i].name, true, i == current);
        m.addSeparator();
    }
    m.addItem (1, "New set...");
    if (current >= 0)
    {
        m.addItem (2, "Rename this set...");
        m.addItem (4, "Move up",   current > 0);
        m.addItem (5, "Move down", current < (int) sets.size() - 1);
        m.addItem (3, "Delete this set");
    }

    m.showMenuAsync (PopupMenu::Options().withTargetComponent (setBtn), [this] (int choice)
    {
        if (choice == 0) return;
        if (choice >= 100)
        {
            engine.selectSet (choice - 100);
            selectedInSet = -1;
            refresh();
            return;
        }
        if (choice == 1) { newSet(); return; }

        const int set = engine.getCurrentSet();
        if (set < 0) return;

        if (choice == 2)
        {
            auto* w = new AlertWindow ("Set", "Name", MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", engine.getSets()[(size_t) set].name);
            w->addButton ("OK", 1, KeyPress (KeyPress::returnKey));
            w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
            w->enterModalState (true, ModalCallbackFunction::create ([this, w, set] (int r)
            {
                std::unique_ptr<AlertWindow> owned (w);
                if (r == 0) return;
                engine.setSetName (set, w->getTextEditorContents ("name"));
                refresh();
            }), false);
        }
        else if (choice == 3)
        {
            engine.removeSet (set);
            selectedInSet = -1;
            refresh();
        }
        else if (choice == 4 || choice == 5)
        {
            engine.moveSet (set, choice == 4 ? -1 : 1);
            refresh();
        }
    });
}

void SongsPanel::showFootswitchMenu()
{
    PopupMenu m;
    const int nextCC = engine.getNextSongCC(), prevCC = engine.getPrevSongCC();

    m.addSectionHeader ("Next song");
    m.addItem (1, "Learn: press the pedal...");
    if (nextCC > 0) m.addItem (2, "Stop using CC " + String (nextCC));

    m.addSectionHeader ("Previous song");
    m.addItem (3, "Learn: press the pedal...");
    if (prevCC > 0) m.addItem (4, "Stop using CC " + String (prevCC));

    m.showMenuAsync (PopupMenu::Options().withTargetComponent (footBtn), [this] (int choice)
    {
        switch (choice)
        {
            case 1: engine.armSongLearn (true);  break;
            case 2: engine.setNextSongCC (0);    break;
            case 3: engine.armSongLearn (false); break;
            case 4: engine.setPrevSongCC (0);    break;
            default: break;
        }
        refresh();
    });
}

} // namespace perf
