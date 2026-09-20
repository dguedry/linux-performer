#pragma once
#include <juce_graphics/juce_graphics.h>

namespace perf
{

/** A small QR encoder, so a tablet can reach Performer without anyone typing.

    Written out rather than pulled in because this has to work on a stage with no
    network and no extra packages installed, and because the job is narrow: we
    encode one short "http://host:port/" URL. Versions 1 to 4 (21 to 33 modules)
    at medium error correction, byte mode -- ample for any address, and medium
    tolerates a fingerprint on the screen or a badly lit printed sheet.

    Not a general QR library: no kanji, no numeric compaction, no versions above
    4. If it is handed something too long it says so rather than guessing. */
class QrCode
{
public:
    /** Encodes text, or returns an invalid code (size 0) if it will not fit. */
    static QrCode encode (const juce::String& text);

    bool isValid() const noexcept          { return size > 0; }
    int getSize() const noexcept           { return size; }
    bool getModule (int x, int y) const noexcept;

    /** Draws the code to fill the given area, snapped to whole pixels per module
        so it never blurs: a half-pixel module edge is what makes a code scan
        slowly or not at all. Includes the mandatory 4-module quiet border. */
    void draw (juce::Graphics&, juce::Rectangle<int> area,
               juce::Colour dark = juce::Colours::black,
               juce::Colour light = juce::Colours::white) const;

    /** The code as a standalone SVG element, for the printed program map. Vector
        rather than a bitmap so it stays sharp at any paper size, and inline so
        the sheet is still a single file you can email. */
    juce::String toSvg (int sizeMm) const;

private:
    int size = 0;
    std::vector<bool> modules, reserved;

    void setModule (int x, int y, bool on) noexcept;
    void drawFunctionPatterns (int version);
    void drawFinder (int cx, int cy);
    void drawAlignment (int cx, int cy);
    void placeFormatBits (int mask);
    void placeData (const std::vector<uint8_t>& data);
    int applyBestMask();
    long penalty() const;
};

} // namespace perf
