#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace perf
{

/** The user manual, inside the app.

    The manual is embedded from docs/MANUAL.md, so there is one source of truth
    and the help can never drift from the file in the repository. That matters
    most for the parts people need on stage -- a phone that will not connect, a
    venue with no wifi -- where going and finding a web page is exactly what
    nobody can do at that moment.

    Just enough Markdown to render our own manual faithfully: headings, bold,
    inline code, fenced code blocks, bullet and numbered lists, and images. Not
    a general Markdown renderer, and it does not pretend to be one. */
class HelpWindow
{
public:
    /** Opens the manual, or brings the existing window to the front. `section`
        optionally scrolls to a heading, matched loosely, so a button can send
        someone straight to the part that answers their question. */
    static void show (const juce::String& section = {});

    /** Closes it, if open. */
    static void close();
};

} // namespace perf
