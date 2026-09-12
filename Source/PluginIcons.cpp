#include "PluginIcons.h"

using namespace juce;

namespace perf
{

PluginIcons::PluginIcons (const File& dir) : cacheDir (dir) {}

//==============================================================================
Image PluginIcons::get (const PluginDescription& d, int size)
{
    auto img = native (d);
    if (img.isValid())
        return img.rescaled (size, size, Graphics::highResamplingQuality);
    return badge (d, size);
}

Image PluginIcons::native (const PluginDescription& d)
{
    const auto key = d.createIdentifierString();
    if (const auto it = memo.find (key); it != memo.end())
        return it->second;

    Image img;
    const File cached = cacheDir.getChildFile (String::toHexString (key.hashCode64()) + ".png");
    if (cached.existsAsFile())
        img = ImageFileFormat::loadFrom (cached);

    if (! img.isValid())
    {
        if (const auto pe = windowsBinaryFor (d); pe.existsAsFile())
            img = fromWindowsBinary (pe);
        if (! img.isValid() && d.pluginFormatName == "VST3")
            img = fromVst3Snapshot (File (d.fileOrIdentifier));
        if (img.isValid())
        {
            cacheDir.createDirectory();
            FileOutputStream out (cached);
            if (out.openedOk()) { out.setPosition (0); out.truncate(); PNGImageFormat().writeImageToStream (img, out); }
        }
    }
    memo[key] = img;
    return img;
}

//==============================================================================
String PluginIcons::initials (const String& name)
{
    StringArray words;
    words.addTokens (name.trim(), " -_./", "");
    words.removeEmptyStrings();
    String out;
    for (auto& w : words)
    {
        if (w.equalsIgnoreCase ("the") || w.equalsIgnoreCase ("of")) continue;
        out << w.substring (0, 1).toUpperCase();
        if (out.length() == 2) break;
    }
    if (out.isEmpty()) out = "?";
    // "Kontakt 8" -> "K8" rather than "K": keep a trailing number
    if (out.length() == 1 && words.size() > 1 && words[words.size() - 1].containsOnly ("0123456789"))
        out << words[words.size() - 1].substring (0, 1);
    return out;
}

Colour PluginIcons::badgeColour (const String& manufacturer)
{
    const auto h = (uint32) manufacturer.trim().toLowerCase().hashCode();
    const float hue = (float) (h % 360) / 360.0f;
    return Colour::fromHSV (hue, 0.55f, 0.62f, 1.0f);
}

Image PluginIcons::badge (const PluginDescription& d, int size)
{
    Image img (Image::ARGB, size, size, true);
    Graphics g (img);
    const auto r = img.getBounds().toFloat();
    const auto base = badgeColour (d.manufacturerName.isNotEmpty() ? d.manufacturerName : d.name);
    g.setGradientFill (ColourGradient (base.brighter (0.25f), 0.0f, 0.0f, base.darker (0.35f), 0.0f, r.getHeight(), false));
    g.fillRoundedRectangle (r, size * 0.2f);
    g.setColour (Colours::white.withAlpha (0.92f));
    const auto text = initials (d.name);
    g.setFont (FontOptions (size * (text.length() > 1 ? 0.46f : 0.58f), Font::bold));
    g.drawText (text, img.getBounds(), Justification::centred, false);
    return img;
}

//==============================================================================
File PluginIcons::windowsBinaryFor (const PluginDescription& d)
{
    File f (d.fileOrIdentifier);
    if (! f.isDirectory()) return {};
    for (const auto& link : f.findChildFiles (File::findFiles, true))
        if (link.isSymbolicLink())
        {
            const auto target = link.getLinkedTarget();
            const auto ext = target.getFileExtension().toLowerCase();
            if (target.existsAsFile() && (ext == ".vst3" || ext == ".dll" || ext == ".clap"))
                return target;
        }
    return {};
}

namespace
{
    struct PeReader
    {
        explicit PeReader (const File& f) : in (f) {}
        bool ok() const { return in.openedOk(); }

        uint32 u32 (int64 pos) { in.setPosition (pos); return (uint32) in.readInt(); }
        uint16 u16 (int64 pos) { in.setPosition (pos); return (uint16) in.readShort(); }
        MemoryBlock bytes (int64 pos, size_t n) { MemoryBlock b; in.setPosition (pos); in.readIntoMemoryBlock (b, (ssize_t) n); return b; }

        /** Locates the resource directory; false for anything that is not a PE with resources. */
        bool parseHeaders()
        {
            if (! ok() || in.getTotalLength() < 0x40 || u16 (0) != 0x5a4d) return false;          // "MZ"
            const int64 pe = u32 (0x3c);
            if (pe <= 0 || pe > in.getTotalLength() - 24 || u32 (pe) != 0x00004550) return false;   // "PE\0\0"
            const int numSections = u16 (pe + 6);
            const int optSize     = u16 (pe + 20);
            const int64 opt = pe + 24;
            const uint16 magic = u16 (opt);
            const int64 dataDirs = opt + (magic == 0x20b ? 112 : 96);
            if (optSize < (magic == 0x20b ? 112 : 96) + 3 * 8) return false;
            rsrcRva  = u32 (dataDirs + 2 * 8);
            rsrcSize = u32 (dataDirs + 2 * 8 + 4);
            if (rsrcRva == 0 || rsrcSize == 0) return false;
            const int64 sec = opt + optSize;
            for (int i = 0; i < numSections && i < 96; ++i)
            {
                const int64 s = sec + i * 40;
                sections.push_back ({ u32 (s + 12), u32 (s + 16), u32 (s + 20) });
            }
            return rvaToOffset (rsrcRva) >= 0;
        }

        int64 rvaToOffset (uint32 rva) const
        {
            for (auto& s : sections)
                if (rva >= s.va && rva < s.va + jmax (s.rawSize, 1u))
                    return (int64) s.rawPtr + (rva - s.va);
            return -1;
        }

        struct Entry { uint32 id; bool isDir; uint32 offset; };   // offset relative to the resource directory
        std::vector<Entry> entries (uint32 dirOffset)
        {
            std::vector<Entry> out;
            const int64 base = rvaToOffset (rsrcRva);
            if (base < 0) return out;
            const int64 d = base + dirOffset;
            const int n = u16 (d + 12) + u16 (d + 14);
            for (int i = 0; i < n && i < 4096; ++i)
            {
                const uint32 name = u32 (d + 16 + i * 8), data = u32 (d + 16 + i * 8 + 4);
                out.push_back ({ name & 0x7fffffffu, (data & 0x80000000u) != 0, data & 0x7fffffffu });
            }
            return out;
        }

        /** Resource data for type/id (first language), as raw bytes. */
        MemoryBlock resource (uint32 type, uint32 id)
        {
            for (auto& t : entries (0))
                if (t.id == type && t.isDir)
                    for (auto& n : entries (t.offset))
                        if (n.id == id && n.isDir)
                            for (auto& lang : entries (n.offset))
                                if (! lang.isDir)
                                {
                                    const int64 base = rvaToOffset (rsrcRva);
                                    const int64 e = base + lang.offset;
                                    const uint32 dataRva = u32 (e), size = u32 (e + 4);
                                    const int64 off = rvaToOffset (dataRva);
                                    if (off >= 0 && size > 0 && size < 64 * 1024 * 1024) return bytes (off, size);
                                    return {};
                                }
            return {};
        }

        /** IDs of all RT_GROUP_ICON resources. */
        std::vector<uint32> groupIconIds()
        {
            std::vector<uint32> ids;
            for (auto& t : entries (0))
                if (t.id == 14 && t.isDir)
                    for (auto& n : entries (t.offset)) ids.push_back (n.id);
            return ids;
        }

        struct Section { uint32 va, rawSize, rawPtr; };
        FileInputStream in;
        uint32 rsrcRva = 0, rsrcSize = 0;
        std::vector<Section> sections;
    };

    Image decodeIconData (const MemoryBlock& data)
    {
        if (data.getSize() > 8 && std::memcmp (data.getData(), "\x89PNG", 4) == 0)
            return PNGImageFormat().loadFrom (data.getData(), data.getSize());

        // A DIB without file header: BITMAPINFOHEADER, XOR pixels (bottom-up), then the AND mask.
        if (data.getSize() < 40) return {};
        const auto* p = static_cast<const uint8*> (data.getData());
        auto rd32 = [p] (size_t o) { return (int32) (p[o] | (p[o+1] << 8) | (p[o+2] << 16) | ((uint32) p[o+3] << 24)); };
        auto rd16 = [p] (size_t o) { return (uint16) (p[o] | (p[o+1] << 8)); };
        const int w = rd32 (4), h2 = rd32 (8), bpp = rd16 (14);
        const int h = h2 / 2;
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || bpp != 32) return {};
        const size_t need = 40 + (size_t) w * (size_t) h * 4;
        if (data.getSize() < need) return {};
        Image img (Image::ARGB, w, h, true);
        Image::BitmapData bd (img, Image::BitmapData::writeOnly);
        for (int y = 0; y < h; ++y)
        {
            const uint8* row = p + 40 + (size_t) (h - 1 - y) * (size_t) w * 4;
            for (int x = 0; x < w; ++x)
            {
                const uint8 b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2], a = row[x * 4 + 3];
                bd.setPixelColour (x, y, Colour (r, g, b, a));
            }
        }
        return img;
    }
}

Image PluginIcons::fromWindowsBinary (const File& pe)
{
    PeReader reader (pe);
    if (! reader.parseHeaders()) return {};

    Image best;
    for (auto groupId : reader.groupIconIds())
    {
        const auto group = reader.resource (14, groupId);
        if (group.getSize() < 6) continue;
        const auto* g = static_cast<const uint8*> (group.getData());
        const int count = g[4] | (g[5] << 8);
        // Prefer the largest entry up to 256 px; PNG entries decode for any depth, DIBs need 32 bpp.
        int bestScore = -1; uint32 bestId = 0;
        for (int i = 0; i < count && 6 + (i + 1) * 14 <= (int) group.getSize(); ++i)
        {
            const auto* e = g + 6 + i * 14;
            const int width = e[0] == 0 ? 256 : e[0];
            const int bits  = e[6] | (e[7] << 8);
            const uint32 id = (uint32) (e[12] | (e[13] << 8));
            if (width > 256) continue;
            const int score = width * 10 + (bits >= 32 ? 1 : 0);
            if (score > bestScore) { bestScore = score; bestId = id; }
        }
        if (bestScore < 0) continue;
        auto img = decodeIconData (reader.resource (3, bestId));
        if (img.isValid() && img.getWidth() > best.getWidth())
            best = img;
        if (best.isValid()) break;      // the first icon group is the application's
    }
    return best;
}

Image PluginIcons::fromVst3Snapshot (const File& bundle)
{
    const auto dir = bundle.getChildFile ("Contents").getChildFile ("Resources").getChildFile ("Snapshots");
    if (! dir.isDirectory()) return {};
    File pick;
    for (const auto& f : dir.findChildFiles (File::findFiles, false, "*.png"))
        if (pick == File() || (! f.getFileName().contains ("_2.0x") && pick.getFileName().contains ("_2.0x")))
            pick = f;
    if (pick == File()) return {};
    auto shot = ImageFileFormat::loadFrom (pick);
    if (! shot.isValid()) return {};
    // Snapshots are GUI screenshots, usually wide: take a centred square from them.
    const int side = jmin (shot.getWidth(), shot.getHeight());
    return shot.getClippedImage ({ (shot.getWidth() - side) / 2, (shot.getHeight() - side) / 2, side, side }).createCopy();
}

} // namespace perf
