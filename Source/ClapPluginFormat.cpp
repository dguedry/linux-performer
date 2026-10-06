#include "ClapPluginFormat.h"

#include <clap/clap.h>
#include <dlfcn.h>

namespace perf
{

using namespace juce;

namespace
{
    /* "<path>|<clap plugin id>". A pipe because a CLAP id is a reverse-DNS
       string ("com.u-he.diva") and a path is a path: neither contains one. */
    constexpr char kIdSeparator = '|';

    /** A loaded .clap bundle: the shared object plus its entry point.

        Closing the library runs the plugin's deinit, so this is kept alive for
        exactly as long as it is being used and no longer. Scanning loads and
        unloads around each bundle; an instance will hold one open for its
        lifetime. */
    class Bundle
    {
    public:
        static std::unique_ptr<Bundle> open (const File& file)
        {
            if (! file.exists()) return {};

            /* RTLD_LOCAL so one plugin's symbols cannot satisfy another's
               undefined ones -- two bundles built against different versions of
               the same library would otherwise resolve to whichever loaded
               first, and crash in ways that look like the second plugin's
               fault. */
            auto* handle = ::dlopen (file.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
            if (handle == nullptr) return {};

            auto* entry = reinterpret_cast<const clap_plugin_entry_t*> (::dlsym (handle, "clap_entry"));
            if (entry == nullptr || entry->init == nullptr || entry->get_factory == nullptr)
            {
                ::dlclose (handle);
                return {};
            }

            // Not a CLAP we understand: refuse rather than guess at its ABI.
            if (! clap_version_is_compatible (entry->clap_version))
            {
                ::dlclose (handle);
                return {};
            }

            if (! entry->init (file.getFullPathName().toRawUTF8()))
            {
                ::dlclose (handle);
                return {};
            }

            return std::unique_ptr<Bundle> (new Bundle (handle, entry));
        }

        ~Bundle()
        {
            if (entry != nullptr && entry->deinit != nullptr) entry->deinit();
            if (handle != nullptr) ::dlclose (handle);
        }

        const clap_plugin_factory_t* factory() const
        {
            if (entry == nullptr) return nullptr;
            return static_cast<const clap_plugin_factory_t*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
        }

    private:
        Bundle (void* h, const clap_plugin_entry_t* e) : handle (h), entry (e) {}

        void* handle = nullptr;
        const clap_plugin_entry_t* entry = nullptr;

        JUCE_DECLARE_NON_COPYABLE (Bundle)
    };

    /** Fills in a JUCE description from a CLAP descriptor. */
    PluginDescription describe (const clap_plugin_descriptor_t& d, const File& file)
    {
        PluginDescription desc;
        desc.pluginFormatName = ClapPluginFormat::getFormatName();
        desc.name             = String::fromUTF8 (d.name != nullptr ? d.name : "");
        desc.descriptiveName  = String::fromUTF8 (d.description != nullptr ? d.description : "");
        desc.manufacturerName = String::fromUTF8 (d.vendor != nullptr ? d.vendor : "");
        desc.version          = String::fromUTF8 (d.version != nullptr ? d.version : "");
        desc.fileOrIdentifier = ClapPluginFormat::makeIdentifier (file.getFullPathName(),
                                                                  String::fromUTF8 (d.id != nullptr ? d.id : ""));
        desc.lastFileModTime  = file.getLastModificationTime();
        desc.lastInfoUpdateTime = Time::getCurrentTime();

        if (desc.name.isEmpty()) desc.name = file.getFileNameWithoutExtension();

        /* A CLAP says what it is in a null-terminated list of feature strings.
           "instrument" is the one that decides whether it can be a slot's
           instrument or only an effect. */
        bool isInstrument = false;
        StringArray features;
        if (d.features != nullptr)
            for (const char* const* f = d.features; *f != nullptr; ++f)
            {
                const String feature (String::fromUTF8 (*f));
                features.add (feature);
                if (feature == CLAP_PLUGIN_FEATURE_INSTRUMENT
                    || feature == CLAP_PLUGIN_FEATURE_SYNTHESIZER)
                    isInstrument = true;
            }

        desc.isInstrument = isInstrument;
        desc.category     = features.joinIntoString (", ");

        /* A plausible channel count for the list. CLAP reports its real port
           layout only once instantiated, so these are corrected when it is
           actually loaded rather than guessed at harder here. */
        desc.numInputChannels  = isInstrument ? 0 : 2;
        desc.numOutputChannels = 2;

        /* The id is what identifies a plugin across machines, so it is what the
           unique id is derived from -- a path would break the moment someone
           installed to a different prefix. */
        desc.uniqueId = desc.deprecatedUid = (int) String::fromUTF8 (d.id != nullptr ? d.id : "").hashCode();
        return desc;
    }
}

//==============================================================================
ClapPluginFormat::ClapPluginFormat() = default;
ClapPluginFormat::~ClapPluginFormat() = default;

String ClapPluginFormat::makeIdentifier (const String& path, const String& pluginId)
{
    return pluginId.isEmpty() ? path : path + String::charToString (kIdSeparator) + pluginId;
}

String ClapPluginFormat::pathFromIdentifier (const String& identifier)
{
    return identifier.upToFirstOccurrenceOf (String::charToString (kIdSeparator), false, false);
}

String ClapPluginFormat::pluginIdFromIdentifier (const String& identifier)
{
    return identifier.fromFirstOccurrenceOf (String::charToString (kIdSeparator), false, false);
}

//==============================================================================
void ClapPluginFormat::findAllTypesForFile (OwnedArray<PluginDescription>& results,
                                            const String& fileOrIdentifier)
{
    const File file (pathFromIdentifier (fileOrIdentifier));
    if (! fileMightContainThisPluginType (file.getFullPathName())) return;

    auto bundle = Bundle::open (file);
    if (bundle == nullptr) return;

    const auto* factory = bundle->factory();
    if (factory == nullptr || factory->get_plugin_count == nullptr
        || factory->get_plugin_descriptor == nullptr)
        return;

    /* One bundle, possibly several plugins: Surge XT ships the synth and its
       effects in one file. Each gets its own description. */
    const auto wanted = pluginIdFromIdentifier (fileOrIdentifier);
    const uint32_t count = factory->get_plugin_count (factory);

    for (uint32_t i = 0; i < count; ++i)
    {
        const auto* d = factory->get_plugin_descriptor (factory, i);
        if (d == nullptr || d->id == nullptr) continue;

        // Asked for one in particular (a rescan of a known plugin): skip the rest.
        if (wanted.isNotEmpty() && String::fromUTF8 (d->id) != wanted) continue;

        results.add (new PluginDescription (describe (*d, file)));
    }
}

bool ClapPluginFormat::fileMightContainThisPluginType (const String& fileOrIdentifier)
{
    const File f (pathFromIdentifier (fileOrIdentifier));
    return f.hasFileExtension ("clap") && f.existsAsFile();
}

String ClapPluginFormat::getNameOfPluginFromIdentifier (const String& fileOrIdentifier)
{
    return File (pathFromIdentifier (fileOrIdentifier)).getFileNameWithoutExtension();
}

bool ClapPluginFormat::pluginNeedsRescanning (const PluginDescription& desc)
{
    return File (pathFromIdentifier (desc.fileOrIdentifier)).getLastModificationTime()
             != desc.lastFileModTime;
}

bool ClapPluginFormat::doesPluginStillExist (const PluginDescription& desc)
{
    return File (pathFromIdentifier (desc.fileOrIdentifier)).existsAsFile();
}

StringArray ClapPluginFormat::searchPathsForPlugins (const FileSearchPath& path, bool recursive, bool)
{
    StringArray found;
    for (int i = 0; i < path.getNumPaths(); ++i)
        for (const auto& f : path[i].findChildFiles (File::findFiles, recursive, "*.clap"))
            found.add (f.getFullPathName());

    found.sort (true);
    return found;
}

FileSearchPath ClapPluginFormat::getDefaultLocationsToSearch()
{
    /* The locations the CLAP specification names for Linux, plus the one
       yabridge uses for bridged Windows plugins. */
    FileSearchPath path;
    path.add (File::getSpecialLocation (File::userHomeDirectory).getChildFile (".clap"));
    path.add (File ("/usr/lib/clap"));
    path.add (File ("/usr/local/lib/clap"));
    return path;
}

void ClapPluginFormat::createPluginInstance (const PluginDescription&, double, int,
                                             PluginCreationCallback callback)
{
    /* Stage 1 scans and describes; instantiation comes next. Failing with a
       clear message beats a silent nothing or a crash. */
    if (callback != nullptr)
        callback (nullptr, "CLAP plugins can be scanned but not yet loaded.");
}

} // namespace perf
