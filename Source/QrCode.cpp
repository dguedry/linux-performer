#include "QrCode.h"

using namespace juce;

namespace perf
{

//==============================================================================
// Reed-Solomon over GF(256) with the QR primitive polynomial 0x11D.
namespace gf
{
    static uint8_t exp_[512], log_[256];

    static void init()
    {
        static bool done = false;
        if (done) return;
        done = true;
        int x = 1;
        for (int i = 0; i < 255; ++i)
        {
            exp_[i] = (uint8_t) x;
            log_[x] = (uint8_t) i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i) exp_[i] = exp_[i - 255];
    }

    static uint8_t mul (uint8_t a, uint8_t b)
    {
        if (a == 0 || b == 0) return 0;
        return exp_[log_[a] + log_[b]];
    }
}

/** The generator polynomial for `n` error-correction codewords. */
static std::vector<uint8_t> rsGenerator (int n)
{
    std::vector<uint8_t> g { 1 };
    for (int i = 0; i < n; ++i)
    {
        g.push_back (0);
        for (int j = (int) g.size() - 1; j > 0; --j)
            g[(size_t) j] ^= gf::mul (g[(size_t) j - 1], gf::exp_[i]);
    }
    return g;
}

static std::vector<uint8_t> rsRemainder (const std::vector<uint8_t>& data, int ecLen)
{
    const auto gen = rsGenerator (ecLen);
    std::vector<uint8_t> rem ((size_t) ecLen, 0);
    for (auto b : data)
    {
        const uint8_t factor = (uint8_t) (b ^ rem[0]);
        rem.erase (rem.begin());
        rem.push_back (0);
        for (size_t i = 0; i < rem.size(); ++i)
            rem[i] ^= gf::mul (gen[i + 1], factor);
    }
    return rem;
}

//==============================================================================
/* Versions 1-4 at error correction level M. Per version: total data codewords,
   EC codewords per block, and block count. Straight from the QR specification's
   tables; only the rows we support are listed, so there is nothing here that is
   not exercised. */
struct VersionInfo { int dataCodewords, ecPerBlock, blocks; };
static const VersionInfo kVersions[] =
{
    { 16, 10, 1 },   // version 1-M
    { 28, 16, 1 },   // version 2-M
    { 44, 26, 1 },   // version 3-M
    { 64, 18, 2 },   // version 4-M
};

static int moduleCountFor (int version) { return version * 4 + 17; }

//==============================================================================
bool QrCode::getModule (int x, int y) const noexcept
{
    if (x < 0 || y < 0 || x >= size || y >= size) return false;
    return modules[(size_t) (y * size + x)];
}

void QrCode::setModule (int x, int y, bool on) noexcept
{
    if (x < 0 || y < 0 || x >= size || y >= size) return;
    modules[(size_t) (y * size + x)] = on;
    reserved[(size_t) (y * size + x)] = true;
}

void QrCode::drawFinder (int cx, int cy)
{
    // 7x7 finder plus a 1-module light separator: a 9x9 sweep.
    for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx)
        {
            const int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= size || y >= size) continue;
            const int d = jmax (std::abs (dx), std::abs (dy));
            setModule (x, y, d != 2 && d <= 3);   // ring, gap, ring
        }
}

void QrCode::drawAlignment (int cx, int cy)
{
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            setModule (cx + dx, cy + dy, jmax (std::abs (dx), std::abs (dy)) != 1);
}

void QrCode::drawFunctionPatterns (int version)
{
    // Timing patterns: alternating, spanning the whole symbol.
    for (int i = 0; i < size; ++i)
    {
        setModule (6, i, i % 2 == 0);
        setModule (i, 6, i % 2 == 0);
    }

    drawFinder (3, 3);
    drawFinder (size - 4, 3);
    drawFinder (3, size - 4);

    // Versions 2+ carry one alignment pattern opposite the corner.
    if (version >= 2)
        drawAlignment (size - 7, size - 7);

    /* Reserve the format areas; the bits themselves go in later. Row and column
       6 are the timing patterns and are NOT part of the format area: writing
       them here overwrites a dark timing module with light and breaks the
       alternation a scanner relies on to lay its grid over the symbol. */
    for (int i = 0; i < 9; ++i)
    {
        if (i != 6) setModule (8, i, false);
        if (i != 6) setModule (i, 8, false);
    }
    for (int i = 0; i < 8; ++i)
    {
        setModule (8, size - 1 - i, false);
        setModule (size - 1 - i, 8, false);
    }
    setModule (8, size - 8, true);    // always dark
}

void QrCode::placeFormatBits (int mask)
{
    // Level M is 00; 15-bit BCH with the standard 0x5412 final XOR.
    const int fmtData = (0b00 << 3) | mask;
    int rem = fmtData;
    for (int i = 0; i < 10; ++i)
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = ((fmtData << 10) | rem) ^ 0x5412;

    auto bit = [bits] (int i) { return ((bits >> i) & 1) != 0; };

    for (int i = 0; i <= 5; ++i)      setModule (8, i, bit (i));
    setModule (8, 7, bit (6));
    setModule (8, 8, bit (7));
    setModule (7, 8, bit (8));
    for (int i = 9; i < 15; ++i)      setModule (14 - i, 8, bit (i));

    for (int i = 0; i < 8; ++i)       setModule (size - 1 - i, 8, bit (i));
    for (int i = 8; i < 15; ++i)      setModule (8, size - 15 + i, bit (i));
    setModule (8, size - 8, true);
}

void QrCode::placeData (const std::vector<uint8_t>& data)
{
    size_t bitIndex = 0;
    // Two-module-wide columns, right to left, snaking up then down.
    for (int right = size - 1; right >= 1; right -= 2)
    {
        if (right == 6) right = 5;    // skip the vertical timing pattern
        for (int v = 0; v < size; ++v)
            for (int j = 0; j < 2; ++j)
            {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? size - 1 - v : v;
                if (reserved[(size_t) (y * size + x)]) continue;

                bool on = false;
                if (bitIndex < data.size() * 8)
                    on = ((data[bitIndex >> 3] >> (7 - (bitIndex & 7))) & 1) != 0;
                ++bitIndex;
                modules[(size_t) (y * size + x)] = on;
            }
    }
}

//==============================================================================
long QrCode::penalty() const
{
    long p = 0;

    // Rule 1: runs of five or more identical modules, scored 3 for the fifth and
    // 1 for each after it, in every row and every column.
    for (int i = 0; i < size; ++i)
        for (int pass = 0; pass < 2; ++pass)
        {
            int run = 1;
            bool prev = pass == 0 ? getModule (0, i) : getModule (i, 0);
            for (int j = 1; j < size; ++j)
            {
                const bool c = pass == 0 ? getModule (j, i) : getModule (i, j);
                if (c == prev)
                {
                    ++run;
                    if (run == 5) p += 3;
                    else if (run > 5) p += 1;
                }
                else { run = 1; prev = c; }
            }
        }

    // 2x2 blocks of one colour.
    for (int y = 0; y < size - 1; ++y)
        for (int x = 0; x < size - 1; ++x)
        {
            const bool c = getModule (x, y);
            if (c == getModule (x + 1, y) && c == getModule (x, y + 1) && c == getModule (x + 1, y + 1))
                p += 3;
        }

    /* Rule 3: the 11-module sequence 1011101 0000 (or its mirror), which imitates
       a finder pattern closely enough to make a scanner mis-locate the symbol.
       Modules past the edge count as light, per the specification -- treating
       them as "skip" instead is what over-counts. */
    const bool wanted[11] = { true, false, true, true, true, false, true, false, false, false, false };
    for (int i = 0; i < size; ++i)
        for (int j = 0; j + 11 <= size; ++j)
            for (int pass = 0; pass < 2; ++pass)
            {
                bool forward = true, backward = true;
                for (int k = 0; k < 11; ++k)
                {
                    const bool m = pass == 0 ? getModule (j + k, i) : getModule (i, j + k);
                    if (m != wanted[k])      forward = false;
                    if (m != wanted[10 - k]) backward = false;
                }
                if (forward || backward) p += 40;
            }

    // Overall imbalance between dark and light.
    // Rule 4: 10 points for every 5% the dark/light balance strays from even.
    int dark = 0;
    for (int i = 0; i < size * size; ++i) if (modules[(size_t) i]) ++dark;
    const int total = size * size;
    const int k = (std::abs (dark * 20 - total * 10) + total - 1) / total;   // ceil
    p += 10 * k;
    return p;
}

int QrCode::applyBestMask()
{
    const auto dataOnly = modules;
    int best = 0;
    long bestPenalty = std::numeric_limits<long>::max();

    for (int mask = 0; mask < 8; ++mask)
    {
        modules = dataOnly;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                if (reserved[(size_t) (y * size + x)]) continue;
                bool invert = false;
                switch (mask)
                {
                    case 0: invert = (x + y) % 2 == 0; break;
                    case 1: invert = y % 2 == 0; break;
                    case 2: invert = x % 3 == 0; break;
                    case 3: invert = (x + y) % 3 == 0; break;
                    case 4: invert = ((y / 2) + (x / 3)) % 2 == 0; break;
                    case 5: invert = (x * y) % 2 + (x * y) % 3 == 0; break;
                    case 6: invert = ((x * y) % 2 + (x * y) % 3) % 2 == 0; break;
                    case 7: invert = ((x + y) % 2 + (x * y) % 3) % 2 == 0; break;
                    default: break;
                }
                if (invert) modules[(size_t) (y * size + x)] = ! modules[(size_t) (y * size + x)];
            }
        placeFormatBits (mask);
        if (const auto p = penalty(); p < bestPenalty) { bestPenalty = p; best = mask; }
    }

    // Re-apply the winner, since the loop left the last mask in place.
    modules = dataOnly;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            if (reserved[(size_t) (y * size + x)]) continue;
            bool invert = false;
            switch (best)
            {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                case 4: invert = ((y / 2) + (x / 3)) % 2 == 0; break;
                case 5: invert = (x * y) % 2 + (x * y) % 3 == 0; break;
                case 6: invert = ((x * y) % 2 + (x * y) % 3) % 2 == 0; break;
                case 7: invert = ((x + y) % 2 + (x * y) % 3) % 2 == 0; break;
                default: break;
            }
            if (invert) modules[(size_t) (y * size + x)] = ! modules[(size_t) (y * size + x)];
        }
    placeFormatBits (best);
    return best;
}

//==============================================================================
QrCode QrCode::encode (const String& text)
{
    gf::init();
    QrCode q;

    const auto utf8 = text.toRawUTF8();
    const int len = (int) strlen (utf8);

    // Smallest version that holds the text: byte mode costs 4 bits for the mode
    // indicator plus 8 for the length (versions 1-9), then 8 bits per character.
    int version = 0;
    const VersionInfo* info = nullptr;
    for (int v = 1; v <= 4; ++v)
    {
        const auto& vi = kVersions[v - 1];
        if ((4 + 8 + len * 8 + 7) / 8 <= vi.dataCodewords) { version = v; info = &vi; break; }
    }
    if (info == nullptr) return q;     // too long: caller shows text instead

    // ---- bit stream ----------------------------------------------------------
    std::vector<uint8_t> bits;
    size_t nbits = 0;
    auto push = [&bits, &nbits] (int value, int count)
    {
        for (int i = count - 1; i >= 0; --i)
        {
            if ((nbits & 7) == 0) bits.push_back (0);
            if (((value >> i) & 1) != 0) bits[nbits >> 3] |= (uint8_t) (1 << (7 - (nbits & 7)));
            ++nbits;
        }
    };

    push (0b0100, 4);                  // byte mode
    push (len, 8);
    for (int i = 0; i < len; ++i) push ((uint8_t) utf8[i], 8);

    const int capacityBits = info->dataCodewords * 8;
    push (0, jmin (4, capacityBits - (int) nbits));          // terminator
    while ((nbits & 7) != 0) push (0, 1);                    // pad to a byte

    /* Alternating pad bytes fill the rest. These go through push() like
       everything else: appending to `bits` directly would leave nbits stale, and
       the two disagreeing about the length silently corrupts the stream from the
       first pad byte onwards. */
    for (bool alt = false; (int) bits.size() < info->dataCodewords; alt = ! alt)
        push (alt ? 0x11 : 0xEC, 8);

    // ---- error correction, interleaved per block -----------------------------
    const int blocks = info->blocks;
    const int shortLen = info->dataCodewords / blocks;
    const int longCount = info->dataCodewords % blocks;

    std::vector<std::vector<uint8_t>> dataBlocks, ecBlocks;
    int offset = 0;
    for (int b = 0; b < blocks; ++b)
    {
        const int thisLen = shortLen + (b >= blocks - longCount ? 1 : 0);
        std::vector<uint8_t> d (bits.begin() + offset, bits.begin() + offset + thisLen);
        offset += thisLen;
        ecBlocks.push_back (rsRemainder (d, info->ecPerBlock));
        dataBlocks.push_back (std::move (d));
    }

    std::vector<uint8_t> final_;
    for (int i = 0; ; ++i)
    {
        bool any = false;
        for (auto& d : dataBlocks)
            if (i < (int) d.size()) { final_.push_back (d[(size_t) i]); any = true; }
        if (! any) break;
    }
    for (int i = 0; i < info->ecPerBlock; ++i)
        for (auto& e : ecBlocks)
            final_.push_back (e[(size_t) i]);

    // ---- lay it out ----------------------------------------------------------
    q.size = moduleCountFor (version);
    q.modules.assign ((size_t) (q.size * q.size), false);
    q.reserved.assign ((size_t) (q.size * q.size), false);
    q.drawFunctionPatterns (version);
    q.placeData (final_);
    q.applyBestMask();
    return q;
}

//==============================================================================
void QrCode::draw (Graphics& g, Rectangle<int> area, Colour dark, Colour light) const
{
    if (! isValid()) return;

    // Whole pixels per module, or the code blurs and scans slowly. Four modules
    // of quiet border are mandatory, not decoration: scanners need it to find
    // the symbol's edge.
    const int quiet = 4;
    const int total = size + quiet * 2;
    const int scale = jmax (1, jmin (area.getWidth(), area.getHeight()) / total);
    const int side = scale * total;

    const auto origin = area.withSizeKeepingCentre (side, side).getTopLeft();

    g.setColour (light);
    g.fillRect (origin.x, origin.y, side, side);

    g.setColour (dark);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (getModule (x, y))
                g.fillRect (origin.x + (x + quiet) * scale,
                            origin.y + (y + quiet) * scale,
                            scale, scale);
}

//==============================================================================
String QrCode::toSvg (int sizeMm) const
{
    if (! isValid()) return {};

    const int quiet = 4;
    const int total = size + quiet * 2;

    // One path of rectangles: far smaller than an element per module, and every
    // renderer handles it identically.
    String path;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (getModule (x, y))
                path << "M" << (x + quiet) << " " << (y + quiet) << "h1v1h-1z";

    String svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << sizeMm << "mm\" height=\"" << sizeMm
        << "mm\" viewBox=\"0 0 " << total << " " << total << "\" shape-rendering=\"crispEdges\">"
        << "<rect width=\"" << total << "\" height=\"" << total << "\" fill=\"#fff\"/>"
        << "<path d=\"" << path << "\" fill=\"#000\"/></svg>";
    return svg;
}

} // namespace perf
